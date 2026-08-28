<?php
/*
 SPDX-FileCopyrightText: © 2026 Siemens AG

 SPDX-License-Identifier: GPL-2.0-only
*/

namespace Fossology\Lib\Dao;

use Fossology\Lib\Data\Clearing\ClearingEventTypes;
use Fossology\Lib\Data\DecisionTypes;
use Fossology\Lib\Db\DbManager;

/**
 * @class ThesmoHighlightBrowserDao
 * @brief Agent-decided files grouped by the thesmo highlight behind them.
 *
 * Every reader shares coveredSql() so the files shown and the files written
 * cannot drift apart.
 */
class ThesmoHighlightBrowserDao
{
  /**
   * Mirrors DeciderAgent::RULES_THESMO_NO_CONTRADICTION, which lives in the
   * agent and is not loadable from the UI. The two must not drift.
   */
  const RULE_THESMO_NO_CONTRADICTION = 0x400;

  /** @var DbManager */
  private $dbManager;
  /** @var bool[] upload id to whether the rule was used */
  private $ruleUsed = array();
  /** @var UploadDao */
  private $uploadDao;
  /** @var AgentDao */
  private $agentDao;

  public function __construct(DbManager $dbManager, UploadDao $uploadDao,
                              AgentDao $agentDao)
  {
    $this->dbManager = $dbManager;
    $this->uploadDao = $uploadDao;
    $this->agentDao = $agentDao;
  }

  /**
   * @brief Whether the decider ran the thesmo rule on this upload.
   *
   * Only the job's rule set is recorded, so this answers "was the rule in
   * play", not "did it conclude this file".
   *
   * @param int $uploadId
   * @return bool
   */
  public function thesmoRuleWasUsed($uploadId)
  {
    $uploadId = intval($uploadId);
    if ($uploadId <= 0) {
      return false;
    }
    if (array_key_exists($uploadId, $this->ruleUsed)) {
      return $this->ruleUsed[$uploadId];
    }
    $row = $this->dbManager->getSingleRow("
      SELECT 1 AS used
      FROM jobqueue jq
      INNER JOIN job j ON j.job_pk = jq.jq_job_fk
      WHERE j.job_upload_fk = $1
        AND jq.jq_type = 'decider'
        -- 1 is Completed; a run that failed, or has not finished, has decided
        -- nothing this page could show
        AND jq.jq_end_bits = 1
        AND (coalesce(substring(jq.jq_cmd_args from '-r([0-9]+)'), '0')::int
             & $2) <> 0
      LIMIT 1",
      array($uploadId, self::RULE_THESMO_NO_CONTRADICTION),
      __METHOD__);
    $this->ruleUsed[$uploadId] = !empty($row);
    return $this->ruleUsed[$uploadId];
  }

  /**
   * @brief The thesmo run whose findings this upload currently shows.
   *
   * A rescanned upload keeps the older run's rows, so the name alone would
   * count one notice once per run.
   *
   * @param int $uploadId
   * @return int|null
   */
  public function latestThesmoAgent($uploadId)
  {
    $map = $this->agentDao->getLatestAgentResultForUpload($uploadId,
      array('thesmo'));
    return array_key_exists('thesmo', $map) && !empty($map['thesmo'])
      ? intval($map['thesmo']) : null;
  }

