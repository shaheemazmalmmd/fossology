/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief What is written for one file, and when the upload may claim it
 *
 * The policy that decides a file's rows sits apart from the statements that
 * write them so that it can be exercised against a stub: the states this has
 * to get right are the ones a database does not reach on request -- a write
 * that does not run at each of the boundaries in turn.
 */
#include <stdio.h>
#include <string.h>
#include "thesmo_commit.h"

/** Whether every name an expression joins is already a license_ref row.
 *
 * A declaration that states an operator is reported as the one expression it
 * states, which is a name no catalog carries. The members are SPDX licenses
 * the database knows, so the composed row records the author's choice rather
 * than inventing a license -- and a name holding an operator this does not
 * recognise is left to the usual rule.
 */
static int thMemberKnown(fo_dbManager *dbm, char *one, thDbStatus *st)
{
  /* A member carrying an exception is a pair, and the pair's own row is only
     there once something has minted it; both halves being known is what the
     WITH rule already accepts. */
  char *with = strstr(one, " WITH ");
  long lic, exc;
  if (thDbLicenseId(dbm, one, st)) return 1;
  if (*st == TH_DB_FAILED || !with) return 0;
  *with = '\0';
  lic = thDbLicenseId(dbm, one, st);
  if (*st == TH_DB_FAILED) return 0;
  exc = thDbLicenseId(dbm, with + 6, st);
  return lic && exc;
}


static int thExpressionKnown(fo_dbManager *dbm, const char *name, thDbStatus *st)
{
  const char *at = name, *sep;
  char one[256];
  int any = 0;
  *st = TH_DB_OK;
  if (!strstr(name, " OR ") && !strstr(name, " AND ")) return 0;
  while (*at) {
    size_t l;
    const char *o = strstr(at, " OR "), *a = strstr(at, " AND ");
    sep = (!o || (a && a < o)) ? a : o;
    l = sep ? (size_t) (sep - at) : strlen(at);
    if (l == 0 || l >= sizeof(one)) return 0;
    memcpy(one, at, l);
    one[l] = '\0';
    if (!thMemberKnown(dbm, one, st)) return 0;
    any = 1;
    if (!sep) break;
    at = sep + (sep == o ? 4 : 5);
  }
  return any;
}


/** Resolve the license_ref row a finding should be recorded against.
 *
 * Returns 0 and TH_DB_ABSENT where the vocabulary does not carry the name,
 * which costs this finding and not the file; TH_DB_FAILED means the question
 * could not be asked and the file has to be rolled back.
 */
static long thFindingRow(fo_dbManager *dbm, const thResult *f, int createMissing,
    thDbStatus *st)
{
  /* SPDX writes the pair as one expression and FOSSology stores it as one
     license_ref row spelled the same way. */
  char named[512];
  const char *shortname = f->license;
  long rfPk;
  /* An exception quoted without its license is reported in both fields, and
     pairing them would mint a license_ref row for "E WITH E". The exception
     alone is the identification. */
  if (f->exception && strcmp(f->license, f->exception) != 0) {
    snprintf(named, sizeof(named), "%s WITH %s", f->license, f->exception);
    shortname = named;
  }
  rfPk = thDbLicenseId(dbm, shortname, st);
  if (rfPk || *st == TH_DB_FAILED) return rfPk;
  if (f->exception) {
    /* Pairing two known names is not minting an unknown license, so it needs
       no --create-missing. Fall back to the license. */
    long lic = thDbLicenseId(dbm, f->license, st);
    if (*st == TH_DB_FAILED) return 0;
    if (lic && thDbLicenseId(dbm, f->exception, st))
      rfPk = thDbCreateLicense(dbm, shortname, st);
    if (*st == TH_DB_FAILED) return 0;
    if (!rfPk)
      rfPk = createMissing ? thDbCreateLicense(dbm, f->license, st)
                           : thDbLicenseId(dbm, f->license, st);
    return rfPk;
  }
  if (thExpressionKnown(dbm, shortname, st)) {
    /* Joining names the database already has is not minting an unknown
       license, so an expression needs no --create-missing either: the same
       reasoning as the pair above. */
    return thDbCreateLicense(dbm, shortname, st);
  }
  if (*st == TH_DB_FAILED) return 0;
  if (createMissing) return thDbCreateLicense(dbm, shortname, st);
  *st = TH_DB_ABSENT;
  return 0;
}


/** Write one file's findings, all of them or none.
 *
 * The transaction is the file, not the upload: it covers the findings, their
 * highlights and the marker that says the file was scanned, so any row this
 * agent holds for a pfile proves the whole file was committed. That is what
 * makes the existing file selection -- which offers a pfile only while this
 * agent holds no row for it -- correct to retry against. Without it a file
 * that wrote its first finding and lost its second is excluded from every
 * later scan, and the loss is permanent and silent.
 *
 * An upload-wide transaction would hold locks for the length of a scan, so
 * the boundary stays at one file.
 *
 * 
eturn 1 where the file is committed and accounted for
 */
int thWriteFile(fo_dbManager *dbm, int agentPk, long pfilePk,
    const thResult *all, int nf, int createMissing, thTally *t)
{
  int f, written = 0, degraded = 0;
  thDbStatus st = TH_DB_OK;
  if (!fo_dbManager_begin(dbm)) {
    t->failed++;
    return 0;
  }
  /* The file list was filtered the same way when it was drawn; this is the
     late look, so a pfile two uploads hold does not get two sets of rows. */
  if (thDbAlreadyScanned(dbm, agentPk, pfilePk, &st)) {
    fo_dbManager_rollback(dbm);
    t->alreadyHeld++;
    return 1;
  }
  if (st == TH_DB_FAILED) {
    fo_dbManager_rollback(dbm);
    t->failed++;
    return 0;
  }
  for (f = 0; f < nf; f++) {
    long rfPk = thFindingRow(dbm, &all[f], createMissing, &st);
    long flPk;
    if (!rfPk) {
      if (st == TH_DB_FAILED) {
        fo_dbManager_rollback(dbm);
        t->failed++;
        return 0;
      }
      degraded++;  /* the name is not seeded: a packaging gap, not a failure */
      continue;
    }
    flPk = thDbInsertFinding(dbm, rfPk, agentPk, pfilePk, all[f].score, &st);
    if (!flPk) {
      fo_dbManager_rollback(dbm);
      t->failed++;
      return 0;
    }
    if (all[f].len > 0) {
      thDbInsertHighlight(dbm, flPk, all[f].start, all[f].len, &st);
      if (st == TH_DB_FAILED) {
        fo_dbManager_rollback(dbm);
        t->failed++;
        return 0;
      }
    }
    written++;
  }
  /* Nothing found is a result and is recorded as one. Findings dropped for
     want of a license_ref row are not: marking the file clean would state
     that it names no license, and would keep it out of the rescan that
     seeding the vocabulary is supposed to make worthwhile. */
  if (!written && !degraded && !thDbInsertNoFinding(dbm, agentPk, pfilePk, &st)) {
    fo_dbManager_rollback(dbm);
    t->failed++;
    return 0;
  }
  if (!fo_dbManager_commit(dbm)) {
    t->failed++;
    return 0;
  }
  if (degraded) t->degraded += degraded;
  if (written) t->withFindings++;
  else if (!degraded) t->clean++;
  return 1;
}


