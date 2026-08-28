<?php
/*
 SPDX-FileCopyrightText: © 2015 Siemens AG

 SPDX-License-Identifier: GPL-2.0-only
*/

namespace Fossology\Decider;

use Fossology\Lib\BusinessRules\AgentLicenseEventProcessor;
use Fossology\Lib\BusinessRules\ClearingDecisionProcessor;
use Fossology\Lib\Dao\AgentDao;
use Fossology\Lib\Dao\ClearingDao;
use Fossology\Lib\Dao\CompatibilityDao;
use Fossology\Lib\Dao\CopyrightDao;
use Fossology\Lib\Dao\HighlightDao;
use Fossology\Lib\Dao\LicenseDao;
use Fossology\Lib\Dao\ShowJobsDao;
use Fossology\Lib\Dao\UploadDao;
use Fossology\Lib\Data\AgentRef;
use Fossology\Lib\Data\DecisionTypes;
use Fossology\Lib\Data\LicenseMatch;
use Fossology\Lib\Data\LicenseRef;
use Fossology\Lib\Data\Tree\ItemTreeBounds;
use Fossology\Lib\Db\DbManager;
use Fossology\Lib\Test\Reflectory;
use Mockery as M;


global $container;
require_once(__DIR__ . '/../../../lib/php/Test/Agent/AgentTestMockHelper.php');
require_once(__DIR__ . '/../../agent/DeciderAgent.php');

/**
 * @class DeciderAgentTest
 * @breif Unit test for DeciderAgent
 */
class DeciderAgentTest extends \PHPUnit\Framework\TestCase
{
  /** @var DbManager */
  private $dbManager;
  /** @var ClearingDao */
  private $clearingDao;
  /** @var ClearingDecisionProcessor */
  private $clearingDecisionProcessor;
  /** @var AgentLicenseEventProcessor */
  private $agentLicenseEventProcessor;
  /** @var UploadDao */
  private $uploadDao;
  /** @var HighlightDao */
  private $highlightDao;
  /** @var ShowJobsDao */
  private $showJobsDao;
  /** @var CopyrightDao $copyrightDao */
  private $copyrightDao;
  /** @var CompatibilityDao $compatibilityDao */
  private $compatibilityDao;
  /** @var LicenseDao $licenseDao */
  private $licenseDao;

  /**
   * @brief Setup test objects, database and repo
   * @see PHPUnit_Framework_TestCase::setUp()
   */
  protected function setUp() : void
  {
    global $container;
    $container = M::mock('ContainerBuilder');
    $this->dbManager = M::mock(DbManager::class);
    $this->agentDao = M::mock(AgentDao::class);
    $this->agentDao->shouldReceive('getCurrentAgentId')->andReturn(1234);
    $this->highlightDao = M::mock(HighlightDao::class);
    $this->uploadDao = M::mock(UploadDao::class);
    $this->copyrightDao = M::mock(CopyrightDao::class);
    $this->showJobsDao = new ShowJobsDao($this->dbManager, $this->uploadDao);
    $this->copyrightDao = M::mock(CopyrightDao::class);
    $this->clearingDao = M::mock(ClearingDao::class);
    $this->compatibilityDao = M::mock(CompatibilityDao::class);
    $this->licenseDao = M::mock(LicenseDao::class);
    $this->clearingDecisionProcessor = M::mock(ClearingDecisionProcessor::class);
    $this->agentLicenseEventProcessor = M::mock(AgentLicenseEventProcessor::class);

    $container->shouldReceive('get')->withArgs(array('db.manager'))->andReturn($this->dbManager);
    $container->shouldReceive('get')->withArgs(array('dao.agent'))->andReturn($this->agentDao);
    $container->shouldReceive('get')->with('dao.highlight')->andReturn($this->highlightDao);
    $container->shouldReceive('get')->with('dao.show_jobs')->andReturn($this->showJobsDao);
    $container->shouldReceive('get')->with('dao.copyright')->andReturn($this->copyrightDao);
    $container->shouldReceive('get')->withArgs(array('dao.upload'))->andReturn($this->uploadDao);
    $container->shouldReceive('get')->withArgs(array('dao.copyright'))->andReturn($this->copyrightDao);
    $container->shouldReceive('get')->withArgs(array('dao.clearing'))->andReturn($this->clearingDao);
    $container->shouldReceive('get')->withArgs(array('dao.compatibility'))->andReturn($this->compatibilityDao);
    $container->shouldReceive('get')->withArgs(array('dao.license'))->andReturn($this->licenseDao);
    $container->shouldReceive('get')->withArgs(array('decision.types'))->andReturn(M::mock(DecisionTypes::class));
    $container->shouldReceive('get')->withArgs(array('businessrules.clearing_decision_processor'))->andReturn($this->clearingDecisionProcessor);
    $container->shouldReceive('get')->withArgs(array('businessrules.agent_license_event_processor'))->andReturn($this->agentLicenseEventProcessor);
    $this->assertCountBefore = \Hamcrest\MatcherAssert::getCount();
  }

