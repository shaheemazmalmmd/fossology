<?php
/*
 SPDX-FileCopyrightText: © 2026 Siemens AG

 SPDX-License-Identifier: GPL-2.0-only
*/

namespace Fossology\Lib\Dao;

use Fossology\Lib\Data\Clearing\ClearingEventTypes;
use Fossology\Lib\Data\DecisionTypes;
use Fossology\Lib\Test\TestPgDb;
use Mockery as M;

/**
 * @class ThesmoHighlightBrowserDaoTest
 * @brief What the highlight grouping must and must not do.
 *
 * Every case here is a mistake this query made while it was written, so each
 * fails if the fix for it is undone:
 *
 *  - one notice at different offsets is one row, not one row per file;
 *  - the sample file and the sample offset belong to the same file;
 *  - an event from a superseded decision is not read;
 *  - an IRRELEVANT decision is not read;
 *  - `removed` reads as unset when null, which the column permits;
 *  - files disagreeing on a text are reported, not flattened to one of them;
 *  - the group listing and the file list are the same set of files.
 */
class ThesmoHighlightBrowserDaoTest extends \PHPUnit\Framework\TestCase
{
  /** @var TestPgDb */
  private $testDb;
  /** @var DbManager */
  private $dbManager;
  /** @var ThesmoHighlightBrowserDao */
  private $browserDao;

  const ITEM_A = 101;   // the notice at offset 90 (lowest id, higher start)
  const ITEM_B = 102;   // the same notice at offset 40 -- the grouping case
  const ITEM_C = 103;   // an older decision superseded by a newer one
  const ITEM_D = 104;   // IRRELEVANT
  const ITEM_E = 105;   // removed IS NULL
  const ITEM_F = 106;   // disagrees on the acknowledgement text
  const UPLOAD = 7;
  const GROUP = 2;
  const AGENT_THESMO = 55;
  const RF_GPL = 3;
  const RF_MIT = 4;     // only ever on ITEM_C's superseded decision
  const ITEM_G = 107;   // two highlights of one license: shares one event row
  const ITEM_H = 108;   // newest decision is WIP, with an older one behind it
  const ITEM_I = 109;   // one license highlighted, one not: the footer case
  const NOTICE_LEN = 177;

  protected function setUp(): void
  {
    $this->testDb = new TestPgDb("thesmoack");
    $this->dbManager = &$this->testDb->getDbManager();
    $this->testDb->createPlainTables(array('upload', 'uploadtree', 'pfile',
      'license_file', 'license_ref', 'highlight', 'clearing_decision',
      'clearing_decision_event', 'clearing_event', 'agent'));
    $this->insertFixture();

    $uploadDao = M::mock(UploadDao::class);
    $uploadDao->shouldReceive('getUploadtreeTableName')->andReturn('uploadtree');
    $agentDao = M::mock(AgentDao::class);
    $agentDao->shouldReceive('getLatestAgentResultForUpload')
      ->andReturn(array('thesmo' => self::AGENT_THESMO));

    $this->browserDao = new ThesmoHighlightBrowserDao($this->dbManager, $uploadDao,
      $agentDao);
  }

  protected function tearDown(): void
  {
    $this->testDb->fullDestruct();
    $this->testDb = null;
    $this->dbManager = null;
    M::close();
  }

