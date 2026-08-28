/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief Database access for the Thesmo agent
 */
#ifndef THESMO_DB_H
#define THESMO_DB_H

#include <libfossdbmanager.h>

/**
 * \brief What became of a statement.
 *
 * A row that is not there and a statement that did not run are different
 * facts, and collapsing both to an id of zero makes a broken connection
 * look like an unseeded shortname: the agent then skips the finding and
 * tells the operator to run the seeder. The first costs one finding by
 * policy, the second must fail the file.
 */
typedef enum {
  TH_DB_OK = 0,  /**< the statement ran and the row is there */
  TH_DB_ABSENT,  /**< the statement ran and there is no such row */
  TH_DB_FAILED   /**< the statement did not run */
} thDbStatus;

/**
 * \brief The agent row for this exact engine and model, created if absent.
 *
 * `fo_GetAgentKey` takes a revision and uses it in one place only: the INSERT
 * it runs when no row carries the agent's name at all. Every run after the
 * first selects `WHERE agent_name = $1 ORDER BY agent_ts DESC LIMIT 1`, so a
 * new revision reuses the old row and a changed detector is indistinguishable
 * from the one before it -- which makes the ARS check say the upload was
 * already scanned and the file selection skip exactly the files whose answers
 * have changed. It also rewrites the stored description in place when it
 * differs, editing history.
 *
 * This resolves (agent_name, agent_rev) exactly, which is what the schema
 * already means by an agent: `agent_unique_name_rev` is a unique constraint
 * over that pair. Shared with no other agent, so `fo_GetAgentKey` keeps the
 * behaviour its other callers were written against.
 *
 * \return agent_pk, or 0 where the row could not be resolved
 */
long thDbAgentId(fo_dbManager *dbm, const char *name, const char *rev,
    const char *desc, thDbStatus *st);

/**
 * \brief Look up a license shortname.
 *
 * \param st set to TH_DB_ABSENT where the name is not in license_ref, or to
 *           TH_DB_FAILED where the query did not run. May be NULL.
 * \return rf_pk, or 0 when the name is absent or the query failed
 */
long thDbLicenseId(fo_dbManager *dbm, const char *shortname, thDbStatus *st);

/**
 * \brief Create a license_ref row for an unseeded shortname.
 *
 * Rows minted this way carry no license text, so monk cannot use them. Thesmo
 * declares its vocabulary in model.json and seeds ahead of time, so this is
 * behind --create-missing and off by default.
 *
 * rf_shortname carries no unique constraint, so two workers minting the same
 * name at once can both insert. Minting inside the file's transaction keeps a
 * rolled-back file from leaving the row behind, but does not make the pair of
 * workers agree.
 *
 * \return rf_pk of the new or existing row, or 0 on failure
 */
long thDbCreateLicense(fo_dbManager *dbm, const char *shortname,
    thDbStatus *st);

/**
 * \brief Record one finding.
 *
 * `matchPct` is the calibrated confidence scaled to 0-100 and stored in
 * rf_match_pct, the column monk already populates.
 *
 * \return fl_pk, or 0 where the insert did not run
 */
long thDbInsertFinding(fo_dbManager *dbm, long rfPk, int agentPk, long pfilePk,
    int matchPct, thDbStatus *st);

/**
 * \brief Record that a file was scanned and names no license.
 *
 * A license_file row with a null rf_fk is what the schema means by it: "if
 * NULL this file does not contain a license". Without one, a file with
 * nothing in it is indistinguishable from a file never scanned, and
 * getSelectedPFiles offers it again on every run.
 *
 * \return 1 on success
 */
int thDbInsertNoFinding(fo_dbManager *dbm, int agentPk, long pfilePk,
    thDbStatus *st);

/**
 * \brief Record where in the file a finding was read.
 *
 * The single-file view links a finding to the text that produced it.
 * \return 1 on success
 */
int thDbInsertHighlight(fo_dbManager *dbm, long flPk, long start, long len,
    thDbStatus *st);

/**
 * \brief Whether this agent already holds rows for this file.
 *
 * The file list is filtered the same way when it is drawn, so this is the
 * late look that narrows the window on a pfile two uploads are scanning at
 * once. It does not close it: both workers can look before either commits.
 *
 * \return 1 where rows are already there
 */
int thDbAlreadyScanned(fo_dbManager *dbm, int agentPk, long pfilePk,
    thDbStatus *st);

#endif
