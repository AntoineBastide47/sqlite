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

extern int getDbPointer(Tcl_Interp*, const char*, sqlite3**);

/* Usage: sqlite3_matchertext_test_bypass BOOLEAN */
static int SQLITE_TCLAPI test_matchertext_bypass(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int b;
  UNUSED_PARAMETER(clientData);
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOLEAN");
    return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[1], &b) ) return TCL_ERROR;
  sqlite3MatchertextTestBypass = b;
  return TCL_OK;
}

static int test_matchertext_exec_cb(
  void *pArg,
  int nCol,
  char **azValue,
  char **azName
){
  Tcl_Obj *pList = (Tcl_Obj*)pArg;
  int i;
  UNUSED_PARAMETER(azName);
  for(i=0; i<nCol; i++){
    Tcl_ListObjAppendElement(0, pList,
        azValue[i] ? Tcl_NewStringObj(azValue[i], -1) : Tcl_NewObj());
  }
  return 0;
}

/* Usage: sqlite3_matchertext_exec DB TEMPLATE ?V|I VALUE ...? */
static int SQLITE_TCLAPI test_matchertext_exec(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  sqlite3_matchertext_arg *aArg = 0;
  sqlite3 *db;
  Tcl_Obj *pResult;
  int i;
  int nArg;
  int rc;
  char *zErr = 0;
  UNUSED_PARAMETER(clientData);

  if( objc<3 || (objc&1)==0 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB TEMPLATE ?V|I VALUE ...?");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  nArg = (objc-3)/2;
  if( nArg ){
    aArg = sqlite3_malloc64(sizeof(*aArg)*(sqlite3_uint64)nArg);
    if( aArg==0 ){
      Tcl_SetResult(interp, "out of memory", TCL_STATIC);
      return TCL_ERROR;
    }
  }
  for(i=0; i<nArg; i++){
    Tcl_Size n;
    const char *zType = Tcl_GetString(objv[3+2*i]);
    aArg[i].data = Tcl_GetByteArrayFromObj(objv[4+2*i], &n);
    aArg[i].size = (sqlite3_uint64)n;
    if( strcmp(zType, "V")==0 ){
      aArg[i].type = SQLITE_MATCHERTEXT_VALUE;
    }else if( strcmp(zType, "I")==0 ){
      aArg[i].type = SQLITE_MATCHERTEXT_IDENTIFIER;
    }else{
      sqlite3_free(aArg);
      Tcl_SetResult(interp, "argument type must be V or I", TCL_STATIC);
      return TCL_ERROR;
    }
  }
  pResult = Tcl_NewListObj(0, 0);
  Tcl_IncrRefCount(pResult);
  rc = sqlite3_matchertext_exec(db, Tcl_GetString(objv[2]), -1,
      aArg, nArg, test_matchertext_exec_cb, pResult, &zErr);
  sqlite3_free(aArg);
  if( rc==SQLITE_OK ){
    Tcl_SetObjResult(interp, pResult);
  }else{
    Tcl_SetObjResult(interp,
        Tcl_NewStringObj(zErr ? zErr : sqlite3_errmsg(db), -1));
  }
  Tcl_DecrRefCount(pResult);
  sqlite3_free(zErr);
  return rc==SQLITE_OK ? TCL_OK : TCL_ERROR;
}

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
** Usage:  sqlite3_matchertext_encode VALUE
**         sqlite3_matchertext_decode VALUE
**
** Exercise the public encoder and its inverse.  Values arrive and leave as
** byte arrays, so zero bytes and invalid UTF-8 survive the trip.
*/
static int SQLITE_TCLAPI test_matchertext_recode(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  unsigned char *zVal;
  Tcl_Size nVal;
  char *zOut;
  sqlite3_int64 nOut = 0;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "VALUE");
    return TCL_ERROR;
  }
  zVal = Tcl_GetByteArrayFromObj(objv[1], &nVal);
  if( clientData ){
    zOut = sqlite3_matchertext_decode((const char*)zVal, (sqlite3_int64)nVal,
                                      &nOut);
  }else{
    zOut = sqlite3_matchertext_encode((const char*)zVal, (sqlite3_int64)nVal,
                                      &nOut);
  }
  if( zOut==0 ){
    Tcl_AppendResult(interp, "out of memory", (char*)0);
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp,
      Tcl_NewByteArrayObj((unsigned char*)zOut, (Tcl_Size)nOut));
  sqlite3_free(zOut);
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
    void *pArg;
  } aObjCmd[] = {
    { "sqlite3_matchertext_verify", test_matchertext_verify, 0 },
    { "sqlite3_matchertext_end",    test_matchertext_end,    0 },
    { "sqlite3_matchertext_printf", test_matchertext_printf, 0 },
    { "sqlite3_matchertext_encode", test_matchertext_recode, 0 },
    { "sqlite3_matchertext_decode", test_matchertext_recode, (void*)1 },
    { "sqlite3_matchertext_exec",   test_matchertext_exec, 0 },
    { "sqlite3_matchertext_test_bypass", test_matchertext_bypass, 0 },
  };
  int i;
  for(i=0; i<(int)(sizeof(aObjCmd)/sizeof(aObjCmd[0])); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, aObjCmd[i].xProc,
                         aObjCmd[i].pArg, 0);
  }
#endif
  return TCL_OK;
}