  /**
   * @brief Six files, each one a case the query has to tell apart.
   */
  private function insertFixture()
  {
    $this->dbManager->insertInto('license_ref', 'rf_pk, rf_shortname',
      array(self::RF_GPL, 'GPL-2.0-or-later'));
    $this->dbManager->insertInto('license_ref', 'rf_pk, rf_shortname',
      array(self::RF_MIT, 'MIT'));
    $this->dbManager->insertInto('agent',
      'agent_pk, agent_name, agent_enabled',
      array(self::AGENT_THESMO, 'thesmo', $this->dbManager->booleanToDb(true)));

    // The notice is one length everywhere; only its offset moves, because the
    // copyright line above it is a different length in each file.
    // ITEM_A has the lowest id and deliberately NOT the lowest offset, so
    // aggregating the sample item and offset apart produces a pair that
    // belongs to no file.
    $offsets = array(self::ITEM_A => 90, self::ITEM_B => 40,
      self::ITEM_C => 40, self::ITEM_D => 40, self::ITEM_E => 40,
      self::ITEM_F => 40);
    $flPk = 500;
    foreach ($offsets as $item => $start) {
      $this->dbManager->insertInto('pfile', 'pfile_pk, pfile_size',
        array($item, 1000));
      $this->dbManager->insertInto('uploadtree',
        'uploadtree_pk, upload_fk, pfile_fk, ufile_name',
        array($item, self::UPLOAD, $item, "f$item.c"));
      $this->dbManager->insertInto('license_file',
        'fl_pk, rf_fk, pfile_fk, agent_fk',
        array($flPk, self::RF_GPL, $item, self::AGENT_THESMO));
      $this->dbManager->insertInto('highlight', 'fl_fk, start, len, type',
        array($flPk, $start, self::NOTICE_LEN, 'L'));
      $flPk++;
    }

    $identified = DecisionTypes::IDENTIFIED;
    $this->decide(1, self::ITEM_A, $identified, 1, 'ack', false);
    $this->decide(2, self::ITEM_B, $identified, 2, 'ack', false);
    // C's first decision names MIT and is superseded by its second, which
    // names GPL. Reading superseded decisions would add an MIT row.
    $this->dbManager->insertInto('license_file',
      'fl_pk, rf_fk, pfile_fk, agent_fk',
      array(600, self::RF_MIT, self::ITEM_C, self::AGENT_THESMO));
    $this->dbManager->insertInto('highlight', 'fl_fk, start, len, type',
      array(600, 40, self::NOTICE_LEN, 'L'));
    $this->decideLicense(3, self::ITEM_C, $identified, 3, self::RF_MIT,
      'stale', false);
    $this->decide(4, self::ITEM_C, $identified, 4, 'ack', false);
    $this->decide(5, self::ITEM_D, DecisionTypes::IRRELEVANT, 5, 'ack', false);
    $this->decide(6, self::ITEM_E, $identified, 6, 'ack', null);
    $this->decide(7, self::ITEM_F, $identified, 7, 'other', false);

    // G carries the notice twice at different lengths. One clearing_event row
    // holds its text, so it must not be split across two groups.
    $this->dbManager->insertInto('pfile', 'pfile_pk, pfile_size',
      array(self::ITEM_G, 1000));
    $this->dbManager->insertInto('uploadtree',
      'uploadtree_pk, upload_fk, pfile_fk, ufile_name',
      array(self::ITEM_G, self::UPLOAD, self::ITEM_G, 'g.c'));
    $this->dbManager->insertInto('license_file',
      'fl_pk, rf_fk, pfile_fk, agent_fk',
      array(700, self::RF_GPL, self::ITEM_G, self::AGENT_THESMO));
    $this->dbManager->insertInto('highlight', 'fl_fk, start, len, type',
      array(700, 40, self::NOTICE_LEN, 'L'));
    $this->dbManager->insertInto('highlight', 'fl_fk, start, len, type',
      array(700, 300, self::NOTICE_LEN + 20, 'L'));
    $this->decide(8, self::ITEM_G, $identified, 8, 'ack', false);

    // H's newest decision is WIP; the one behind it is not.
    $this->dbManager->insertInto('pfile', 'pfile_pk, pfile_size',
      array(self::ITEM_H, 1000));
    $this->dbManager->insertInto('uploadtree',
      'uploadtree_pk, upload_fk, pfile_fk, ufile_name',
      array(self::ITEM_H, self::UPLOAD, self::ITEM_H, 'h.c'));
    $this->dbManager->insertInto('license_file',
      'fl_pk, rf_fk, pfile_fk, agent_fk',
      array(701, self::RF_GPL, self::ITEM_H, self::AGENT_THESMO));
    $this->dbManager->insertInto('highlight', 'fl_fk, start, len, type',
      array(701, 40, self::NOTICE_LEN, 'L'));
    $this->decide(9, self::ITEM_H, $identified, 9, 'ack', false);
    $this->decide(10, self::ITEM_H, DecisionTypes::WIP, 10, 'wip', false);

    // I is concluded twice: GPL is highlighted, MIT is not.
    $this->dbManager->insertInto('pfile', 'pfile_pk, pfile_size',
      array(self::ITEM_I, 1000));
    $this->dbManager->insertInto('uploadtree',
      'uploadtree_pk, upload_fk, pfile_fk, ufile_name',
      array(self::ITEM_I, self::UPLOAD, self::ITEM_I, 'i.c'));
    $this->dbManager->insertInto('license_file',
      'fl_pk, rf_fk, pfile_fk, agent_fk',
      array(702, self::RF_GPL, self::ITEM_I, self::AGENT_THESMO));
    $this->dbManager->insertInto('highlight', 'fl_fk, start, len, type',
      array(702, 40, self::NOTICE_LEN, 'L'));
    // MIT found by thesmo on the same file, but with no highlight
    $this->dbManager->insertInto('license_file',
      'fl_pk, rf_fk, pfile_fk, agent_fk',
      array(703, self::RF_MIT, self::ITEM_I, self::AGENT_THESMO));
    $this->dbManager->insertInto('clearing_decision',
      'clearing_decision_pk, uploadtree_fk, pfile_fk, group_fk, '
      . 'decision_type, scope',
      array(11, self::ITEM_I, self::ITEM_I, self::GROUP, $identified, 0));
    foreach (array(array(11, self::RF_GPL), array(12, self::RF_MIT)) as $ev) {
      $this->dbManager->insertInto('clearing_event',
        'clearing_event_pk, uploadtree_fk, rf_fk, group_fk, user_fk, type_fk, '
        . 'removed, acknowledgement, comment',
        array($ev[0], self::ITEM_I, $ev[1], self::GROUP, 1,
          ClearingEventTypes::AGENT, $this->dbManager->booleanToDb(false),
          'ack', 'c'));
      $this->dbManager->insertInto('clearing_decision_event',
        'clearing_decision_fk, clearing_event_fk', array(11, $ev[0]));
    }
  }

