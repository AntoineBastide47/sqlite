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
** TCL commands that reach the matchertext scanner in src/matchertext.c
** directly, so that VERIFY and FINDEMBEDEND can be tested on their own
** rather than only through whatever the SQL surface happens to exercise.
**
** FINDEMBEDEND is reachable through SQL, since the tokenizer calls it, but
** only for input the tokenizer is willing to hand it.  VERIFY has no SQL
** surface at all: it belongs to the write path, where a caller checks a value
** before placing it in a hole.  Without these commands its behaviour could
** not be tested inside this tree.
**
** Values arrive as byte arrays rather than strings so that zero bytes and
** invalid UTF-8 survive the trip from the test script.
*/
#include "sqliteInt.h"
#include "tclsqlite.h"
#include <string.h>

#ifdef SQLITE_ENABLE_MATCHERTEXT

/*
** Usage:  sqlite3_matchertext_verify VALUE
**
** Return 1 if VALUE is valid matchertext and 0 if it is not.  VALUE is taken
** with an explicit length, so zero bytes within it are ordinary nonmatchers.
*/
static int SQLITE_TCLAPI test_matchertext_verify(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  unsigned char *zVal;
  Tcl_Size nVal;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "VALUE");
    return TCL_ERROR;
  }
  zVal = Tcl_GetByteArrayFromObj(objv[1], &nVal);
  Tcl_SetObjResult(interp,
      Tcl_NewIntObj(sqlite3MatchertextVerify(zVal, (i64)nVal)));
  return TCL_OK;
}

/*
** Usage:  sqlite3_matchertext_end TEXT
**
** Return the length of the embedding that begins at the first byte of TEXT,
** or 0 if none does.  The value embedded is therefore the result minus two
** bytes, starting one byte in.
**
** The scanner reads to a zero byte, matching how the tokenizer calls it, so
** TEXT is copied into a zero-terminated buffer here rather than requiring
** every caller to remember the terminator.
*/
static int SQLITE_TCLAPI test_matchertext_end(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  unsigned char *zVal;
  Tcl_Size nVal;
  unsigned char *zCopy;
  i64 res;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "TEXT");
    return TCL_ERROR;
  }
  zVal = Tcl_GetByteArrayFromObj(objv[1], &nVal);
  zCopy = sqlite3_malloc64((sqlite3_uint64)nVal + 1);
  if( zCopy==0 ){
    Tcl_AppendResult(interp, "out of memory", (char*)0);
    return TCL_ERROR;
  }
  if( nVal>0 ) memcpy(zCopy, zVal, (size_t)nVal);
  zCopy[nVal] = 0;
  res = sqlite3MatchertextEnd(zCopy);
  sqlite3_free(zCopy);

  Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)res));
  return TCL_OK;
}

/*
** Usage:  sqlite3_matchertext_printf FORMAT ?VALUE ...?
**
** Run sqlite3_mprintf() with up to four string arguments, so that the %m
** conversion can be exercised from a test script.  Returns the formatted
** string, or the empty string with a "rejected" prefix when mprintf returns
** NULL, which is what a refused value looks like to a caller.
*/
static int SQLITE_TCLAPI test_matchertext_printf(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *azArg[4] = { 0, 0, 0, 0 };
  char *zOut;
  int i;

  if( objc<2 || objc>6 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FORMAT ?VALUE ...?");
    return TCL_ERROR;
  }
  for(i=2; i<objc; i++){
    azArg[i-2] = Tcl_GetString(objv[i]);
  }
  zOut = sqlite3_mprintf(Tcl_GetString(objv[1]),
                         azArg[0], azArg[1], azArg[2], azArg[3]);
  if( zOut==0 ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj("rejected", -1));
  }else{
    Tcl_SetObjResult(interp, Tcl_NewStringObj(zOut, -1));
    sqlite3_free(zOut);
  }
  return TCL_OK;
}

#endif /* SQLITE_ENABLE_MATCHERTEXT */

/*
** Register the commands above with the TCL interpreter.  With the feature
** disabled there is nothing to register and the scanner is not in the build,
** so the commands are simply absent and the test script skips itself.
*/
int Sqlitetest_matchertext_Init(Tcl_Interp *interp){
#ifdef SQLITE_ENABLE_MATCHERTEXT
  static struct {
    char *zName;
    Tcl_ObjCmdProc *xProc;
  } aObjCmd[] = {
    { "sqlite3_matchertext_verify", test_matchertext_verify },
    { "sqlite3_matchertext_end",    test_matchertext_end    },
    { "sqlite3_matchertext_printf", test_matchertext_printf },
  };
  int i;
  for(i=0; i<(int)(sizeof(aObjCmd)/sizeof(aObjCmd[0])); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, aObjCmd[i].xProc, 0, 0);
  }
#endif
  return TCL_OK;
}