  /**
   * @brief Remove test objects
   * @see PHPUnit_Framework_TestCase::tearDown()
   */
  protected function tearDown() : void
  {
    $this->addToAssertionCount(\Hamcrest\MatcherAssert::getCount()-$this->assertCountBefore);
    M::close();
  }

  /**
   * @test
   * -# Create empty license matches
   * -# Test if DeciderAgent::areNomosMatchesInsideAMonkMatch()
   * returns false
   */
  public function testAreNomosMatchesInsideAMonkMatchIfNoneAtAll()
  {
    $deciderAgent = new DeciderAgent();

    $reflection = new \ReflectionClass($deciderAgent);
    $method = $reflection->getMethod('areNomosMatchesInsideAMonkMatch');
    $method->setAccessible(true);

    $licenseMatches = array();
    assertThat( $method->invoke($deciderAgent,$licenseMatches), equalTo(false) );
  }

  /**
   * @test
   * -# Create nomos license match only
   * -# Test if DeciderAgent::areNomosMatchesInsideAMonkMatch()
   * returns false
   */
  public function testAreNomosMatchesInsideAMonkMatchIfNoMonk()
  {
    $deciderAgent = new DeciderAgent();

    $reflection = new \ReflectionClass($deciderAgent);
    $method = $reflection->getMethod('areNomosMatchesInsideAMonkMatch');
    $method->setAccessible(true);

    $this->highlightDao->shouldReceive('getHighlightRegion')->andReturn(array($start=2, $end=5));
    $licenseMatches = array('nomos'=>
        array($this->createLicenseMatch('nomos',1))
        );
    assertThat( $method->invoke($deciderAgent,$licenseMatches), equalTo(false) );
  }

  /**
   * @test
   * -# Create monk license matche only
   * -# Test if DeciderAgent::areNomosMatchesInsideAMonkMatch()
   * returns false
   */
  public function testAreNomosMatchesInsideAMonkMatchIfNoNomos()
  {
    $deciderAgent = new DeciderAgent();

    $reflection = new \ReflectionClass($deciderAgent);
    $method = $reflection->getMethod('areNomosMatchesInsideAMonkMatch');
    $method->setAccessible(true);

    $this->highlightDao->shouldReceive('getHighlightRegion')->andReturn(array($start=2, $end=5));
    $licenseMatches = array('monk'=>
        array($this->createLicenseMatch('monk',1))
        );
    assertThat( $method->invoke($deciderAgent,$licenseMatches), equalTo(false) );
  }