  /**
   * @brief Every (license, highlight) pair with the file and offset it is at.
   *
   * Clearing events accumulate, so only a file's newest decision counts.
   * Parameters are $1 upload, $2 group, $3 event type, $4 agent, $5 WIP,
   * $6 IRRELEVANT; a caller may append its own.
   *
   * @param string $treeTable
   * @return string
   */
  private function coveredSql($treeTable)
  {
    return "
      WITH newest_decision AS (
        SELECT DISTINCT ON (ut.uploadtree_pk)
               cd.clearing_decision_pk AS decision_id,
               ut.uploadtree_pk, ut.pfile_fk, ut.ufile_name, cd.decision_type
        FROM clearing_decision cd
        INNER JOIN $treeTable ut ON ut.uploadtree_pk = cd.uploadtree_fk
        WHERE cd.group_fk = $2 AND ut.upload_fk = $1
        ORDER BY ut.uploadtree_pk, cd.clearing_decision_pk DESC
      ), latest_valid_decision AS (
        -- judged after the pick: rejecting WIP first would let an older
        -- decision stand in for one a reviewer has reopened
        SELECT * FROM newest_decision
        WHERE decision_type != $5 AND decision_type != $6
      ), decided AS (
        SELECT DISTINCT lvd.uploadtree_pk, lvd.pfile_fk, lvd.ufile_name,
               ce.rf_fk,
               coalesce(ce.acknowledgement, '') AS ack,
               coalesce(ce.comment, '')         AS comment,
               coalesce(ce.reportinfo, '')      AS reportinfo
        FROM latest_valid_decision lvd
        INNER JOIN clearing_decision_event cde
          ON cde.clearing_decision_fk = lvd.decision_id
        INNER JOIN clearing_event ce
          ON ce.clearing_event_pk = cde.clearing_event_fk
        WHERE ce.type_fk = $3
          AND (ce.removed IS NULL OR ce.removed = FALSE)
      ), covered AS (
        -- One highlight per (file, license), because that is how the text is
        -- stored: two length groups over one file would share one row and the
        -- second save would discard the first. Longest span wins.
        SELECT DISTINCT ON (d.uploadtree_pk, d.rf_fk)
               d.rf_fk, h.len, d.uploadtree_pk, d.ufile_name,
               h.start, d.ack, d.comment, d.reportinfo
        FROM decided d
        INNER JOIN license_file lf ON lf.pfile_fk = d.pfile_fk
                                  AND lf.rf_fk    = d.rf_fk
                                  AND lf.agent_fk = $4
        INNER JOIN highlight h     ON h.fl_fk = lf.fl_pk
        ORDER BY d.uploadtree_pk, d.rf_fk, h.len DESC, h.start
      )";
  }

  /**
   * @return array the fixed parameters `coveredSql` reads
   */
  private function coveredParams($uploadId, $groupId, $agentId)
  {
    return array($uploadId, $groupId, ClearingEventTypes::AGENT, $agentId,
      DecisionTypes::WIP, DecisionTypes::IRRELEVANT);
  }

  /**
   * @brief One row per highlight, with the files it covers.
   *
   * Grouped by length, not offset: start moves with the copyright line above
   * the notice, so grouping on it splits one text into one row per file. The
   * sample item and its offset come from one row, never aggregated apart.
   *
   * @param int $uploadId
   * @param int $groupId
   * @param int $agentId
   * @return array[]
   */
  public function getHighlightGroups($uploadId, $groupId, $agentId)
  {
    $treeTable = $this->uploadDao->getUploadtreeTableName($uploadId);
    $sql = $this->coveredSql($treeTable) . ", sample AS (
        SELECT DISTINCT ON (rf_fk, len) rf_fk, len, uploadtree_pk, start
        FROM covered ORDER BY rf_fk, len, uploadtree_pk, start
      )
      SELECT lr.rf_pk, lr.rf_shortname, c.len,
             count(DISTINCT c.uploadtree_pk) AS files,
             min(s.uploadtree_pk)            AS sample_item,
             min(s.start)                    AS sample_start,
             count(DISTINCT c.ack)           AS ack_variants,
             min(c.ack)                      AS ack,
             count(DISTINCT c.comment)       AS comment_variants,
             min(c.comment)                  AS comment,
             count(DISTINCT c.reportinfo)    AS reportinfo_variants,
             min(c.reportinfo)               AS reportinfo
      FROM covered c
      INNER JOIN sample s       ON s.rf_fk = c.rf_fk AND s.len = c.len
      INNER JOIN license_ref lr ON lr.rf_pk = c.rf_fk
      GROUP BY lr.rf_pk, lr.rf_shortname, c.len
      ORDER BY files DESC, lr.rf_shortname, c.len";

