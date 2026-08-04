/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Main file for the SQLite library.  The routines in this file
** implement the programmer interface to the library.  Routines in
** other files are for internal use by SQLite and should not be
** accessed by users of the library.
*/

#include "sqliteInt.h"

/*
** Execute SQL code.  Return one of the SQLITE_ success/failure
** codes.  Also write an error message into memory obtained from
** malloc() and make *pzErrMsg point to that message.
**
** If the SQL is a query, then for each row in the query result
** the xCallback() function is called.  pArg becomes the first
** argument to xCallback().  If xCallback=NULL then no callback
** is invoked, even for queries.
*/
int sqlite3_exec(
  sqlite3 *db,                /* The database on which the SQL executes */
  const char *zSql,           /* The SQL to be executed */
  sqlite3_callback xCallback, /* Invoke this callback routine */
  void *pArg,                 /* First argument to xCallback() */
  char **pzErrMsg             /* Write error messages here */
){
  int rc = SQLITE_OK;         /* Return code */
  const char *zLeftover;      /* Tail of unprocessed SQL */
  sqlite3_stmt *pStmt = 0;    /* The current SQL statement */
  char **azCols = 0;          /* Names of result columns */
  int callbackIsInit;         /* True if callback data is initialized */

  if( !sqlite3SafetyCheckOk(db) ) return SQLITE_MISUSE_BKPT;
#ifdef SQLITE_ENABLE_MATCHERTEXT
  if( !sqlite3MatchertextLegacyAllowed(db) ){
    rc = sqlite3MatchertextLegacyError(db, "sqlite3_matchertext_exec()");
    if( pzErrMsg ) *pzErrMsg = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    return rc;
  }
#endif
  if( zSql==0 ) zSql = "";

  sqlite3_mutex_enter(db->mutex);
  sqlite3Error(db, SQLITE_OK);
  while( rc==SQLITE_OK && zSql[0] ){
    int nCol = 0;
    char **azVals = 0;

    pStmt = 0;
    rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, &zLeftover);
    assert( rc==SQLITE_OK || pStmt==0 );
    if( rc!=SQLITE_OK ){
      continue;
    }
    if( !pStmt ){
      /* this happens for a comment or white-space */
      zSql = zLeftover;
      continue;
    }
    callbackIsInit = 0;

    while( 1 ){
      int i;
      rc = sqlite3_step(pStmt);

      /* Invoke the callback function if required */
      if( xCallback && (SQLITE_ROW==rc || 
          (SQLITE_DONE==rc && !callbackIsInit
                           && db->flags&SQLITE_NullCallback)) ){
        if( !callbackIsInit ){
          nCol = sqlite3_column_count(pStmt);
          azCols = sqlite3DbMallocRaw(db, (2*nCol+1)*sizeof(const char*));
          if( azCols==0 ){
            goto exec_out;
          }
          for(i=0; i<nCol; i++){
            azCols[i] = (char *)sqlite3_column_name(pStmt, i);
            /* sqlite3VdbeSetColName() installs column names as UTF8
            ** strings so there is no way for sqlite3_column_name() to fail. */
            assert( azCols[i]!=0 );
          }
          callbackIsInit = 1;
        }
        if( rc==SQLITE_ROW ){
          azVals = &azCols[nCol];
          for(i=0; i<nCol; i++){
            azVals[i] = (char *)sqlite3_column_text(pStmt, i);
            if( !azVals[i] && sqlite3_column_type(pStmt, i)!=SQLITE_NULL ){
              sqlite3OomFault(db);
              goto exec_out;
            }
          }
          azVals[i] = 0;
        }
        if( xCallback(pArg, nCol, azVals, azCols) ){
          /* EVIDENCE-OF: R-38229-40159 If the callback function to
          ** sqlite3_exec() returns non-zero, then sqlite3_exec() will
          ** return SQLITE_ABORT. */
          rc = SQLITE_ABORT;
          sqlite3VdbeFinalize((Vdbe *)pStmt);
          pStmt = 0;
          sqlite3Error(db, SQLITE_ABORT);
          goto exec_out;
        }
      }

      if( rc!=SQLITE_ROW ){
        rc = sqlite3VdbeFinalize((Vdbe *)pStmt);
        pStmt = 0;
        zSql = zLeftover;
        while( sqlite3Isspace(zSql[0]) ) zSql++;
        break;
      }
    }

    sqlite3DbFree(db, azCols);
    azCols = 0;
  }

exec_out:
  if( pStmt ) sqlite3VdbeFinalize((Vdbe *)pStmt);
  sqlite3DbFree(db, azCols);

  rc = sqlite3ApiExit(db, rc);
  if( rc!=SQLITE_OK && pzErrMsg ){
    *pzErrMsg = sqlite3DbStrDup(0, sqlite3_errmsg(db));
    if( *pzErrMsg==0 ){
      rc = SQLITE_NOMEM_BKPT;
      sqlite3Error(db, SQLITE_NOMEM);
    }
  }else if( pzErrMsg ){
    *pzErrMsg = 0;
  }

  assert( (rc&db->errMask)==rc );
  sqlite3_mutex_leave(db->mutex);
  return rc;
}

#ifdef SQLITE_ENABLE_MATCHERTEXT
/* Execute one checked matchertext template. */
int sqlite3_matchertext_exec(
  sqlite3 *db,
  const char *zTemplate,
  int nTemplate,
  const sqlite3_matchertext_arg *aArg,
  int nArg,
  sqlite3_callback xCallback,
  void *pArg,
  char **pzErrMsg
){
  sqlite3_stmt *pStmt = 0;
  char **az = 0;
  int i;
  int nCol = 0;
  int rc;

  if( pzErrMsg ) *pzErrMsg = 0;
  rc = sqlite3_matchertext_prepare_v3(db, zTemplate, nTemplate, 0,
                                      aArg, nArg, &pStmt);
  if( rc!=SQLITE_OK ) goto matchertext_exec_out;
  if( pStmt==0 ) goto matchertext_exec_out;
  nCol = sqlite3_column_count(pStmt);
  if( xCallback && nCol>0 ){
    az = sqlite3_malloc64((sqlite3_uint64)(2*nCol+1)*sizeof(char*));
    if( az==0 ){
      rc = SQLITE_NOMEM_BKPT;
      goto matchertext_exec_out;
    }
    for(i=0; i<nCol; i++) az[i] = (char*)sqlite3_column_name(pStmt, i);
  }
  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    if( xCallback ){
      for(i=0; i<nCol; i++){
        az[nCol+i] = (char*)sqlite3_column_text(pStmt, i);
        if( !az[nCol+i] && sqlite3_column_type(pStmt, i)!=SQLITE_NULL ){
          sqlite3OomFault(db);
          rc = SQLITE_NOMEM_BKPT;
          goto matchertext_exec_out;
        }
      }
      az[2*nCol] = 0;
      if( xCallback(pArg, nCol, az+nCol, az) ){
        rc = SQLITE_ABORT;
        break;
      }
    }
  }
  if( rc==SQLITE_DONE ) rc = SQLITE_OK;

matchertext_exec_out:
  sqlite3_free(az);
  if( pStmt ){
    int rc2 = sqlite3_finalize(pStmt);
    if( rc==SQLITE_OK ) rc = rc2;
  }
  if( rc!=SQLITE_OK && pzErrMsg ){
    *pzErrMsg = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    if( *pzErrMsg==0 ) rc = SQLITE_NOMEM_BKPT;
  }
  return rc;
}
#endif
