<?php
/*
 SPDX-FileCopyrightText: © 2020 Siemens AG
 Author: Gaurav Mishra <mishra.gaurav@siemens.com>

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * @file
 * @brief Tests for Decider
 */

namespace Fossology\UI\Api\Test\Models;

use Fossology\UI\Api\Models\Decider;
use Fossology\UI\Api\Models\ApiVersion;

/**
 * @class DeciderTest
 * @brief Tests for Decider model
 */
class DeciderTest extends \PHPUnit\Framework\TestCase
{
  ////// Constructor Tests //////

  /**
   * Tests that the Decider constructor initializes an instance correctly.
   *
   * @return void
   */
  public function testConstructor()
  {
    $decider = new Decider(
      true,       
      false,   
      true,      
      false,       
      'GPL-3.0'    
  );
    $this->assertInstanceOf(Decider::class, $decider);
  }

  /**
   * @test
   * -# Test for Decider::setUsingArray() when $version is V1
   * -# Create a test array and pass to setUsingArray() on test object
   * -# Check if the object is updated
   */
  public function testSetUsingArrayV1()
  {
    $this->testSetUsingArray(ApiVersion::V1);
  }

  /**
   * @test
   * -# Test for Decider::setUsingArray() when $version is V2
   * -# Create a test array and pass to setUsingArray() on test object
   * -# Check if the object is updated
   */
  public function testSetUsingArrayV2()
  {
    $this->testSetUsingArray(ApiVersion::V2);
  }

  /**
   * @param $version version to test
   * @return void
   * -# Test for Decider::setUsingArray() to check if the object is updated
   */
  private function testSetUsingArray($version)
  {
    if ($version == ApiVersion::V1) {
      $deciderArray = [
        "nomos_monk" => true,
        "bulk_reused" => false,
        "ojo_decider" => (1==1),
        "conclude_license_type" => "   Permissive ",
        "copyright_deactivation" => false,
        "copyright_clutter_removal" => true
      ];
    } else {
      $deciderArray = [
        "nomosMonk" => true,
        "bulkReused" => false,
        "ojoDecider" => (1==1),
        "concludeLicenseType" => "   Permissive ",
        "copyrightDeactivation" => false,
        "copyrightClutterRemoval" => true
      ];
    }

    $expectedObject = new Decider(true, false, false, true, "Permissive", false, true);

    $actualObject = new Decider();
    $actualObject->setUsingArray($deciderArray, $version);

    $this->assertEquals($expectedObject, $actualObject);
  }

  /**
   * @test
   * -# Test the data format returned by Decider::getArray($version) model when $version is V1
   * -# Create expected array and update test object to match it
   * -# Get the array from object and match with expected array
   */
  public function testDataFormatV1()
  {
    $this->testDataFormat(ApiVersion::V1);
  }

  /**
   * @test
   * -# Test the data format returned by Decider::getArray($version) model when $version is V2
   * -# Create expected array and update test object to match it
   * -# Get the array from object and match with expected array
   */
  public function testDataFormatV2()
  {
    $this->testDataFormat(ApiVersion::V2);
  }

  /**
   * @param $version version to test
   * @return void
   * -# Test the data format returned by Decider::getArray($version) model
   */
  private function testDataFormat($version)
  {
    if ($version == ApiVersion::V1) {
      $expectedArray = [
        "nomos_monk"  => true,
        "bulk_reused" => false,
        "new_scanner" => false,
        "ojo_decider" => true,
        "thesmo_decider" => false,
        "conclude_license_type" => "Permissive",
        "copyright_deactivation" => false,
        "copyright_clutter_removal" => false
      ];
    } else {
      $expectedArray = [
        "nomosMonk"  => true,
        "bulkReused" => false,
        "newScanner" => false,
        "ojoDecider" => true,
        "thesmoDecider" => false,
        "concludeLicenseType" => "Permissive",
        "copyrightDeactivation" => false,
        "copyrightClutterRemoval" => false
      ];
    }

    $actualObject = new Decider();
    $actualObject->setNomosMonk(true);
    $actualObject->setOjoDecider(true);
    $actualObject->setConcludeLicenseType(" Permissive ");

    $this->assertEquals($expectedArray, $actualObject->getArray($version));
  }

  /**
   * The files the Thesmo option crosses on its way to the agent. It is named
   * here, turned into a string by ScanOptions, and matched as that string by
   * DeciderAgentPlugin -- and only the first two are code this model can be
   * held to.
   */
  private function sourceOf($what)
  {
    $root = dirname(__DIR__, 4);
    $paths = [
      "emitted" => $root . '/www/ui/api/Models/ScanOptions.php',
      "consumed" => $root . '/decider/ui/DeciderAgentPlugin.php',
    ];
    $this->assertFileExists($paths[$what]);
    return file_get_contents($paths[$what]);
  }

  /**
   * @test
   * -# The Thesmo option round-trips under each version's own spelling
   * -# And a version does not answer to the other's, which would set an
   *    option on one interface and silently not on the other
   */
  public function testThesmoOptionRoundTrips()
  {
    foreach ([[ApiVersion::V1, "thesmo_decider", "thesmoDecider"],
              [ApiVersion::V2, "thesmoDecider", "thesmo_decider"]] as $case) {
      list($version, $mine, $theirs) = $case;

      $decider = new Decider();
      $this->assertFalse($decider->getThesmoDecider());

      $decider->setUsingArray([$mine => true], $version);
      $this->assertTrue($decider->getThesmoDecider());
      $this->assertTrue($decider->getArray($version)[$mine]);

      $other = new Decider();
      $other->setUsingArray([$theirs => true], $version);
      $this->assertFalse($other->getThesmoDecider());
    }
  }

  /**
   * @test
   * -# A request carries "true" and not true, so the option is read as a
   *    boolean however it was spelled
   */
  public function testThesmoOptionIsReadFromAString()
  {
    $decider = new Decider();
    $decider->setThesmoDecider("true");
    $this->assertTrue($decider->getThesmoDecider());
    $decider->setThesmoDecider("false");
    $this->assertFalse($decider->getThesmoDecider());
  }

  /**
   * @test
   * -# The option string ScanOptions emits is one the agent plugin answers to
   *
   * This is the join no type system checks: the plugin matches a literal in a
   * switch, so a name differing by a letter leaves the option settable,
   * readable, and with no effect at all.
   */
  public function testThesmoOptionStringReachesTheAgent()
  {
    $emitted = $this->sourceOf("emitted");
    $this->assertStringContainsString("getThesmoDecider()", $emitted);
    $this->assertStringContainsString("'thesmoNoContradiction'", $emitted);
    $this->assertStringContainsString("case 'thesmoNoContradiction':",
      $this->sourceOf("consumed"));
  }

  /**
   * @test
   * -# Turning the rule on schedules what it reads: it compares Thesmo
   *    against nomos and monk, and a scanner that did not run contradicts
   *    nothing, which would make the check weaker than its name
   */
  public function testThesmoRuleSchedulesWhatItReads()
  {
    $consumed = $this->sourceOf("consumed");
    $at = strpos($consumed, "case 'thesmoNoContradiction':");
    $this->assertNotFalse($at);
    $arm = substr($consumed, $at, 600);
    $end = strpos($arm, 'break;');
    $arm = $end === false ? $arm : substr($arm, 0, $end);
    foreach (['agent_nomos', 'agent_monk', 'agent_thesmo'] as $agent) {
      $this->assertStringContainsString($agent, $arm,
        "the rule does not depend on " . $agent);
    }
  }
}