  /**
   * @param int $decisionId
   * @param int $item
   * @param int $type
   * @param int $eventId
   * @param string $ack
   * @param bool|null $removed null exercises the nullable column
   */
  private function decide($decisionId, $item, $type, $eventId, $ack, $removed)
  {
    $this->decideLicense($decisionId, $item, $type, $eventId, self::RF_GPL,
      $ack, $removed);
  }

  /**
   * @brief decide(), for a decision naming a license other than the default.
   */
  private function decideLicense($decisionId, $item, $type, $eventId,
                                 $licenseId, $ack, $removed)
  {
    $this->dbManager->insertInto('clearing_decision',
      'clearing_decision_pk, uploadtree_fk, pfile_fk, group_fk, '
      . 'decision_type, scope',
      array($decisionId, $item, $item, self::GROUP, $type, 0));
    $this->dbManager->insertInto('clearing_event',
      'clearing_event_pk, uploadtree_fk, rf_fk, group_fk, user_fk, type_fk, '
      . 'removed, acknowledgement, comment',
      array($eventId, $item, $licenseId, self::GROUP, 1,
        ClearingEventTypes::AGENT,
        $removed === null ? null : $this->dbManager->booleanToDb($removed),
        $ack, 'c'));
    $this->dbManager->insertInto('clearing_decision_event',
      'clearing_decision_fk, clearing_event_fk', array($decisionId, $eventId));
  }

  /** @return array[] */
  private function groups()
  {
    return $this->browserDao->getHighlightGroups(self::UPLOAD, self::GROUP,
      self::AGENT_THESMO);
  }

  /** @return int[] */
  private function itemsInTheGroup()
  {
    $items = array();
    foreach ($this->browserDao->getItemsInGroup(self::UPLOAD, self::GROUP,
             self::AGENT_THESMO, self::RF_GPL, self::NOTICE_LEN) as $row) {
      $items[] = intval($row['uploadtree_pk']);
    }
    return $items;
  }

