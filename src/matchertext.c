/*
** 2026 July 29
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file implements the matchertext syntactic discipline: the rule that
** the ASCII matcher pairs "()", "[]" and "{}" must always match.
**
** Two routines are provided, corresponding to the two algorithms of the
** matchertext development:
**
**   sqlite3MatchertextVerify()   VERIFY.  Decide whether a string is valid
**                                matchertext.  One linear scan.
**   sqlite3MatchertextEnd()      FINDEMBEDEND.  Given the opener of an
**                                embedding, locate its matching closer by
**                                matcher balance rather than by scanning
**                                for a terminator.
**
** Both are host-independent: they inspect only the six matcher characters
** and know nothing about SQL.  Nothing in this file allocates memory, and
** neither routine modifies its input.  That is the point.  A value admitted
** by these checks is admitted by a passive verification, not rewritten by a
** content-modifying transformation, so a bug here rejects data that should
** have embedded rather than admitting a breakout.
**
** UTF-8 is scanned safely byte-by-byte.  Every matcher character is below
** 0x80, while every byte of a multi-byte UTF-8 sequence is at or above
** 0x80, so no part of a multi-byte character can be mistaken for a matcher.
** No decoding step is required, and none is performed.  The caller must
** supply the value in its final, fully decoded form: a check applied to any
** earlier, still-encoded representation is unsound, because such a value can
** acquire an unmatched matcher when a later stage decodes it.
*/
#ifdef SQLITE_ENABLE_MATCHERTEXT
#include "sqliteInt.h"

/*
** The maximum matcher nesting depth accepted.  Input nested more deeply than
** this is rejected.  Rejection is safe: the value does not embed and the
** statement fails to parse, which is the intended failure mode.  Deep nesting
** is vanishingly rare in practice; the deepest matched nesting observed in a
** 469-million-sample corpus of production source code was 500.
*/
#ifndef SQLITE_MAX_MATCHER_DEPTH
# define SQLITE_MAX_MATCHER_DEPTH 1000
#endif

/*
** The open matcher of each pending level is held on a stack of two-bit
** entries, 32 levels to a u64 word.  Level numbering is zero-based, so a
** current depth of d means levels 0 through d-1 are pending and MT_TOP()
** reads level d-1.  Every level is written by MT_PUSH() before it is read.
**
** The arguments of these macros are evaluated more than once and so must be
** free of side effects.
*/
#define MT_NWORD  ((SQLITE_MAX_MATCHER_DEPTH+31)/32)
#define MT_PUSH(A,D,K) A[(D)>>5] = (A[(D)>>5] & ~(((u64)3)<<(((D)&31)*2))) | (((u64)(K))<<(((D)&31)*2))
#define MT_TOP(A,D) ((int)((A[((D)-1)>>5] >> ((((D)-1)&31)*2)) & 3))

/*
** Classify one byte against the matcher pair set.  Returns a positive kind
** 1, 2 or 3 for an opener, the negation of that kind for the corresponding
** closer, and zero for a nonmatcher.  The sign carries open versus close and
** the magnitude carries the pair, so an opener of kind k is closed by, and
** only by, the byte that classifies as -k.
*/
static int mtClass(unsigned int c){
  switch( c ){
    case '(': return  1;
    case ')': return -1;
    case '[': return  2;
    case ']': return -2;
    case '{': return  3;
    case '}': return -3;
    default: return 0;
  }
}

/*
** Scan z[] maintaining the matcher stack.  If n is negative the scan runs to
** the first zero byte, otherwise it runs for exactly n bytes.  The scan also
** stops, short of either limit, at the first closer arriving while no opener
** is pending: that byte is the balance boundary and is not consumed.
**
** Write the number of bytes consumed into *pnUsed.  Return the matcher depth
** reached, which is never negative, or -1 if the input is not matchertext
** because a closer met an opener of the wrong kind or the depth limit was
** exceeded.
**
** A return of zero therefore means the bytes consumed are valid matchertext.
** A positive return means they are valid so far but leave openers pending.
*/
static int mtScan(const unsigned char *z, i64 n, i64 *pnUsed){
  u64 aStk[MT_NWORD];
  i64 i;
  int d = 0;
  int k;

  for(i=0; n<0 ? z[i]!=0 : i<n; i++){
    k = mtClass(z[i]);
    if( k==0 ) continue;
    if( k>0 ){
      if( d>=SQLITE_MAX_MATCHER_DEPTH ){
        *pnUsed = i;
        return -1;
      }
      MT_PUSH(aStk, d, k);
      d++;
    }else{
      if( d==0 ) break;
      if( MT_TOP(aStk, d)!= -k ){
        *pnUsed = i;
        return -1;
      }
      d--;
    }
  }
  *pnUsed = i;
  assert( d>=0 );
  return d;
}

/*
** VERIFY.  Return true if and only if z[0..n-1] is valid matchertext.
**
** The empty string is matchertext.  Zero bytes within the input are ordinary
** nonmatchers, so a value carrying them is handled like any other.
**
** A false return is not an error condition to be repaired.  It reports that
** the value cannot embed verbatim, and the caller must reject it or route it
** through an encoder that escapes its unmatched matchers.
*/
int sqlite3MatchertextVerify(const unsigned char *z, i64 n){
  i64 nUsed;
  assert( n>=0 );
  assert( z!=0 || n==0 );
  /* The scan stops early at an unmatched closer, so requiring that it
  ** consumed every byte is what rejects input such as "a)b". */
  return mtScan(z, n, &nUsed)==0 && nUsed==n;
}

/*
** FINDEMBEDEND.  If z[0] opens an embedding "o m c" in zero-terminated text,
** locate c by matcher balance and return the total length of the embedding,
** so that the embedded value m is the nUsed-2 bytes at z[1] where nUsed is
** the return value.  Return 0 if no embedding ends here, that is if z[0] is
** not an opener at all, if the balance never returns to the level of z[0], if
** a closer of the wrong kind arrives, or if the text ends first.
**
** Deciding whether z[0] opens an embedding is left here rather than asked of
** the caller, so that the matcher pairs stay written down in exactly one
** place.  A caller that had to test for an opener itself would be a second
** copy of the configuration, free to drift from this one.
**
** The scan of m begins one byte past the opener, and the byte that first
** drives the depth below zero is exactly the terminating closer.  This is the
** operational form of the boundary result proved in embed_boundary.lean: for
** valid matchertext m every prefix of m has non-negative depth, m itself
** returns to depth zero, and the following closer takes the depth to -1.  The
** value may therefore hold any character at all, quotes and the host's own
** terminators included, provided only that its matchers balance.
**
** This routine locates the boundary and nothing else.  Whatever encloses the
** embedding, a quote in SQL or a tag in markup, remains the caller's concern,
** which is what keeps this file free of any one host language.
*/
i64 sqlite3MatchertextEnd(const unsigned char *z){
  i64 nUsed;
  int k;

  assert( z!=0 );
  k = mtClass(z[0]);
  if( k<=0 ) return 0;

  /* Anything other than a clean return to depth zero means the value is not
  ** matchertext, or the text ran out with openers still pending. */
  if( mtScan(z+1, -1, &nUsed)!=0 ) return 0;

  /* The boundary byte closes z[0] only if it is of the matching kind.  This
  ** also rejects the case where the text ended, since a zero byte classifies
  ** as a nonmatcher and can never equal -k. */
  if( mtClass(z[1+nUsed])!= -k ) return 0;

  return nUsed+2;
}

#endif /* SQLITE_ENABLE_MATCHERTEXT */
