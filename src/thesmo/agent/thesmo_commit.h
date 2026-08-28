/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief What is written for one file, and when the upload may claim it
 */
#ifndef THESMO_COMMIT_H
#define THESMO_COMMIT_H

#include "thesmo_db.h"
#include "thesmo_model.h"

/** What one upload's files came to.
 *
 * Every selected file lands in exactly one of these, so the upload can be
 * completed on an accounted-for outcome rather than on having reached the
 * end of the loop.
 */
typedef struct {
  int withFindings;  /**< scanned, at least one row written */
  int clean;         /**< scanned, named no license */
  int unreadable;    /**< no repository path, or not text */
  int truncated;     /**< longer than the read cap, so read in part */
  int crowded;       /**< more findings than there was room to report */
  int alreadyHeld;   /**< another worker had already written it */
  int degraded;      /**< findings dropped for want of a license_ref row */
  int failed;        /**< a required write did not run; rolled back */
} thTally;


/**
 * \brief Write one file's findings, all of them or none.
 *
 * \param t counted into, one file per call
 * \return 1 where the file is committed and accounted for; 0 where a
 *         required write did not run and the file was rolled back
 */
int thWriteFile(fo_dbManager *dbm, int agentPk, long pfilePk,
    const thResult *all, int nf, int createMissing, thTally *t);

#endif