  /**
   * @brief Acknowledgements on the newest decision of each given file.
   * @param int[] $items
   * @return string[]
   */
  private function acksOf($items, $licenseId = self::RF_GPL)
  {
    // Bound, not interpolated: a statement cached under one name must not
    // carry a previous call's id list, the hazard this suite found in
    // setTextsForItems.
    $list = implode(',', array_map('intval', $items));
    $rows = $this->dbManager->getRows(
      "SELECT ce.acknowledgement FROM clearing_event ce "
      . "INNER JOIN clearing_decision_event cde "
      . "  ON cde.clearing_event_fk = ce.clearing_event_pk "
      . "INNER JOIN clearing_decision cd "
      . "  ON cd.clearing_decision_pk = cde.clearing_decision_fk "
      . "WHERE ce.uploadtree_fk = ANY(string_to_array($1, ',')::bigint[]) "
      . "  AND ce.rf_fk = $2 "
      . "  AND (ce.removed IS NULL OR ce.removed = FALSE) "
      . "  AND cd.clearing_decision_pk = (SELECT max(clearing_decision_pk) "
      . "      FROM clearing_decision WHERE uploadtree_fk = ce.uploadtree_fk)",
      array($list, $licenseId), __METHOD__);
    $acks = array();
    foreach ($rows as $row) {
      $acks[] = $row['acknowledgement'];
    }
    return $acks;
  }

  /**
   * @brief One notice at two offsets is one row.
   *
   * Grouping on (start, len) would make this two rows, and a table of one row
   * per file is what this query replaces.
   */
  public function testOneNoticeAtTwoOffsetsIsOneRow()
  {
    $items = $this->itemsInTheGroup();
    $this->assertContains(self::ITEM_A, $items,
      "the notice at offset 90 belongs to this group");
    $this->assertContains(self::ITEM_B, $items,
      "and so does the same notice at offset 40: one row, not two");
    $atThisLength = array();
    foreach ($this->groups() as $group) {
      if ($group['len'] == self::NOTICE_LEN) {
        $atThisLength[] = $group;
      }
    }
    $this->assertCount(1, $atThisLength, "one length of one license is one row");
    $this->assertEquals('GPL-2.0-or-later', $atThisLength[0]['shortName']);
  }

  /**
   * @brief The sample offset belongs to the sample file.
   *
   * Taking min(item) and min(start) apart yields the lowest id beside another
   * file's offset, and the sample is the one place the text gets read.
   */
  public function testTheSampleOffsetBelongsToTheSampleFile()
  {
    $groups = $this->groups();
    $row = $this->dbManager->getSingleRow(
      "SELECT h.start FROM uploadtree ut "
      . "INNER JOIN license_file lf ON lf.pfile_fk = ut.pfile_fk "
      . "INNER JOIN highlight h ON h.fl_fk = lf.fl_pk "
      . "WHERE ut.uploadtree_pk = $1",
      array($groups[0]['sampleItem']), __METHOD__);
    $this->assertEquals(intval($row['start']), $groups[0]['sampleStart'],
      "the sample offset must be the sample file's own");
  }

  /**
   * @brief Only a file's newest decision is read.
   */
  public function testASupersededEventIsNotRead()
  {
    $this->assertContains(self::ITEM_C, $this->itemsInTheGroup(),
      "the file's newest decision still counts");
    $names = array();
    foreach ($this->groups() as $group) {
      $names[] = $group['shortName'];
    }
    $this->assertNotContains('MIT', $names,
      "a license only the superseded decision names must not appear");
    $this->assertNotContains('stale', $this->acksOf(array(self::ITEM_C)),
      "the superseded event's text must not be read");
  }

  /**
   * @brief An IRRELEVANT decision is not an acknowledgeable finding.
   */
  public function testAnIrrelevantDecisionIsNotRead()
  {
    $this->assertNotContains(self::ITEM_D, $this->itemsInTheGroup(),
      "an IRRELEVANT decision must not appear");
  }

  /**
   * @brief `removed` is nullable, and null means not removed.
   *
   * A clean database carries no nulls here, so `removed = FALSE` passes
   * locally and drops rows in the field.
   */
  public function testANullRemovedFlagCountsAsNotRemoved()
  {
    $this->assertContains(self::ITEM_E, $this->itemsInTheGroup(),
      "removed IS NULL must read as not removed");
  }

  /**
   * @brief Files that disagree on a text are reported as disagreeing.
   */
  public function testDisagreeingFilesAreFlaggedNotFlattened()
  {
    $groups = $this->groups();
    $this->assertTrue($groups[0]['ackMixed'],
      "a group whose files hold different text must say so");
    $this->assertSame('', $groups[0]['ack'],
      "and must not offer one of them as the group's value");
  }

