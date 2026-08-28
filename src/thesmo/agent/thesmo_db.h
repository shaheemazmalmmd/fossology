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
 * \brief Look up a license shortname.
 *
 * \return rf_pk, or 0 when the name is absent
 */
long thDbLicenseId(fo_dbManager *dbm, const char *shortname);

/**
 * \brief Create a license_ref row for an unseeded shortname.
 *
 * Rows minted this way carry no license text, so monk cannot use them. Thesmo
 * declares its vocabulary in model.json and seeds ahead of time, so this is
 * behind --create-missing and off by default.
 *
 * \return rf_pk of the new or existing row, or 0 on failure
 */
long thDbCreateLicense(fo_dbManager *dbm, const char *shortname);

/**
 * \brief Record one finding.
 *
 * `matchPct` is the calibrated confidence scaled to 0-100 and stored in
 * rf_match_pct, the column monk already populates.
 */
/**
 * \brief Record where in the file a finding was read.
 *
 * The single-file view links a finding to the text that produced it.
 * \return 1 on success
 */
int thDbInsertHighlight(fo_dbManager *dbm, long flPk, long start, long len);

long thDbInsertFinding(fo_dbManager *dbm, long rfPk, int agentPk, long pfilePk,
    int matchPct);

#endif
