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

static void thDbSet(thDbStatus *st, thDbStatus value)
{
  if (st) *st = value;
}

long thDbAgentId(fo_dbManager *dbm, const char *name, const char *rev,
    const char *desc, thDbStatus *st)
{
  PGresult *r;
  long id = 0;
  /* The pair is unique, so the insert names that constraint and a worker that
     lost the race reads the row the winner made. The description is written
     only when the row is new: an existing identity's record is history. */
  r = fo_dbManager_ExecPrepared(
      fo_dbManager_PrepareStamement(dbm, "thesmo_agent_id",
          "WITH made AS ("
            "INSERT INTO agent (agent_name, agent_desc, agent_enabled, agent_rev)"
            " VALUES ($1, $2, true, $3)"
            " ON CONFLICT (agent_name, agent_rev) DO NOTHING"
            " RETURNING agent_pk"
          ") "
          "SELECT agent_pk FROM made"
          " UNION ALL "
          "SELECT agent_pk FROM agent"
          " WHERE agent_name = $1 AND agent_rev = $3"
          " LIMIT 1",
          char*, char*, char*),
      name, desc, rev);
  if (!r) {
    thDbSet(st, TH_DB_FAILED);
    return 0;
  }
  if (PQntuples(r) > 0) id = atol(PQgetvalue(r, 0, 0));
  PQclear(r);
  thDbSet(st, id ? TH_DB_OK : TH_DB_FAILED);
  return id;
}

long thDbLicenseId(fo_dbManager *dbm, const char *shortname, thDbStatus *st)
{
  PGresult *r;
  long id = 0;
  r = fo_dbManager_ExecPrepared(
      fo_dbManager_PrepareStamement(dbm, "thesmo_license_id",
          "SELECT rf_pk FROM license_ref WHERE rf_shortname = $1 LIMIT 1",
          char*),
      shortname);
  if (!r) {
    thDbSet(st, TH_DB_FAILED);
    return 0;
  }
  if (PQntuples(r) > 0) id = atol(PQgetvalue(r, 0, 0));
  PQclear(r);
  thDbSet(st, id ? TH_DB_OK : TH_DB_ABSENT);
  return id;
}

long thDbInsertFinding(fo_dbManager *dbm, long rfPk, int agentPk, long pfilePk,
    int matchPct, thDbStatus *st)
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
  if (!r) {
    thDbSet(st, TH_DB_FAILED);
    return 0;
  }
  if (PQntuples(r) > 0) flPk = atol(PQgetvalue(r, 0, 0));
  PQclear(r);
  /* The insert ran, so a missing RETURNING row is not an absence to tolerate:
     there is no fl_pk to hang a highlight on and nothing was recorded. */
  thDbSet(st, flPk ? TH_DB_OK : TH_DB_FAILED);
  return flPk;
}

int thDbInsertNoFinding(fo_dbManager *dbm, int agentPk, long pfilePk,
    thDbStatus *st)
{
  PGresult *r;
  r = fo_dbManager_ExecPrepared(
      fo_dbManager_PrepareStamement(dbm, "thesmo_insert_no_finding",
          "INSERT INTO license_file (agent_fk, pfile_fk) VALUES ($1, $2)",
          int, long),
      agentPk, pfilePk);
  if (!r) {
    thDbSet(st, TH_DB_FAILED);
    return 0;
  }
  PQclear(r);
  thDbSet(st, TH_DB_OK);
  return 1;
}

int thDbInsertHighlight(fo_dbManager *dbm, long flPk, long start, long len,
    thDbStatus *st)
{
  PGresult *r;
  if (flPk <= 0 || len <= 0) {
    thDbSet(st, TH_DB_ABSENT);
    return 0;
  }
  r = fo_dbManager_ExecPrepared(
      fo_dbManager_PrepareStamement(dbm, "thesmo_insert_highlight",
          "INSERT INTO highlight (fl_fk, start, len, type)"
          " VALUES ($1, $2, $3, 'L')",
          long, long, long),
      flPk, start, len);
  if (!r) {
    thDbSet(st, TH_DB_FAILED);
    return 0;
  }
  PQclear(r);
  thDbSet(st, TH_DB_OK);
  return 1;
}

int thDbAlreadyScanned(fo_dbManager *dbm, int agentPk, long pfilePk,
    thDbStatus *st)
{
  PGresult *r;
  int seen = 0;
  r = fo_dbManager_ExecPrepared(
      fo_dbManager_PrepareStamement(dbm, "thesmo_already_scanned",
          "SELECT 1 WHERE EXISTS (SELECT 1 FROM license_file"
          " WHERE agent_fk = $1 AND pfile_fk = $2)",
          int, long),
      agentPk, pfilePk);
  if (!r) {
    thDbSet(st, TH_DB_FAILED);
    return 0;
  }
  seen = PQntuples(r) > 0;
  PQclear(r);
  thDbSet(st, seen ? TH_DB_OK : TH_DB_ABSENT);
  return seen;
}

long thDbCreateLicense(fo_dbManager *dbm, const char *shortname,
    thDbStatus *st)
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
  if (!r) {
    thDbSet(st, TH_DB_FAILED);
    return 0;
  }
  if (PQntuples(r) > 0) id = atol(PQgetvalue(r, 0, 0));
  PQclear(r);
  thDbSet(st, id ? TH_DB_OK : TH_DB_FAILED);
  return id;
}
