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
** and know nothing about SQL.  Neither routine allocates memory or modifies
** its input.  That is the point.  A value admitted
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

/* The three routines below are host glue rather than scanner: they read the
** SQLite tokenizer and connection state, so they depend on the full internal
** header.  SQLITE_MATCHERTEXT_SCANNER_ONLY excludes them, which is what lets
** the cross-language scanner test compile this file against a stub header and
** keep the boundary routines self-contained. */
#ifndef SQLITE_MATCHERTEXT_SCANNER_ONLY

/* Verify SQL matcher balance without treating comment text as SQL input. */
int sqlite3MatchertextVerifySql(const unsigned char *z, i64 n){
  u64 aStk[MT_NWORD];
  i64 i = 0;
  int d = 0;

  assert( n>=0 );
  assert( z!=0 || n==0 );
  while( i<n ){
    int j;
    int tokenType;
    int nToken = sqlite3GetToken(z+i, &tokenType);
    if( nToken<=0 || nToken>n-i ) return 0;
    if( tokenType!=TK_COMMENT ){
      for(j=0; j<nToken; j++){
        int k = mtClass(z[i+j]);
        if( k>0 ){
          if( d>=SQLITE_MAX_MATCHER_DEPTH ) return 0;
          MT_PUSH(aStk, d, k);
          d++;
        }else if( k<0 ){
          if( d==0 || MT_TOP(aStk, d)!= -k ) return 0;
          d--;
        }
      }
    }
    i += nToken;
  }
  return d==0;
}

/* Legacy SQL entry points remain available only to SQLite's own parsers. */
int sqlite3MatchertextInternalAllowed(sqlite3 *db){
  Parse *pParse = db->pParse;
  return db->init.busy
      || db->nSqlExec                        /* inside OP_SqlExec: SQLite's own SQL */
      || db->nMatchertextInternal
      || (pParse!=0 && pParse->eParseMode!=PARSE_MODE_NORMAL);
}

/* Set the migration error returned by a disabled legacy SQL entry point. */
int sqlite3MatchertextLegacyError(sqlite3 *db, const char *zReplacement){
  int rc;
  if( !sqlite3SafetyCheckOk(db) ) return SQLITE_MISUSE_BKPT;
  sqlite3_mutex_enter(db->mutex);
  sqlite3ErrorWithMsg(db, SQLITE_MISUSE,
      "matchertext requires %s; pass inputs separately", zReplacement);
  rc = sqlite3ApiExit(db, SQLITE_MISUSE);
  sqlite3_mutex_leave(db->mutex);
  return rc;
}

#endif /* SQLITE_MATCHERTEXT_SCANNER_ONLY */

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

/*
** The escape alphabet used by TOMATCHERTEXT and FROMMATCHERTEXT below.
**
**   \\     a literal backslash
**   \o()   \c()   a literal '(' or ')'
**   \o[]   \c[]   a literal '[' or ']'
**   \o{}   \c{}   a literal '{' or '}'
**
** These are the forms table 1 of the matchertext paper proposes, and the
** letters follow the Unicode character classes: 'o' for Open Punctuation and
** 'c' for Close Punctuation.
**
** Each escape names its pair by carrying that pair, matched.  So an escape is
** itself matchertext, and the escape mechanism obeys the discipline it serves
** rather than sitting outside it.  A scanner counting balance sees \o() go up
** by one and back down, never negative and never leaving anything pending.
**
** Note for the Lean development: to_matchertext.lean derives the output-is-
** matchertext theorem from
**
**   hesc : forall c x, x in esc c -> Nonmatcher Pi x
**
** which asks that an escape hold no matcher and so establishes MT (esc c) by
** the flat rule.  That hypothesis is stronger than the theorem needs.  These
** escapes satisfy MT (esc c) by the nesting rule instead, so generalizing
** hesc to the weaker "forall c, MT Pi (esc c)" would cover them, and esc_mt
** becomes one way of discharging it rather than the only way.
**
** The backslash is the introducer.  It is escaped unconditionally, which is
** what makes the encoding injective: a value that already reads like an
** escape encodes to something that decodes back to itself.
*/
#define MT_ESC '\\'