  /**
   * @brief The listing and the file list are the same set.
   *
   * A save writes to what getItemsInGroup returns, so a count the file list
   * cannot reproduce would mean saving over files nobody saw.
   */
  public function testTheListingAndTheFileListAgree()
  {
    $this->assertEquals($this->groups()[0]['files'],
      count($this->itemsInTheGroup()),
      "the count shown and the files written to must be one set");
  }

  /**
   * @brief A file is in one group per license, never two.
   *
   * The acknowledgement is stored once per (file, license). Splitting a file
   * across two length groups puts two editable rows on one row's storage, and
   * the second save discards the first with nothing said. nomos and monk both
   * emit several highlights per finding today, so this is a property of the
   * schema and not of what thesmo currently happens to write.
   */
  public function testAFileIsInOneGroupPerLicense()
  {
    $seen = array();
    foreach ($this->groups() as $group) {
      foreach ($this->itemsOfGroup($group) as $item) {
        $key = $item . ':' . $group['licenseId'];
        $this->assertNotContains($key, $seen,
          "file $item appears in two groups for one license, so both rows "
          . "would write the same clearing_event");
        $seen[] = $key;
      }
    }
    // and the longest span is the one kept
    $lens = array();
    foreach ($this->groups() as $group) {
      $lens[] = $group['len'];
    }
    $this->assertContains(self::NOTICE_LEN + 20, $lens,
      "the longer of a file's two highlights is the one presented");
  }

  /**
   * @brief A file whose newest decision is WIP is not acknowledgeable.
   *
   * Rejecting WIP before the newest decision is picked lets the decision
   * behind it stand in, so a file a reviewer has reopened reads as settled.
   */
  public function testAFileReopenedAsWipIsNotShown()
  {
    $this->assertNotContains(self::ITEM_H, $this->itemsInTheGroup(),
      "a file whose newest decision is WIP must not appear");
  }

  /**
   * @brief A license with no highlight reaches the footer even where another
   *        license on the same file does have one.
   */
  public function testAnUnhighlightedLicenseIsCountedPerLicenseNotPerFile()
  {
    $names = array();
    foreach ($this->browserDao->getUnhighlighted(self::UPLOAD, self::GROUP,
             self::AGENT_THESMO) as $row) {
      $names[$row['shortName']] = intval($row['files']);
    }
    $this->assertArrayHasKey('MIT', $names,
      "a license with no highlight must be reported even where the file's "
      . "other license is highlighted");
    $this->assertEquals(1, $names['MIT']);
  }

  /** @return int[] the items a group covers */
  private function itemsOfGroup($group)
  {
    $items = array();
    foreach ($this->browserDao->getItemsInGroup(self::UPLOAD, self::GROUP,
             self::AGENT_THESMO, $group['licenseId'], $group['len']) as $row) {
      $items[] = intval($row['uploadtree_pk']);
    }
    return $items;
  }

  /**
   * @brief The License Text column is carried, not just acknowledgement.
   *
   * It is `clearing_event.reportinfo`, which the clearing pages label License
   * Text; the page offers all three texts and a write that dropped one would
   * look like a save that silently did nothing.
   */
  public function testTheLicenseTextIsReadAndWritten()
  {
    $items = $this->itemsInTheGroup();
    $this->browserDao->setTextsForItems($items, self::GROUP, self::RF_GPL,
      'an ack', 'a comment', 'the license text');
    $groups = $this->groups();
    $this->assertEquals('the license text', $groups[0]['licenseText']);
    $this->assertFalse($groups[0]['licenseTextMixed']);
    $this->assertEquals('an ack', $groups[0]['ack']);
    $this->assertEquals('a comment', $groups[0]['comment']);
  }

  /**
   * @brief A write reaches every file of the group and no other.
   */
  public function testAWriteReachesTheWholeGroupAndNothingElse()
  {
    $items = $this->itemsInTheGroup();
    $written = $this->browserDao->setTextsForItems($items, self::GROUP,
      self::RF_GPL, 'agreed text', 'a comment', 'the license text');
    $this->assertEquals(count($items), $written);
    foreach ($this->acksOf($items) as $ack) {
      $this->assertEquals('agreed text', $ack);
    }
    // the IRRELEVANT file was never in the group, so it keeps its own text
    $this->assertEquals(array('ack'), $this->acksOf(array(self::ITEM_D)));
  }
}
