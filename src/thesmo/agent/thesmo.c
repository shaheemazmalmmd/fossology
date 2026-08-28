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
#define AGENT_NAME "thesmo"
#define AGENT_ARS  "thesmo_ars"
#endif

#ifndef DATADIR
#define DATADIR "/usr/local/share/fossology"
#endif
#ifndef VERSION_S
#define VERSION_S "0.0.0"
#endif

/**
 * \brief Read a file, refusing anything that is not text.
 *
 * Binary bytes still form character n-grams and enough of them land on some
 * license to win an argmax. Nothing readable carries a NUL in its first block.
 */
static char *thSlurp(const char *p, size_t cap)
{
  FILE *f = fopen(p, "rb");
  char *b;
  size_t n, probe, i;
  if (!f) return NULL;
  b = malloc(cap + 1);
  if (!b) { fclose(f); return NULL; }
  n = fread(b, 1, cap, f);
  b[n] = '\0';
  fclose(f);
  probe = n < 8192 ? n : 8192;
  for (i = 0; i < probe; i++)
    if (b[i] == '\0') { free(b); return NULL; }
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
         "  -h      :: this help\n", me, DATADIR);
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

#ifdef FOSSOLOGY_AGENT
  if (scheduler) {
    fo_dbManager *dbm = NULL;
    PGconn *conn;
    int agentPk;
    int missing = 0;
    fo_scheduler_connect_dbMan(&argc, argv, &dbm);
    conn = fo_dbManager_getWrappedConnection(dbm);
    agentPk = fo_GetAgentKey(conn, AGENT_NAME, 0, VERSION_S,
        "Thesmo licence-reference detector");
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
    /* Without verifier.json the fallback margin bands are a weaker gate at the
       same number, so a partial model shows up only as lost precision. */
    if (!m->haveVerifier)
      LOG_WARNING("thesmo: %s has no verifier.json; scoring falls back to "
          "margin bands and min_score %d is not calibrated for them",
          modelDir, m->minScore);
    /* An unseeded name is a packaging gap, not a scan-time surprise, and costs
       its own findings rather than the upload. */
    for (i = 0; i < m->nClasses; i++)
      if (thDbLicenseId(dbm, m->classes[i]) == 0) {
        if (createMissing) {
          if (thDbCreateLicense(dbm, m->classes[i]) == 0)
            LOG_ERROR("thesmo: cannot create license_ref row for '%s'", m->classes[i]);
        } else {
          LOG_WARNING("thesmo: shortname '%s' is not in license_ref; its "
              "findings will be skipped", m->classes[i]);
          missing++;
        }
      }
    if (missing)
      LOG_WARNING("thesmo: %d of %d model classes have no license_ref row and "
          "their findings will be skipped; run "
          "'php %s/thesmo/install/thesmo-seed.php %s/model.json', or pass "
          "--create-missing to have the agent insert them itself",
          missing, m->nClasses, DATADIR, modelDir);
    while (fo_scheduler_next()) {
      int uploadPk = atoi(fo_scheduler_current());
      PGresult *rows;
      int n, k, arsPk = 0, skipped = 0;
      if (uploadPk == 0) continue;
      if (GetUploadPerm(conn, uploadPk, fo_scheduler_userID()) < PERM_WRITE) {
        LOG_ERROR("You have no update permissions on upload %d", uploadPk);
        continue;
      }
      arsPk = fo_WriteARS(conn, arsPk, uploadPk, agentPk, AGENT_ARS, 0, 0);
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
        fo_scheduler_heart(1);
        if (!repFile) continue;
        text = thSlurp(repFile, 4u * 1024 * 1024);
        free(repFile);
        if (!text) continue; /* unreadable, or not text */
        {
          thResult all[TH_MAX_FINDINGS];
          int nf = thClassifyAll(m, text, all, TH_MAX_FINDINGS, pickbuf, sizeof(pickbuf));
          int f;
          for (f = 0; f < nf; f++) {
            /* SPDX writes the pair as one expression and FOSSology stores it
               as one license_ref row spelled the same way. */
            char named[512];
            const char *shortname = all[f].license;
            long rfPk;
            if (all[f].exception) {
              snprintf(named, sizeof(named), "%s WITH %s",
                       all[f].license, all[f].exception);
              shortname = named;
            }
            rfPk = thDbLicenseId(dbm, shortname);
            if (!rfPk && all[f].exception) {
              /* Pairing two known names is not minting an unknown license, so
                 it needs no --create-missing. Fall back to the license. */
              if (thDbLicenseId(dbm, all[f].license)
                  && thDbLicenseId(dbm, all[f].exception))
                rfPk = thDbCreateLicense(dbm, shortname);
              if (!rfPk)
                rfPk = createMissing ? thDbCreateLicense(dbm, all[f].license)
                                     : thDbLicenseId(dbm, all[f].license);
            } else if (!rfPk && createMissing) {
              rfPk = thDbCreateLicense(dbm, shortname);
            }
            if (rfPk) {
              long flPk = thDbInsertFinding(dbm, rfPk, agentPk, pfilePk,
                                            all[f].score);
              if (flPk && all[f].len > 0)
                thDbInsertHighlight(dbm, flPk, all[f].start, all[f].len);
            } else {
              skipped++;
            }
          }
        }
        free(text);
      }
      PQclear(rows);
      if (skipped)
        LOG_WARNING("thesmo: %d findings skipped for want of a license_ref row",
            skipped);
      fo_WriteARS(conn, arsPk, uploadPk, agentPk, AGENT_ARS, 0, 1);
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
  if (verbose) fprintf(stderr, "thesmo: model %s, %d classes\n", m->version, m->nClasses);
  if (!m->haveVerifier)
    fprintf(stderr, "thesmo: warning: %s has no verifier.json; "
        "scoring falls back to margin bands\n", modelDir);

  if (json) printf("[\n");
  for (i = optind; i < argc; i++) {
    char *text = thSlurp(argv[i], 4u * 1024 * 1024);
    thResult r;
    if (!text) { fprintf(stderr, "thesmo: %s is not readable text\n", argv[i]); continue; }
    char pickbuf[1024];
    thResult all[TH_MAX_FINDINGS];
    int nf = thClassifyAll(m, text, all, TH_MAX_FINDINGS, pickbuf, sizeof(pickbuf));
    r = nf > 0 ? all[0] : (thResult){0};
    if (nf == 0) thClassifyFile(m, text, &r, pickbuf, sizeof(pickbuf));
    if (json) {
      if (!first) printf(",\n");
      first = 0;
      if (nf == 0) {
        printf("  {\"file\":\"");
        thJsonPuts(argv[i]);
        printf("\",\"engine\":\"thesmo\",\"model_version\":\"%s\","
               "\"license\":null,\"status\":\"UNKNOWN\",\"gate\":%.3f,\"margin\":%.3f,"
               "\"match_pct\":0,\"exception\":null,\"method\":\"%s\"}",
               m->version, r.gate, r.margin, r.method ? r.method : "none");
      } else {
        int k;
        for (k = 0; k < nf; k++) {
          if (k) printf(",\n");
          printf("  {\"file\":\"");
          thJsonPuts(argv[i]);
          printf("\",\"engine\":\"thesmo\",\"model_version\":\"%s\","
                 "\"license\":\"%s\",\"status\":\"%s\",\"gate\":%.3f,\"margin\":%.3f,"
                 "\"match_pct\":%d,\"start\":%ld,\"len\":%ld,"
                 "\"exception\":%s%s%s,\"method\":\"%s\"}",
                 m->version, all[k].license, all[k].status, all[k].gate,
                 all[k].margin, all[k].score, all[k].start, all[k].len,
                 all[k].exception ? "\"" : "", all[k].exception ? all[k].exception : "null",
                 all[k].exception ? "\"" : "", all[k].method);
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