/* Return the three characters following the introducer for matcher kind k, or
** 0 if k is not a matcher.  The first says open or close, the pair that
** follows names which matcher is meant and keeps the escape balanced. */
static const char *mtEscape(int k){
  switch( k ){
    case  1: return "o()";
    case -1: return "c()";
    case  2: return "o[]";
    case -2: return "c[]";
    case  3: return "o{}";
    case -3: return "c{}";
  }
  return 0;
}

/* The inverse of mtEscape().  Given the three characters after the
** introducer, return the byte they stand for, or 0 if they are not an escape.
** The pair must be a real matcher pair, so "\o(]" is not an escape. */
static int mtUnescape(int c1, int c2, int c3){
  int k = mtClass((unsigned int)c2);
  if( k<=0 || mtClass((unsigned int)c3)!= -k ) return 0;
  if( c1=='o' ) return c2;
  if( c1=='c' ) return c3;
  return 0;
}

/*
** TOMATCHERTEXT.  Encode z[0..n-1] so that the result is valid matchertext,
** whatever the input was.  Return a zero-terminated buffer from
** sqlite3_malloc64(), or 0 if allocation fails, and write its length to
** *pnOut when pnOut is not 0.  The caller frees it with sqlite3_free().
**
** Only unmatched matchers are escaped, and the backslash that introduces an
** escape.  A value whose matchers already balance and which holds no
** backslash therefore passes through byte for byte, which is what keeps the
** common case readable in the SQL text.  Exception: nesting at the depth
** limit exactly gets its deepest pair escaped; see pass one below.
**
** This is the total counterpart of VERIFY.  VERIFY answers whether a value
** may embed as it stands and refuses the ones that may not; this refuses
** nothing, so a free-form field carrying "smile :]" or 'printf("[")' can
** still be placed in a hole.  The price is that the value is no longer
** verbatim, so a reader has to run FROMMATCHERTEXT to get the bytes back.
*/
char *sqlite3MatchertextEncode(const char *z, i64 n, i64 *pnOut){
  unsigned char *aEsc;      /* aEsc[i] is true if z[i] must be escaped */
  char *zOut;
  i64 i, j, nOut;
  int d = 0;
  int k;
  /* Positions of openers not yet matched.  The depth cap below bounds how
  ** many there can be, so this is a fixed size rather than one slot per
  ** input byte.  Positions are i64: an int would truncate past 2 GiB. */
  i64 aStk[SQLITE_MAX_MATCHER_DEPTH];

  assert( n>=0 );
  assert( z!=0 || n==0 );

  if( n==0 ){
    zOut = sqlite3_malloc64(1);
    if( zOut ) zOut[0] = 0;
    if( pnOut ) *pnOut = 0;
    return zOut;
  }

  aEsc = sqlite3_malloc64((sqlite3_uint64)n);
  if( aEsc==0 ) return 0;
  memset(aEsc, 0, (size_t)n);

  /* Pass one: find the matchers that are not matched.  Nesting past
  ** SQLITE_MAX_MATCHER_DEPTH-1 is escaped as if unmatched; escapes carry
  ** one pair of their own, so the output stays within the host's limit. */
  for(i=0; i<n; i++){
    k = mtClass((unsigned char)z[i]);
    if( k==0 ) continue;
    if( k>0 ){
      if( d>=SQLITE_MAX_MATCHER_DEPTH-1 ){
        aEsc[i] = 1;
      }else{
        aStk[d++] = i;
      }
    }else if( d>0 && mtClass((unsigned char)z[aStk[d-1]])== -k ){
      d--;
    }else{
      aEsc[i] = 1;
    }
  }
  while( d>0 ) aEsc[aStk[--d]] = 1;

  /* Pass two: size the result, then fill it.  An escaped matcher costs three
  ** extra bytes and a backslash one. */
  nOut = n;
  for(i=0; i<n; i++){
    if( aEsc[i] ) nOut += 3;
    else if( z[i]==MT_ESC ) nOut += 1;
  }
  zOut = sqlite3_malloc64((sqlite3_uint64)nOut + 1);
  if( zOut==0 ){
    sqlite3_free(aEsc);
    return 0;
  }
  for(i=0, j=0; i<n; i++){
    if( aEsc[i] ){
      const char *zEsc = mtEscape(mtClass((unsigned char)z[i]));
      assert( zEsc!=0 );
      zOut[j++] = MT_ESC;
      zOut[j++] = zEsc[0];
      zOut[j++] = zEsc[1];
      zOut[j++] = zEsc[2];
    }else if( z[i]==MT_ESC ){
      zOut[j++] = MT_ESC;
      zOut[j++] = MT_ESC;
    }else{
      zOut[j++] = z[i];
    }
  }
  assert( j==nOut );
  zOut[j] = 0;

  sqlite3_free(aEsc);
  if( pnOut ) *pnOut = nOut;
  return zOut;
}