    $statement = __METHOD__ . ".$treeTable";
    $this->dbManager->prepare($statement, $sql);
    $result = $this->dbManager->execute($statement,
      $this->coveredParams($uploadId, $groupId, $agentId));
    $groups = array();
    while ($row = $this->dbManager->fetchArray($result)) {
      $groups[] = array(
        'licenseId'    => intval($row['rf_pk']),
        'shortName'    => $row['rf_shortname'],
        'len'          => intval($row['len']),
        'files'        => intval($row['files']),
        'sampleItem'   => intval($row['sample_item']),
        'sampleStart'  => intval($row['sample_start']),
        'ack'          => intval($row['ack_variants']) === 1 ? $row['ack'] : '',
        'ackMixed'     => intval($row['ack_variants']) > 1,
        'comment'      => intval($row['comment_variants']) === 1
                            ? $row['comment'] : '',
        'commentMixed' => intval($row['comment_variants']) > 1,
        'licenseText'  => intval($row['reportinfo_variants']) === 1
                            ? $row['reportinfo'] : '',
        'licenseTextMixed' => intval($row['reportinfo_variants']) > 1,
      );
    }
    $this->dbManager->freeResult($result);
    return $groups;
  }

  /**
   * @brief Agent-decided files this page cannot show, by concluded license.
   *
   * A pointer such as Same-license-as carries no span, so it has no highlight
   * to acknowledge. Reported so the count is not short of the upload's total.
   *
   * @param int $uploadId
   * @param int $groupId
   * @param int $agentId
   * @return array[] rf_shortname and files, commonest first
   */
  public function getUnhighlighted($uploadId, $groupId, $agentId)
  {
    $treeTable = $this->uploadDao->getUploadtreeTableName($uploadId);
    $sql = $this->coveredSql($treeTable) . "
      SELECT lr.rf_shortname, count(DISTINCT d.uploadtree_pk) AS files
      FROM decided d
      INNER JOIN license_ref lr ON lr.rf_pk = d.rf_fk
      WHERE NOT EXISTS (SELECT 1 FROM covered c
                        WHERE c.uploadtree_pk = d.uploadtree_pk
                          AND c.rf_fk        = d.rf_fk)
      GROUP BY lr.rf_shortname
      ORDER BY files DESC, lr.rf_shortname";
    $statement = __METHOD__ . ".$treeTable";
    $this->dbManager->prepare($statement, $sql);
    $result = $this->dbManager->execute($statement,
      $this->coveredParams($uploadId, $groupId, $agentId));
    $rows = array();
    while ($row = $this->dbManager->fetchArray($result)) {
      $rows[] = array('shortName' => $row['rf_shortname'],
                      'files' => intval($row['files']));
    }
    $this->dbManager->freeResult($result);
    return $rows;
  }

  /**
   * @brief The files one highlight group covers, by uploadtree id and name.
   * @param int $uploadId
   * @param int $groupId
   * @param int $agentId
   * @param int $licenseId
   * @param int $len
   * @return array[]
   */
  public function getItemsInGroup($uploadId, $groupId, $agentId, $licenseId,
                                  $len)
  {
    $treeTable = $this->uploadDao->getUploadtreeTableName($uploadId);
    $sql = $this->coveredSql($treeTable) . "
      SELECT DISTINCT uploadtree_pk, ufile_name
      FROM covered WHERE rf_fk = $7 AND len = $8
      ORDER BY ufile_name";
    $statement = __METHOD__ . ".$treeTable";
    $this->dbManager->prepare($statement, $sql);
    $params = $this->coveredParams($uploadId, $groupId, $agentId);
    $params[] = $licenseId;
    $params[] = $len;
    $result = $this->dbManager->execute($statement, $params);
    $rows = $this->dbManager->fetchAll($result);
    $this->dbManager->freeResult($result);
    return $rows === false ? array() : $rows;
  }

  /**
   * @brief Write the three texts across the given items.
   *
   * The list is bound and expanded in SQL, never interpolated: a statement is
   * cached under its name, so an interpolated list would leave every later
   * save writing to the first group's files.
   *
   * @param int[] $items uploadtree ids
   * @param int $groupId
   * @param int $licenseId
   * @param string $acknowledgement
   * @param string $comment
   * @param string $licenseText the reportinfo column, labeled License Text
   * @return int how many items were addressed
   */
  public function setTextsForItems($items, $groupId, $licenseId,
                                 $acknowledgement, $comment, $licenseText)
  {
    $ids = array();
    foreach ($items as $item) {
      $ids[] = intval($item);
    }
    if (empty($ids)) {
      return 0;
    }
    $statement = __METHOD__;
    $this->dbManager->prepare($statement, "
      UPDATE clearing_event
      SET acknowledgement = $3, comment = $4, reportinfo = $7
      WHERE uploadtree_fk = ANY(string_to_array($6, ',')::bigint[])
        AND group_fk = $1
        AND rf_fk    = $2
        AND type_fk  = $5
        AND (removed IS NULL OR removed = FALSE)");
    $result = $this->dbManager->execute($statement, array($groupId, $licenseId,
      $acknowledgement, $comment, ClearingEventTypes::AGENT,
      implode(',', $ids), $licenseText));
    $this->dbManager->freeResult($result);
    return count($ids);
  }
}