  /**
   * @test
   * -# Create monk license match
   * -# Create nomos license match bigger than monk
   * license match
   * -# Test if DeciderAgent::areNomosMatchesInsideAMonkMatch()
   * returns false
   */
  public function testAreNomosMatchesInsideAMonkMatchIfNotFit()
  {
    $deciderAgent = new DeciderAgent();

    $reflection = new \ReflectionClass($deciderAgent);
    $method = $reflection->getMethod('areNomosMatchesInsideAMonkMatch');
    $method->setAccessible(true);
    $monkId = 1;
    $nomosId = 2;
    $this->highlightDao->shouldReceive('getHighlightRegion')->with($monkId)->andReturn(array($start=2, $end=5));
    $this->highlightDao->shouldReceive('getHighlightRegion')->with($nomosId)->andReturn(array($start=2, $end=8));
    $licenseMatches = array('monk'=>array($this->createLicenseMatch('monk',$monkId)),
            'nomos'=>array($this->createLicenseMatch('nomos',$nomosId)));
    assertThat( $method->invoke($deciderAgent,$licenseMatches), equalTo(false) );
  }

  /**
   * @test
   * -# Create monk license match
   * -# Create nomos license match inside monk match
   * -# Test if DeciderAgent::areNomosMatchesInsideAMonkMatch()
   * returns true
   */
  public function testAreNomosMatchesInsideAMonkMatchIfFit()
  {
    $deciderAgent = new DeciderAgent();

    $reflection = new \ReflectionClass($deciderAgent);
    $method = $reflection->getMethod('areNomosMatchesInsideAMonkMatch');
    $method->setAccessible(true);
    $monkId = 1;
    $nomosId = 2;
    $this->highlightDao->shouldReceive('getHighlightRegion')->with($monkId)->andReturn(array($start=2, $end=5));
    $this->highlightDao->shouldReceive('getHighlightRegion')->with($nomosId)->andReturn(array($start=4, $end=5));
    $licenseMatches = array('monk'=>array($this->createLicenseMatch('monk',$monkId)),
            'nomos'=>array($this->createLicenseMatch('nomos',$nomosId)));
    assertThat( $method->invoke($deciderAgent,$licenseMatches), equalTo(true) );
  }


  /**
   * @brief Create mock LicenseMatch object with getLicenseFileId returning
   * $matchId
   * @param string $agentName
   * @param int    $matchId
   * @return Mockery::MockInterface
   */
  protected function createLicenseMatch($agentName, $matchId)
  {
    $licenseMatch = M::mock(LicenseMatch::class);
    $licenseMatch->shouldReceive("getLicenseFileId")->withNoArgs()->andReturn($matchId);
    return $licenseMatch;
  }

  /**
   * @test
   * -# Create monk and nomos license match only
   * -# Test if DeciderAgent::areNomosMonkNinkaAgreed()
   * returns false
   */
  public function testAreNomosMonkNinkaAgreed_notIfOnlyTwoOfThem()
  {
    $deciderAgent = new DeciderAgent();
    $licId = 401;
    $licenseMatches = array('monk'=>array($this->createLicenseMatchWithLicId($licId)),
            'nomos'=>array($this->createLicenseMatchWithLicId($licId)));
    $agree = Reflectory::invokeObjectsMethodnameWith($deciderAgent, 'areNomosMonkNinkaAgreed', array($licenseMatches));
    assertThat($agree, equalTo(false) );
  }

  /**
   * @test
   * -# Create monk, nomos and ninka license match
   * -# Add multiple match for an agent with the same license id
   * -# Test if DeciderAgent::areNomosMonkNinkaAgreed()
   * returns true
   */
  public function testAreNomosMonkNinkaAgreed_alsoMultiMatch()
  {
    $deciderAgent = new DeciderAgent();
    $licId = 401;
    $licenseMatches = array('monk'=>array($this->createLicenseMatchWithLicId($licId)),
            'nomos'=>array($this->createLicenseMatchWithLicId($licId),$this->createLicenseMatchWithLicId($licId)),
            'ninka'=>array($this->createLicenseMatchWithLicId($licId)));
    $agree = Reflectory::invokeObjectsMethodnameWith($deciderAgent, 'areNomosMonkNinkaAgreed', array($licenseMatches));
    assertThat($agree, equalTo(true) );
  }