/*
** FROMMATCHERTEXT.  Undo TOMATCHERTEXT.  Return a zero-terminated buffer from
** sqlite3_malloc64(), or 0 if allocation fails, and write its length to
** *pnOut when pnOut is not 0.
**
** A backslash that does not introduce one of the escapes above is copied
** through unchanged.  The encoder never produces such a sequence, since it
** doubles every backslash, so this only affects text written by hand, where
** passing it through is friendlier than failing.  The decoder is therefore
** not injective, but it does not need to be: what must hold is that decoding
** an encoded value returns the original, and that is what the round trip
** test asserts.
**
** This routine is a data-fidelity convenience and carries no part of the
** security argument, which rests on the encoder's output being matchertext.
**
** The in-place form is what the parser uses, so that reading a value costs no
** allocation; the allocating form is for callers holding const input.
**
** bFull nonzero is the value alphabet, the inverse of TOMATCHERTEXT: matcher
** escapes decode and \\ collapses.  Zero is the name alphabet for [...]:
** matcher escapes only, a backslash stays an ordinary byte as in stock
** SQLite.
*/
i64 sqlite3MatchertextDecodeInPlace(char *z, i64 n, int bFull){
  i64 i, j;
  int c;

  assert( n>=0 );
  assert( z!=0 || n==0 );

  /* Decoding only ever shortens, so the result can overwrite the input as the
  ** scan advances: j never runs ahead of i. */
  for(i=0, j=0; i<n; ){
    if( bFull && z[i]==MT_ESC && i+1<n && z[i+1]==MT_ESC ){
      z[j++] = MT_ESC;
      i += 2;
    }else if( z[i]==MT_ESC && i+3<n
           && (c = mtUnescape((unsigned char)z[i+1], (unsigned char)z[i+2],
                              (unsigned char)z[i+3]))!=0 ){
      z[j++] = (char)c;
      i += 4;
    }else{
      z[j++] = z[i];
      i++;
    }
    assert( j<=i );
  }
  z[j] = 0;
  return j;
}

/*
** Public interfaces to the two routines above, for an application that
** places a value in a hole itself rather than through the %m conversion.
** A negative n means the input is zero-terminated.
*/
int sqlite3_matchertext_verify(const char *z, sqlite3_int64 n){
#ifdef SQLITE_ENABLE_API_ARMOR
  if( z==0 ) return 0;
#endif
  if( n<0 ) n = (i64)strlen(z);
  return sqlite3MatchertextVerify((const unsigned char*)z, n);
}
char *sqlite3_matchertext_encode(const char *z, sqlite3_int64 n,
                                 sqlite3_int64 *pn){
#ifdef SQLITE_ENABLE_API_ARMOR
  if( z==0 ){ (void)SQLITE_MISUSE_BKPT; return 0; }
#endif
  if( n<0 ) n = (i64)strlen(z);
  return sqlite3MatchertextEncode(z, n, pn);
}
char *sqlite3_matchertext_decode(const char *z, sqlite3_int64 n,
                                 sqlite3_int64 *pn){
  char *zOut;
#ifdef SQLITE_ENABLE_API_ARMOR
  if( z==0 ){ (void)SQLITE_MISUSE_BKPT; return 0; }
#endif
  if( n<0 ) n = (i64)strlen(z);
  zOut = sqlite3_malloc64((sqlite3_uint64)n + 1);
  if( zOut==0 ) return 0;
  if( n>0 ) memcpy(zOut, z, (size_t)n);
  zOut[n] = 0;
  n = sqlite3MatchertextDecodeInPlace(zOut, n, 1);
  if( pn ) *pn = n;
  return zOut;
}

#endif /* SQLITE_ENABLE_MATCHERTEXT */
