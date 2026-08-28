/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief Database access for the Thesmo agent
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "thesmo_db.h"

long thDbLicenseId(fo_dbManager *dbm, const char *shortname)
{
  PGresult *r;
  long id = 0;
  r = fo_dbManager_ExecPrepared(
      fo_dbManager_PrepareStamement(dbm, "thesmo_license_id",
          "SELECT rf_pk FROM license_ref WHERE rf_shortname = $1 LIMIT 1",
          char*),
      shortname);
  if (!r) return 0;
  if (PQntuples(r) > 0) id = atol(PQgetvalue(r, 0, 0));
  PQclear(r);
  return id;
}

long thDbInsertFinding(fo_dbManager *dbm, long rfPk, int agentPk, long pfilePk,
    int matchPct)
{
  PGresult *r;
  long flPk = 0;
  /* A referential finding says nothing about how much text matched, so it
     records no percentage; a literal 0% would read as a very poor match. */
  if (matchPct <= 0) {
    r = fo_dbManager_ExecPrepared(
        fo_dbManager_PrepareStamement(dbm, "thesmo_insert_finding_nopct",
            "INSERT INTO license_file (rf_fk, agent_fk, pfile_fk, rf_match_pct)"
            " VALUES ($1, $2, $3, NULL) RETURNING fl_pk",
            long, int, long),
        rfPk, agentPk, pfilePk);
  } else {
    r = fo_dbManager_ExecPrepared(
        fo_dbManager_PrepareStamement(dbm, "thesmo_insert_finding",
            "INSERT INTO license_file (rf_fk, agent_fk, pfile_fk, rf_match_pct)"
            " VALUES ($1, $2, $3, $4) RETURNING fl_pk",
            long, int, long, int),
        rfPk, agentPk, pfilePk, matchPct);
  }
  if (!r) return 0;
  if (PQntuples(r) > 0) flPk = atol(PQgetvalue(r, 0, 0));
  PQclear(r);
  return flPk;
}

int thDbInsertHighlight(fo_dbManager *dbm, long flPk, long start, long len)
{
  PGresult *r;
  if (flPk <= 0 || len <= 0) return 0;
  r = fo_dbManager_ExecPrepared(
      fo_dbManager_PrepareStamement(dbm, "thesmo_insert_highlight",
          "INSERT INTO highlight (fl_fk, start, len, type)"
          " VALUES ($1, $2, $3, 'L')",
          long, long, long),
      flPk, start, len);
  if (!r) return 0;
  PQclear(r);
  return 1;
}

long thDbCreateLicense(fo_dbManager *dbm, const char *shortname)
{
  PGresult *r;
  long id = 0;
  /* rf_shortname is indexed but not unique, so ON CONFLICT has no constraint
     to name. Select-or-insert in one statement, as scancode does. */
  r = fo_dbManager_ExecPrepared(
      fo_dbManager_PrepareStamement(dbm, "thesmo_create_license",
          "WITH existing AS ("
            "SELECT rf_pk FROM ONLY license_ref WHERE rf_shortname = $1"
          "), created AS ("
            "INSERT INTO license_ref (rf_shortname, rf_text, rf_detector_type)"
            " SELECT $1, $2, 2"
            " WHERE NOT EXISTS (SELECT 1 FROM existing)"
            " RETURNING rf_pk"
          ") "
          "SELECT rf_pk FROM created UNION SELECT rf_pk FROM existing",
          char*, char*),
      shortname, "License by Thesmo.");
  if (!r) return 0;
  if (PQntuples(r) > 0) id = atol(PQgetvalue(r, 0, 0));
  PQclear(r);
  return id;
}
