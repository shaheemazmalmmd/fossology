/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \dir
 * \brief The Thesmo agent
 * \file
 * \brief Entry point for the thesmo agent
 * \page thesmo Thesmo Agent
 * \tableofcontents
 *
 * Thesmo is a second-stage detector for the license references nomos leaves
 * unnamed. It is a sibling agent, not a change to nomos: it writes its own rows
 * under its own agent id, so a nomos finding can never be overwritten by a
 * model. The decider combines the two.
 *
 * The model is a grant gate, a linear classifier over TF-IDF features, and two
 * corrective heads (only/or-later, and a deterministic version constraint). It
 * abstains whenever the gate rejects the file or the top two classes are too
 * close, which is the behavior the FOSSology false-positive budget requires.
 *
 * \section thesmoactions Supported actions
 * | Command line flag | Description |
 * | ---: | :--- |
 * | -h | Shows help |
 * | -J | Output JSON |
 * | -m dir | Model directory (default DATADIR/thesmo/model) |
 * | -v | Verbose |
 * | -c config | Path to the sysconfigdir |
 * | --scheduler_start | Runs in scheduler mode |
 * | file... | Files to scan |
 *
 * \section thesmo_source Agent source
 *   - \link src/thesmo/agent \endlink
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include "thesmo_model.h"

#ifdef FOSSOLOGY_AGENT
#include <libfossology.h>
#include "thesmo_db.h"
#include "thesmo_commit.h"
#define AGENT_NAME "thesmo"
#define AGENT_ARS  "thesmo_ars"
#endif

#ifndef DATADIR
#define DATADIR "/usr/local/share/fossology"
#endif
#ifndef VERSION_S
#define VERSION_S "0.0.0"
#endif
#ifndef COMMIT_HASH_S
#define COMMIT_HASH_S "unknown"
#endif

/**
 * \brief Read a file, refusing anything that is not text.
 *
 * Binary bytes still form character n-grams and enough of them land on some
 * license to win an argmax. Nothing readable carries a NUL in its first block.
 */
static char *thSlurp(const char *p, size_t cap, int *truncated)
{
  FILE *f = fopen(p, "rb");
  char *b;
  size_t n, probe, i;
  if (truncated) *truncated = 0;
  if (!f) return NULL;
  b = malloc(cap + 1);
  if (!b) { fclose(f); return NULL; }
  n = fread(b, 1, cap, f);
  b[n] = '\0';
  /* A file longer than the cap is read as far as the cap and scanned as if
     that were all of it, so a license past the boundary reads exactly like a
     file with no license in it. One more byte says which happened. */
  if (truncated && n == cap && fgetc(f) != EOF) *truncated = 1;
  fclose(f);
  probe = n < 8192 ? n : 8192;
  for (i = 0; i < probe; i++)
    if (b[i] == '\0') { free(b); return NULL; }
  /* A NUL past the first block is a generator's appended index, and a C string
     would end the file there. Blanked, not cut, so offsets still hold. */
  for (i = 0; i < n; i++)
    if (b[i] == '\0') b[i] = ' ';
  return b;
}


static void usage(const char *me)
{
  printf("Usage: %s [options] file [file ...]\n"
         "  -m dir  :: model directory (default %s/thesmo/model)\n"
         "  -J      :: JSON output\n"
         "  -v      :: verbose\n"
         "  -c dir  :: sysconfigdir\n"
         "  --create-missing :: insert license_ref rows for names the table\n"
         "                      lacks, instead of skipping those findings\n"
         "  --identity       :: print this detector's identity and check %s\n"
         "  --write-digest   :: write %s beside the model\n"
         "  -h      :: this help\n", me, DATADIR, TH_DIGEST_FILE,
         TH_DIGEST_FILE);
}

/** A path as a JSON string body: a name may carry a quote or a backslash. */
static void thJsonPuts(const char *s)
{
  for (; *s; s++) {
    unsigned char c = (unsigned char) *s;
    if (c == '"' || c == '\\') printf("\\%c", c);
    else if (c < 0x20) printf("\\u%04x", c);
    else putchar(c);
  }
}

