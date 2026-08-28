/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief A write that does not run, at each boundary in turn
 *
 * The states this has to get right are the ones a database will not reach on
 * request. The statements are stubbed here so each of them can be made to
 * fail on demand, and what is checked is the only thing that matters to a
 * later scan: whether the file left rows behind.
 *
 * The invariant: any row this agent holds for a pfile proves the whole file
 * was committed. The file list offers a pfile only while the agent holds no
 * row for it, so a half-written file would be excluded from every later scan
 * and the loss would be permanent and silent.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "thesmo_commit.h"

/* ------------------------------------------------------------------ stubs */

/** Which statement is made to fail, and the shape of the database. */
static struct {
  const char *failAt;  /* the stub that returns TH_DB_FAILED */
  int failOnCall;      /* which call of it, 1-based; 0 means every call */
  int calls;           /* calls to that stub so far */
  int inTxn;           /* BEGIN seen and not yet ended */
  int commits;
  int rollbacks;
  int rows;            /* rows the transaction holds */
  int committed;       /* rows that survived */
  int alreadyScanned;
  int knownNames;      /* 0: nothing is seeded */
  long nextPk;
} db;

static int failing(const char *who)
{
  if (!db.failAt || strcmp(db.failAt, who) != 0) return 0;
  db.calls++;
  return !db.failOnCall || db.calls == db.failOnCall;
}

int fo_dbManager_begin(fo_dbManager *dbm)
{
  (void) dbm;
  if (failing("begin")) return 0;
  db.inTxn = 1;
  db.rows = 0;
  return 1;
}

int fo_dbManager_commit(fo_dbManager *dbm)
{
  (void) dbm;
  if (failing("commit")) { db.inTxn = 0; return 0; }
  db.inTxn = 0;
  db.commits++;
  db.committed += db.rows;
  db.rows = 0;
  return 1;
}

int fo_dbManager_rollback(fo_dbManager *dbm)
{
  (void) dbm;
  db.inTxn = 0;
  db.rollbacks++;
  db.rows = 0;
  return 1;
}

long thDbLicenseId(fo_dbManager *dbm, const char *shortname, thDbStatus *st)
{
  (void) dbm;
  if (failing("licenseId")) { if (st) *st = TH_DB_FAILED; return 0; }
  if (db.knownNames && strchr(shortname, ' ') == NULL) {
    if (st) *st = TH_DB_OK;
    return 42;
  }
  if (st) *st = TH_DB_ABSENT;
  return 0;
}

long thDbCreateLicense(fo_dbManager *dbm, const char *shortname, thDbStatus *st)
{
  (void) dbm; (void) shortname;
  if (failing("createLicense")) { if (st) *st = TH_DB_FAILED; return 0; }
  if (st) *st = TH_DB_OK;
  return 99;
}

long thDbInsertFinding(fo_dbManager *dbm, long rfPk, int agentPk, long pfilePk,
    int matchPct, thDbStatus *st)
{
  (void) dbm; (void) rfPk; (void) agentPk; (void) pfilePk; (void) matchPct;
  if (failing("insertFinding")) { if (st) *st = TH_DB_FAILED; return 0; }
  if (st) *st = TH_DB_OK;
  db.rows++;
  return ++db.nextPk;
}

int thDbInsertNoFinding(fo_dbManager *dbm, int agentPk, long pfilePk,
    thDbStatus *st)
{
  (void) dbm; (void) agentPk; (void) pfilePk;
  if (failing("insertNoFinding")) { if (st) *st = TH_DB_FAILED; return 0; }
  if (st) *st = TH_DB_OK;
  db.rows++;
  return 1;
}

int thDbInsertHighlight(fo_dbManager *dbm, long flPk, long start, long len,
    thDbStatus *st)
{
  (void) dbm; (void) flPk; (void) start; (void) len;
  if (failing("insertHighlight")) { if (st) *st = TH_DB_FAILED; return 0; }
  if (st) *st = TH_DB_OK;
  db.rows++;
  return 1;
}

int thDbAlreadyScanned(fo_dbManager *dbm, int agentPk, long pfilePk,
    thDbStatus *st)
{
  (void) dbm; (void) agentPk; (void) pfilePk;
  if (failing("alreadyScanned")) { if (st) *st = TH_DB_FAILED; return 0; }
  if (st) *st = db.alreadyScanned ? TH_DB_OK : TH_DB_ABSENT;
  return db.alreadyScanned;
}

/* ------------------------------------------------------------------ cases */

static int failures;

static void check(int ok, const char *what)
{
  printf("%s - %s\n", ok ? "ok      " : "NOT ok  ", what);
  if (!ok) failures++;
}

/** Two findings, the second of them carrying a highlight. */
static int twoFindings(thResult *all)
{
  memset(all, 0, sizeof(all[0]) * 2);
  all[0].license = "MIT";
  all[0].score = 90;
  all[1].license = "Apache-2.0";
  all[1].score = 80;
  all[1].start = 10;
  all[1].len = 20;
  return 2;
}