  /**
   * @test
   * -# Create monk, nomos and ninka license match
   * -# Add multiple match for an agent with the different license id
   * -# Test if DeciderAgent::areNomosMonkNinkaAgreed()
   * returns false
   */
  public function testAreNomosMonkNinkaAgreed_notIfAnyOther()
  {
    $deciderAgent = new DeciderAgent();
    $licId = 401;
    $otherLicId = 402;
    $licenseMatches = array('monk'=>array($this->createLicenseMatchWithLicId($licId)),
            'nomos'=>array($this->createLicenseMatchWithLicId($licId),$this->createLicenseMatchWithLicId($otherLicId)),
            'ninka'=>array($this->createLicenseMatchWithLicId($licId)));
    $agree = Reflectory::invokeObjectsMethodnameWith($deciderAgent, 'areNomosMonkNinkaAgreed', array($licenseMatches));
    assertThat($agree, equalTo(false) );
  }

  /**
   * @brief Create mock LicenseMatch object with getLicenseId returning
   * $licId
   * @param int $licId
   * @return LicenseMatch
   */
  /**
   * @test
   * -# thesmo reports only a pointer -- "See-file", a bare family, a choice --
   *    and nothing else contradicts it
   * -# Test that DeciderAgent::thesmoAgreement() returns null, so a pointer
   *    is never concluded as a licence
   */
  public function testThesmoPointersAreNotIdentifications()
  {
    $deciderAgent = new DeciderAgent();
    foreach (DeciderAgent::THESMO_NOT_IDENTIFICATIONS as $shortName) {
      $licenseMatches = array(
          'thesmo' => array($this->createLicenseMatchWithShortName(701, $shortName)),
          'nomos' => array($this->createLicenseMatchWithLicId(701)));
      $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
          'thesmoAgreement', array($licenseMatches));
      assertThat($shortName, $verdict, equalTo(null));
    }
  }

  /**
   * @test
   * -# thesmo identifies a licence and nomos agrees
   * -# Test that DeciderAgent::thesmoAgreement() agrees with nothing to remove
   */
  public function testThesmoIdentificationIsConcluded()
  {
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithLicId(401)),
        'nomos' => array($this->createLicenseMatchWithLicId(401)));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(array()));
  }

  /**
   * @test
   * -# thesmo identifies one licence and points at another file besides
   * -# nomos identifies a licence thesmo did not
   * -# Test that the pointer does not stand in for the missing identification
   */
  public function testThesmoPointerDoesNotCoverANomosFinding()
  {
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithShortName(701, 'See-file')),
        'nomos' => array($this->createLicenseMatchWithLicId(401)));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(null));
  }

  /**
   * @test
   * -# thesmo identifies a licence and a pointer; nomos identifies the licence
   * -# Test that they agree and the pointer is what gets removed, so it is not
   *    concluded beside the licence
   */
  public function testThesmoPointerIsRemovedFromTheDecision()
  {
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithLicId(401),
                          $this->createLicenseMatchWithShortName(701, 'See-file.LICENSE')),
        'nomos' => array($this->createLicenseMatchWithLicId(401)));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(array(701)));
  }

  /**
   * @test
   * -# thesmo identifies a licence nomos did not find
   * -# Test that an uncorroborated identification is not concluded
   */
  public function testThesmoFindingNeedsCorroboration()
  {
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithLicId(401),
                          $this->createLicenseMatchWithLicId(402)),
        'nomos' => array($this->createLicenseMatchWithLicId(401)));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(null));
  }

  /**
   * @test
   * -# thesmo reports "L WITH E"; nomos reports L and E as two findings
   * -# Test that the two are read as the same statement, and nomos's two rows
   *    are what gets removed so the WITH form alone is concluded
   */
  public function testLicenceWithExceptionAgreesWithThePair()
  {
    $this->mockLicenceLookup(array('GPL-3.0-or-later' => 401, 'GCC-exception-3.1' => 402));
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithShortName(900,
            'GPL-3.0-or-later WITH GCC-exception-3.1')),
        'nomos' => array($this->createLicenseMatchWithLicId(401),
                         $this->createLicenseMatchWithLicId(402)));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(array(401, 402)));
  }

  /**
   * @test
   * -# thesmo reports "L WITH E"; nomos reports only L
   * -# Test that an exception no other scanner saw is not concluded
   */
  public function testExceptionNeedsCorroboration()
  {
    $this->mockLicenceLookup(array('GPL-3.0-or-later' => 401, 'GCC-exception-3.1' => 402));
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithShortName(900,
            'GPL-3.0-or-later WITH GCC-exception-3.1')),
        'nomos' => array($this->createLicenseMatchWithLicId(401)));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(null));
  }

  /**
   * @test
   * -# two licences and one exception; only thesmo says which licence the
   *    exception binds to, and no other scanner reports the WITH form
   * -# Test that the binding is not concluded automatically
   */
  public function testExceptionBindingNeedsASingleLicenceOrTheSameForm()
  {
    $this->mockLicenceLookup(array('GPL-2.0-only' => 401, 'Classpath-exception-2.0' => 402));
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithLicId(403),
                          $this->createLicenseMatchWithShortName(900,
            'GPL-2.0-only WITH Classpath-exception-2.0')),
        'nomos' => array($this->createLicenseMatchWithLicId(403),
                         $this->createLicenseMatchWithLicId(401),
                         $this->createLicenseMatchWithLicId(402)));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(null));

    // another scanner reporting the same WITH form settles the binding
    $licenseMatches['monk'] = array($this->createLicenseMatchWithShortName(900,
        'GPL-2.0-only WITH Classpath-exception-2.0'));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(array(401, 402)));
  }

  /**
   * @test
   * -# nomos reports MIT and MIT-style, thesmo MIT and the MIT-style pointer
   * -# Test that the family name takes no part in the agreement and both
   *    scanners' family rows are removed, so MIT is concluded
   */
  public function testAFamilyNameFromAnyScannerIsRemovedNotConcluded()
  {
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithShortName(401, 'MIT'),
                          $this->createLicenseMatchWithShortName(410, 'MIT-style')),
        'nomos' => array($this->createLicenseMatchWithShortName(401, 'MIT'),
                         $this->createLicenseMatchWithShortName(410, 'MIT-style')));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(array(410)));
  }

  /**
   * @test
   * -# both scanners report only "see LICENSE"
   * -# Test that nothing is concluded: a pointer is not a licence
   */
  public function testAPointerAloneConcludesNothing()
  {
    $deciderAgent = new DeciderAgent();
    $licenseMatches = array(
        'thesmo' => array($this->createLicenseMatchWithShortName(420, 'See-file.LICENSE')),
        'nomos' => array($this->createLicenseMatchWithShortName(420, 'See-file.LICENSE')));
    $verdict = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'thesmoAgreement', array($licenseMatches));
    assertThat($verdict, equalTo(null));
  }

  /**
   * @test
   * -# the decider hands the rule its matches grouped by licence id, and a
   *    WITH form and the pair it stands for are three different ids
   * -# Test that the rule reads the file as a whole and decides, recording
   *    nomos's two rows as removed
   */
  public function testThesmoRuleReadsTheFileNotTheLicenceGroups()
  {
    $this->mockLicenceLookup(array('GPL-3.0-or-later' => 401, 'GCC-exception-3.1' => 402));
    $itemTreeBounds = M::mock(ItemTreeBounds::class);
    $itemTreeBounds->shouldReceive('getItemId')->andReturn(716188);
    $this->clearingDao->shouldReceive('insertClearingEvent')
        ->with(716188, M::any(), M::any(), 401, true, M::any())->once();
    $this->clearingDao->shouldReceive('insertClearingEvent')
        ->with(716188, M::any(), M::any(), 402, true, M::any())->once();
    $this->clearingDecisionProcessor->shouldReceive('makeDecisionFromLastEvents')
        ->with($itemTreeBounds, M::any(), M::any(), DecisionTypes::IDENTIFIED, false)->once();
    $deciderAgent = new DeciderAgent();
    // grouped by licence id, as remapByProjectedId() hands them over
    $matches = array(
        401 => array('nomos' => array($this->createLicenseMatchWithLicId(401))),
        402 => array('nomos' => array($this->createLicenseMatchWithLicId(402))),
        900 => array('thesmo' => array($this->createLicenseMatchWithShortName(900,
            'GPL-3.0-or-later WITH GCC-exception-3.1'))));
    $decided = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'autodecideIfThesmoMatchesNoContradiction', array($itemTreeBounds, $matches));
    assertThat($decided, equalTo(true));
  }

  private function mockLicenceLookup(array $idsByShortName)
  {
    foreach ($idsByShortName as $shortName => $id) {
      $this->licenseDao->shouldReceive('getLicenseByShortName')
          ->with($shortName, M::any())
          ->andReturn(new LicenseRef($id, $shortName, $shortName, $shortName));
    }
  }

  protected function createLicenseMatchWithShortName($licId, $shortName)
  {
    return new LicenseMatch(1,
        new LicenseRef($licId, $shortName, $shortName, $shortName),
        M::mock(AgentRef::class),
        1);
  }

  protected function createLicenseMatchWithLicId($licId)
  {
    if ($licId == 401) {
      $licenseShortName = "LicA";
      $licenseName = "LicenseA";
    } else {
      $licenseShortName = "LicB";
      $licenseName = "LicenseB";
    }
    return new LicenseMatch(1,
        new LicenseRef($licId, $licenseShortName, $licenseName, $licenseShortName),
        M::mock(AgentRef::class),
        1);
  }

  /**
   * @test
   * -# Create compatibility matches.
   * -# Test if DeciderAgent::noLicenseConflict() returns true
   */
  public function testnoLicenseConflict_twoOfThem()
  {
    $deciderAgent = new DeciderAgent();
    $licId = 401;
    $otherLicId = 402;
    $licenseMatches = [
      $licId => [
        'monk' => [
          $this->createLicenseMatchWithLicId($licId)
        ],
        'nomos' => [
          $this->createLicenseMatchWithLicId($licId),
          $this->createLicenseMatchWithLicId($otherLicId)
        ],
        'ojo' => [
          $this->createLicenseMatchWithLicId($licId)
        ]
      ],
      $otherLicId => [
        'nomos' => [
          $this->createLicenseMatchWithLicId($licId),
          $this->createLicenseMatchWithLicId($otherLicId)
        ]
      ]
    ];
    $itemTreeBounds = new ItemTreeBounds(123, "uploadtree", "2", 1, 4);

    $this->compatibilityDao->shouldReceive("getCompatibilityForFile")
        ->withArgs([
            $itemTreeBounds,
            $this->createLicenseMatchWithLicId($licId)->getLicenseRef()
                ->getShortName()
        ])
        ->andReturn(true);
    $this->compatibilityDao->shouldReceive("getCompatibilityForFile")
        ->withArgs([
            $itemTreeBounds,
            $this->createLicenseMatchWithLicId($otherLicId)->getLicenseRef()
                ->getShortName()
        ])
        ->andReturn(true);

    $agree = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'noLicenseConflict', [$itemTreeBounds, $licenseMatches]);
    $this->assertTrue($agree, "Wrong result for compatible licenses");
  }

  /**
   * @test
   * -# Create compatibility miss match for 2 licenses.
   * -# Test if DeciderAgent::noLicenseConflict() returns false
   */
  public function testnoLicenseConflict_twoOfThemNotComp()
  {
    $deciderAgent = new DeciderAgent();
    $licId = 401;
    $otherLicId = 402;
    $licenseMatches = [
        $licId => [
            'monk' => [
                $this->createLicenseMatchWithLicId($licId)
            ],
            'nomos' => [
                $this->createLicenseMatchWithLicId($licId),
                $this->createLicenseMatchWithLicId($otherLicId)
            ],
            'ojo' => [
                $this->createLicenseMatchWithLicId($licId)
            ]
        ],
        $otherLicId => [
            'nomos' => [
                $this->createLicenseMatchWithLicId($licId),
                $this->createLicenseMatchWithLicId($otherLicId)
            ]
        ]
    ];
    $itemTreeBounds = new ItemTreeBounds(123, "uploadtree", "2", 1, 4);

    $this->compatibilityDao->shouldReceive("getCompatibilityForFile")
        ->withArgs([
            $itemTreeBounds,
            $this->createLicenseMatchWithLicId($licId)->getLicenseRef()
                ->getShortName()
        ])
        ->andReturn(true);
    $this->compatibilityDao->shouldReceive("getCompatibilityForFile")
        ->withArgs([
            $itemTreeBounds,
            $this->createLicenseMatchWithLicId($otherLicId)->getLicenseRef()
                ->getShortName()
        ])
        ->andReturn(false);

    $agree = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'noLicenseConflict', [$itemTreeBounds, $licenseMatches]);
    $this->assertFalse($agree, "Wrong result for incompatible licenses");
  }

  /**
   * @test
   * -# Create matches with compliant license type.
   * -# Test if DeciderAgent::allLicenseInType() returns true
   */
  public function testallLicenseInType_twoOfThem()
  {
    $deciderAgent = new DeciderAgent();
    $licId = 401;
    $otherLicId = 402;
    $licenseMatches = [
        $licId => [
            'monk' => [
                $this->createLicenseMatchWithLicId($licId)
            ],
            'nomos' => [
                $this->createLicenseMatchWithLicId($licId),
                $this->createLicenseMatchWithLicId($otherLicId)
            ],
            'ojo' => [
                $this->createLicenseMatchWithLicId($licId)
            ]
        ],
        $otherLicId => [
            'nomos' => [
                $this->createLicenseMatchWithLicId($licId),
                $this->createLicenseMatchWithLicId($otherLicId)
            ]
        ]
    ];

    $this->licenseDao->shouldReceive("getLicenseType")
        ->withArgs([$licId])->andReturn("Permissive");
    $this->licenseDao->shouldReceive("getLicenseType")
        ->withArgs([$otherLicId])->andReturn("Permissive");

    $reflector = new \ReflectionProperty(DeciderAgent::class, "licenseType");
    $reflector->setAccessible(true);
    $reflector->setValue($deciderAgent, "Permissive");

    $agree = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'allLicenseInType', [$licenseMatches]);
    $this->assertTrue($agree, "Wrong result for compatible license types");
  }

  /**
   * @test
   * -# Create matches with compliant and non-compliant license types.
   * -# Test if DeciderAgent::allLicenseInType() returns false
   */
  public function testallLicenseInType_twoOfThemNonComp()
  {
    $deciderAgent = new DeciderAgent();
    $licId = 401;
    $otherLicId = 402;
    $licenseMatches = [
        $licId => [
            'monk' => [
                $this->createLicenseMatchWithLicId($licId)
            ],
            'nomos' => [
                $this->createLicenseMatchWithLicId($licId),
                $this->createLicenseMatchWithLicId($otherLicId)
            ],
            'ojo' => [
                $this->createLicenseMatchWithLicId($licId)
            ]
        ],
        $otherLicId => [
            'nomos' => [
                $this->createLicenseMatchWithLicId($licId),
                $this->createLicenseMatchWithLicId($otherLicId)
            ]
        ]
    ];

    $this->licenseDao->shouldReceive("getLicenseType")
        ->withArgs([$licId])->andReturn("Permissive");
    $this->licenseDao->shouldReceive("getLicenseType")
        ->withArgs([$otherLicId])->andReturn("Copyleft");

    $reflector = new \ReflectionProperty(DeciderAgent::class, "licenseType");
    $reflector->setAccessible(true);
    $reflector->setValue($deciderAgent, "Permissive");

    $agree = Reflectory::invokeObjectsMethodnameWith($deciderAgent,
        'allLicenseInType', [$licenseMatches]);
    $this->assertFalse($agree, "Wrong result for non-compatible license types");
  }
}