int main(int argc, char **argv)
{
  const char *modelDir = NULL;
  int json = 0, verbose = 0, c, i, first = 1, scheduler = 0;
  int identity = 0, writeDigest = 0;
#ifdef FOSSOLOGY_AGENT
  /* Off by default: the seeder puts the whole vocabulary in license_ref first.
     Minting rows at scan time fills a database with textless shortnames. */
  int createMissing = 0;
#endif
  char dflt[1024], err[256];
  thModel *m;
  /* The scheduler always passes the long forms, and getopt_long rejects any it
     has not been told about. */
  static struct option longOpts[] = {
    {"config", required_argument, 0, 'c'},
    {"scheduler_start", no_argument, 0, 1000},
    {"userID", required_argument, 0, 1001},
    {"groupID", required_argument, 0, 1002},
    {"jobId", required_argument, 0, 1003},
    {"create-missing", no_argument, 0, 1004},
    {"no-create-missing", no_argument, 0, 1005},
    {"json", no_argument, 0, 'J'},
    {"verbose", no_argument, 0, 'v'},
    {"model", required_argument, 0, 'm'},
    {"identity", no_argument, 0, 1006},
    {"write-digest", no_argument, 0, 1007},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
  };

  while ((c = getopt_long(argc, argv, "m:c:Jvh", longOpts, NULL)) != -1) {
    switch (c) {
      case 'm': modelDir = optarg; break;
      case 'J': json = 1; break;
      case 'v': verbose = 1; break;
      case 'c': break; /* consumed by fo_scheduler_connect() */
      case 1000: scheduler = 1; break;
      case 1001: case 1002: case 1003: break;
      case 1004:
#ifdef FOSSOLOGY_AGENT
        createMissing = 1;
#endif
        break;
      case 1006: identity = 1; break;
      case 1007: writeDigest = 1; break;
      case 1005:
#ifdef FOSSOLOGY_AGENT
        createMissing = 0;
#endif
        break;
      default: usage(argv[0]); return c == 'h' ? 0 : 1;
    }
  }
  if (!modelDir) {
    snprintf(dflt, sizeof(dflt), "%s/thesmo/model", DATADIR);
    modelDir = dflt;
  }

  if (writeDigest) {
    if (thWriteModelDigest(modelDir, err, sizeof(err)) != 0) {
      fprintf(stderr, "thesmo: %s\n", err);
      return 1;
    }
    printf("%s/%s written\n", modelDir, TH_DIGEST_FILE);
    return 0;
  }
  if (identity) {
    char digest[17], found[64];
    int agrees;
    if (thModelDigest(modelDir, digest, sizeof(digest)) != 0) {
      fprintf(stderr, "thesmo: cannot digest %s\n", modelDir);
      return 1;
    }
    printf("engine     %s.%s\n", VERSION_S, COMMIT_HASH_S);
    printf("model      %s\n", modelDir);
    printf("detector   %s.%s.m%s\n", VERSION_S, COMMIT_HASH_S, digest);
    agrees = thCheckModelDigest(modelDir, digest, found, sizeof(found));
    if (agrees < 0)
      printf("%-10s absent; a reader has to digest the model itself\n",
          TH_DIGEST_FILE);
    else
      printf("%-10s %s (%s)\n", TH_DIGEST_FILE, found,
          agrees ? "agrees" : "STALE: the bytes say otherwise");
    return agrees == 0 ? 1 : 0;
  }

#ifdef FOSSOLOGY_AGENT
  if (scheduler) {
    fo_dbManager *dbm = NULL;
    PGconn *conn;
    int agentPk;
    int missing = 0, unstored = 0;
    char rev[128];
    thDbStatus st = TH_DB_OK;
    fo_scheduler_connect_dbMan(&argc, argv, &dbm);
    conn = fo_dbManager_getWrappedConnection(dbm);
    m = thLoad(modelDir, err, sizeof(err));
    if (!m) {
      LOG_FATAL("thesmo: cannot load model from %s: %s\n"
          "  The model is not part of the FOSSology sources. Reconfigure with\n"
          "  -DTHESMO_MODEL_DIR=/path/to/model and reinstall, or copy the model\n"
          "  directory (the one holding model.json) to %s.",
          modelDir, err, modelDir);
      fo_scheduler_disconnect(1);
      return 1;
    }
    /* The model is loaded first because the identity is partly its: the engine
       answers with these tables and these thresholds, so a model swap is a
       different detector and gets its own agent row. The ARS check and the
       file selection both key on that row, so they then do the right thing
       without being told -- an upgraded detector finds no successful scan of
       its own and is offered every file again, and the findings the previous
       one wrote stay under its id. */
    snprintf(rev, sizeof(rev), "%s.%s.m%s", VERSION_S, COMMIT_HASH_S,
        m->digest);
    agentPk = (int) thDbAgentId(dbm, AGENT_NAME, rev,
        "Thesmo licence-reference detector", &st);
    if (!agentPk) {
      LOG_FATAL("thesmo: cannot resolve the agent row for %s", rev);
      thFree(m);
      fo_scheduler_disconnect(1);
      return 1;
    }
    LOG_NOTICE("thesmo: detector %s, model %s, %d classes",
        rev, m->version, m->nClasses);
    {
      /* The identity above came from the bytes. This only says whether the
         number published beside the model still matches them, because that is
         what a reader too cheap to digest 87 MB will believe. */
      char found[64];
      int agrees = thCheckModelDigest(modelDir, m->digest, found, sizeof(found));
      if (agrees == 0) {
        LOG_ERROR("thesmo: %s/%s says %s and the model digests to %s; "
            "regenerate it with 'thesmo --write-digest -m %s' or anything "
            "reading it will take this scan for the other model",
            modelDir, TH_DIGEST_FILE, found, m->digest, modelDir);
      } else if (agrees < 0) {
        LOG_WARNING("thesmo: %s/%s is absent, so a reader cannot tell this "
            "model from another without digesting it; write it with "
            "'thesmo --write-digest -m %s'", modelDir, TH_DIGEST_FILE,
            modelDir);
      }
    }
    /* Without verifier.json the fallback margin bands are a weaker gate at the
       same number, so a partial model shows up only as lost precision. */
    if (!m->haveVerifier)
      LOG_WARNING("thesmo: %s has no verifier.json; scoring falls back to "
          "margin bands and min_score %d is not calibrated for them",
          modelDir, m->minScore);
    /* An unseeded name is a packaging gap, not a scan-time surprise, and costs
       its own findings rather than the upload.
       Both kinds of name are looked at, but they are not the same kind of
       problem. A learned class with no row is this model's packaging gap and
       the seeder fixes it. A family or a pointer from the referential table is
       FOSSology's own vocabulary: nomos emits nearly all of them and mints the
       row itself, and nomos runs over the upload before this agent does, so
       the row is normally there by the time anything is written. Reported
       apart so that one count stays actionable. A name beginning NOT- is a
       denial the rules read as a guard and never report. */
    for (i = 0; i < m->nClasses + m->nRef; i++) {
      thDbStatus st = TH_DB_OK;
      int learned = i < m->nClasses;
      const char *name = learned ? m->classes[i]
                                 : m->refName[i - m->nClasses];
      if (!learned
          && (strncmp(name, "NOT-", 4) == 0 || thDeclaredClass(m, name))) {
        continue;  /* a guard, or already looked at as a class */
      }
      if (thDbLicenseId(dbm, name, &st) == 0) {
        if (st == TH_DB_FAILED) {
          /* The vocabulary could not be read, so every finding would be
             skipped and the upload would complete looking scanned. */
          LOG_FATAL("thesmo: cannot read license_ref");
          thFree(m);
          fo_scheduler_disconnect(1);
          return 1;
        }
        if (!learned) {
          unstored++;
          continue;
        }
        if (createMissing) {
          if (thDbCreateLicense(dbm, name, &st) == 0)
            LOG_ERROR("thesmo: cannot create license_ref row for '%s'", name);
        } else {
          LOG_WARNING("thesmo: shortname '%s' is not in license_ref; its "
              "findings will be skipped", name);
          missing++;
        }
      }
    }
    if (missing)
      LOG_WARNING("thesmo: %d of %d model classes have no license_ref row and "
          "their findings will be skipped; run "
          "'php %s/thesmo/install/thesmo-seed.php %s/model.json', or pass "
          "--create-missing to have the agent insert them itself",
          missing, m->nClasses, DATADIR, modelDir);
    if (unstored)
      LOG_NOTICE("thesmo: %d families and pointers have no license_ref row "
          "yet; they are FOSSology's own names, which nomos mints as it meets "
          "them, and findings under them are skipped until it has",
          unstored);
    while (fo_scheduler_next()) {
      int uploadPk = atoi(fo_scheduler_current());
      PGresult *rows;
      int n, k, arsPk = 0;
      thTally t = {0, 0, 0, 0, 0, 0, 0, 0};
      char status[256];
      if (uploadPk == 0) continue;
      if (GetUploadPerm(conn, uploadPk, fo_scheduler_userID()) < PERM_WRITE) {
        LOG_ERROR("You have no update permissions on upload %d", uploadPk);
        continue;
      }
      arsPk = fo_WriteARS(conn, arsPk, uploadPk, agentPk, AGENT_ARS, 0, 0);
      if (!arsPk) {
        /* Carrying a zero into the closing call would insert a second row
           rather than complete this one, and that row's success argument is
           ignored. Nothing records the scan, so nothing may claim it. */
        LOG_ERROR("thesmo: cannot open an ARS record for upload %d", uploadPk);
        continue;
      }
      rows = getSelectedPFiles(conn, uploadPk, agentPk, false);
      if (fo_checkPQresult(conn, rows, NULL, __FILE__, __LINE__)) {
        /* Leave the ARS row unsuccessful: closing it would tell CheckARS the
           upload was scanned and nothing would scan it again. */
        LOG_FATAL("thesmo: cannot list the files of upload %d", uploadPk);
        thFree(m);
        fo_scheduler_disconnect(1);
        return 1;
      }
      n = PQntuples(rows);
      for (k = 0; k < n; k++) {
        long pfilePk = atol(PQgetvalue(rows, k, 0));
        char *repFile = fo_RepMkPath("files", PQgetvalue(rows, k, 1));
        char *text;
        char pickbuf[1024];
        thResult all[TH_MAX_FINDINGS];
        int nf, cut = 0, dropped = 0;
        fo_scheduler_heart(1);
        if (!repFile) { t.unreadable++; continue; }
        text = thSlurp(repFile, 4u * 1024 * 1024, &cut);
        free(repFile);
        if (!text) { t.unreadable++; continue; } /* unreadable, or not text */
        nf = thClassifyAll(m, text, all, TH_MAX_FINDINGS, pickbuf,
                           sizeof(pickbuf), &dropped);
        /* A cap reached is not a clean scan, and the answer for such a file is
           shorter than the file's own. Counted so the upload says so rather
           than reporting a coverage it did not have. */
        if (cut) t.truncated++;
        if (dropped) t.crowded++;
        /* The names are copied into pickbuf, so nothing points into the text;
           it is freed after the write all the same, rather than resting on
           that. */
        if (!thWriteFile(dbm, agentPk, pfilePk, all, nf, createMissing, &t)) {
          /* A write that did not run is the database, not this file, so
             grinding through the rest would fail every one of them. The
             files already committed stand and this upload stays retryable. */
          LOG_ERROR("thesmo: upload %d left incomplete at pfile %ld; %d of %d "
              "files were written", uploadPk, pfilePk,
              t.withFindings + t.clean, n);
          free(text);
          break;
        }
        free(text);
      }
      PQclear(rows);
      snprintf(status, sizeof(status),
          "files %d, with findings %d, none found %d, unreadable %d, "
          "already held %d, findings skipped %d, read in part %d, "
          "more findings than room %d",
          n, t.withFindings, t.clean, t.unreadable, t.alreadyHeld, t.degraded,
          t.truncated, t.crowded);
      if (t.degraded)
        LOG_WARNING("thesmo: %d findings skipped for want of a license_ref row",
            t.degraded);
      if (t.truncated)
        LOG_WARNING("thesmo: %d file(s) were longer than the %u MiB read cap "
            "and were scanned as far as it; a license past the boundary is "
            "not reported and reads as a file with none",
            t.truncated, 4u);
      if (t.crowded)
        LOG_WARNING("thesmo: %d file(s) name more licenses than the %d a "
            "finding set holds, so their answer is shorter than the file's",
            t.crowded, TH_MAX_FINDINGS);
      /* Completed only where every selected file is accounted for. A failed
         write leaves ars_success false, which is what CheckARS reads, so the
         upload is offered again -- and the rolled-back files are the ones it
         is offered, because a committed file holds rows and is filtered out. */
      if (t.failed) {
        char failed[320];
        snprintf(failed, sizeof(failed), "incomplete: a write did not run; %s",
            status);
        fo_WriteARS(conn, arsPk, uploadPk, agentPk, AGENT_ARS, failed, 0);
      } else {
        fo_WriteARS(conn, arsPk, uploadPk, agentPk, AGENT_ARS, status, 1);
      }
    }
    thFree(m);
    fo_scheduler_disconnect(0);
    return 0;
  }
#else
  if (scheduler) {
    fprintf(stderr, "thesmo: built without FOSSology support, "
                    "--scheduler_start is unavailable\n");
    return 1;
  }
#endif

  if (optind >= argc) { usage(argv[0]); return 1; }
  m = thLoad(modelDir, err, sizeof(err));
  if (!m) {
    fprintf(stderr, "thesmo: cannot load model from %s: %s\n", modelDir, err);
    return 1;
  }
  if (verbose)
    fprintf(stderr, "thesmo: detector %s.%s.m%s, model %s, %d classes\n",
        VERSION_S, COMMIT_HASH_S, m->digest, m->version, m->nClasses);
  if (!m->haveVerifier)
    fprintf(stderr, "thesmo: warning: %s has no verifier.json; "
        "scoring falls back to margin bands\n", modelDir);

  if (json) printf("[\n");
  for (i = optind; i < argc; i++) {
    int cut = 0, dropped = 0;
    char *text = thSlurp(argv[i], 4u * 1024 * 1024, &cut);
    thResult r;
    if (!text) { fprintf(stderr, "thesmo: %s is not readable text\n", argv[i]); continue; }
    char pickbuf[1024];
    thResult all[TH_MAX_FINDINGS];
    int nf = thClassifyAll(m, text, all, TH_MAX_FINDINGS, pickbuf,
                           sizeof(pickbuf), &dropped);
    r = nf > 0 ? all[0] : (thResult){0};
    if (nf == 0) thClassifyFile(m, text, &r, pickbuf, sizeof(pickbuf));
    if (json) {
      if (!first) printf(",\n");
      first = 0;
      if (nf == 0) {
        printf("  {\"file\":\"");
        thJsonPuts(argv[i]);
        printf("\",\"engine\":\"thesmo\",\"model_version\":\"%s\","
               "\"detector_id\":\"%s\","
               "\"license\":null,\"status\":\"UNKNOWN\",\"gate\":%.3f,\"margin\":%.3f,"
               "\"match_pct\":0,\"exception\":null,\"method\":\"%s\","
               "\"read_in_part\":%s,\"findings_dropped\":%d}",
               m->version, m->digest, r.gate, r.margin,
               r.method ? r.method : "none",
               cut ? "true" : "false", dropped);
      } else {
        int k;
        for (k = 0; k < nf; k++) {
          if (k) printf(",\n");
          printf("  {\"file\":\"");
          thJsonPuts(argv[i]);
          printf("\",\"engine\":\"thesmo\",\"model_version\":\"%s\","
                 "\"detector_id\":\"%s\","
                 "\"license\":\"%s\",\"status\":\"%s\",\"gate\":%.3f,\"margin\":%.3f,"
                 "\"match_pct\":%d,\"start\":%ld,\"len\":%ld,"
                 "\"exception\":%s%s%s,\"method\":\"%s\","
                 "\"read_in_part\":%s,\"findings_dropped\":%d}",
                 m->version, m->digest, all[k].license, all[k].status, all[k].gate,
                 all[k].margin, all[k].score, all[k].start, all[k].len,
                 all[k].exception ? "\"" : "", all[k].exception ? all[k].exception : "null",
                 all[k].exception ? "\"" : "", all[k].method,
                 cut ? "true" : "false", dropped);
        }
      }
    } else {
      if (nf > 0) {
        int k;
        printf("File %s contains license(s) ", argv[i]);
        for (k = 0; k < nf; k++)
          printf("%s%s", k ? "," : "", all[k].license);
        printf("\n");
      } else {
        printf("File %s contains license(s) No_license_found\n", argv[i]);
      }
      if (cut)
        fprintf(stderr, "thesmo: %s was read as far as the %u MiB cap; a "
            "license past it is not reported\n", argv[i], 4u);
      if (dropped)
        fprintf(stderr, "thesmo: %s names more licenses than the %d reported\n",
            argv[i], nf);
      if (verbose) {
        int k;
        for (k = 0; k < nf; k++)
          printf("    %-28s score %3d (%s)  gate %.3f  margin %.3f  %s\n",
                 all[k].license, all[k].score, all[k].status, all[k].gate,
                 all[k].margin, all[k].method);
        if (!nf)
          printf("    none  gate %.3f  margin %.3f  method %s\n",
                 r.gate, r.margin, r.method ? r.method : "-");
      }
    }
    free(text);
  }
  if (json) printf("\n]\n");
  thFree(m);
  return 0;
}