static void reset(const char *failAt)
{
  memset(&db, 0, sizeof(db));
  db.failAt = failAt;
  db.knownNames = 1;
}

static void resetAt(const char *failAt, int call)
{
  reset(failAt);
  db.failOnCall = call;
}

/** A write that does not run leaves no rows and the file retryable. */
static void boundary(const char *failAt, const char *what)
{
  thResult all[2];
  thTally t = {0, 0, 0, 0, 0, 0};
  int nf = twoFindings(all);
  char msg[160];
  int ok;
  reset(failAt);
  ok = thWriteFile(NULL, 7, 1234, all, nf, 0, &t);
  snprintf(msg, sizeof(msg), "%s: rolled back, nothing committed, retryable",
      what);
  check(!ok && t.failed == 1 && db.committed == 0 && db.inTxn == 0, msg);
}

int main(void)
{
  thResult all[2];

  /* Every boundary in turn. A transaction that never opened needs no
     rollback, which is why the check is on what was committed. */
  boundary("begin", "the transaction does not open");
  boundary("alreadyScanned", "the look for existing rows fails");
  boundary("insertFinding", "the first finding does not insert");
  boundary("insertHighlight", "a highlight does not insert");
  boundary("commit", "the commit does not run");

  {
    /* The shape that cost findings before there was a transaction: the first
       finding inserts, the second does not. The file then held a row, so the
       file list never offered it again and the second licence was lost for
       good. Both have to go back. */
    thTally t = {0, 0, 0, 0, 0, 0};
    int nf = twoFindings(all);
    resetAt("insertFinding", 2);
    check(!thWriteFile(NULL, 7, 1234, all, nf, 0, &t) && t.failed == 1
          && db.committed == 0 && db.rollbacks == 1,
        "a second finding that does not insert takes the first one back");
  }

  {
    thTally t = {0, 0, 0, 0, 0, 0};
    int nf = twoFindings(all);
    reset(NULL);
    check(thWriteFile(NULL, 7, 1234, all, nf, 0, &t) && t.withFindings == 1
          && db.commits == 1 && db.committed == 3 && db.rollbacks == 0,
        "nothing fails: two findings and one highlight commit together");
  }

  {
    /* Nothing found is a result: without a row the file is offered again on
       every run, and "scanned, found nothing" cannot be told from "never
       scanned". */
    thTally t = {0, 0, 0, 0, 0, 0};
    reset(NULL);
    check(thWriteFile(NULL, 7, 1234, NULL, 0, 0, &t) && t.clean == 1
          && db.committed == 1,
        "a file naming no license is recorded as scanned");
  }

  {
    /* The vocabulary does not carry the name. The finding is lost by policy,
       but the file must NOT be marked as naming no license: that would state
       something untrue and would keep the file out of the rescan that seeding
       the vocabulary is meant to make worthwhile. */
    thTally t = {0, 0, 0, 0, 0, 0};
    int nf = twoFindings(all);
    reset(NULL);
    db.knownNames = 0;
    check(thWriteFile(NULL, 7, 1234, all, nf, 0, &t) && t.degraded == 2
          && t.clean == 0 && t.withFindings == 0 && db.committed == 0,
        "an unseeded name costs its finding and leaves the file unrecorded");
  }

  {
    /* A name the catalogue does not have is absent, not a failure, and the
       two are told apart by the status rather than by an id of zero: a
       failure here has to roll the file back. */
    thTally t = {0, 0, 0, 0, 0, 0};
    int nf = twoFindings(all);
    reset("licenseId");
    check(!thWriteFile(NULL, 7, 1234, all, nf, 0, &t) && t.failed == 1
          && t.degraded == 0 && db.committed == 0,
        "a lookup that fails is not read as an unseeded name");
  }

  {
    /* Two workers on one pfile: the same file can be in more than one upload.
       The late look does not close the window, but a file another worker
       holds must not get a second set of rows. */
    thTally t = {0, 0, 0, 0, 0, 0};
    int nf = twoFindings(all);
    reset(NULL);
    db.alreadyScanned = 1;
    check(thWriteFile(NULL, 7, 1234, all, nf, 0, &t) && t.alreadyHeld == 1
          && db.committed == 0 && db.rollbacks == 1,
        "a file another worker already holds is left alone");
  }

  {
    /* Retrying the file the previous run rolled back writes it once. */
    thTally t = {0, 0, 0, 0, 0, 0};
    int nf = twoFindings(all);
    reset("insertHighlight");
    thWriteFile(NULL, 7, 1234, all, nf, 0, &t);
    db.failAt = NULL;
    check(thWriteFile(NULL, 7, 1234, all, nf, 0, &t) && t.withFindings == 1
          && db.committed == 3 && t.failed == 1,
        "the retry after a rollback writes the file exactly once");
  }

  printf("%s\n", failures ? "FAILED" : "all boundaries accounted for");
  return failures ? 1 : 0;
}
