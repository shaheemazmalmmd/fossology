<?php
/*
 SPDX-FileCopyrightText: © 2016 Siemens AG

 SPDX-License-Identifier: GPL-2.0-only
*/

namespace Fossology\Spdx;

use Fossology\Lib\Data\LicenseRef;
use Fossology\Lib\Util\StringOperation;

/**
 * @class SpdxUtils
 * @brief Utilities for SPDX2
 */
class SpdxUtils
{
  /**
   * @brief For a given set of arguments assign $args[$key1] and $args[$key2]
   *
   * Get the array of arguments and find $key1 and $key2 values and assign
   * to $args[$key1] and $args[$key2].
   * @param string[] $args String array
   * @param string $key1 Key1
   * @param string $key2 Key2
   * @return string[] $args
   */
  static public function preWorkOnArgsFlp($args,$key1,$key2)
  {
    $needle = ' --'.$key2.'=';
    if (is_array($args) &&
        array_key_exists($key1, $args) &&
        strpos($args[$key1],$needle) !== false) {
      $exploded = explode($needle,$args[$key1]);
      $args[$key1] = trim($exploded[0]);
      $args[$key2] = trim($exploded[1]);
    }
    return $args;
  }

  /**
   * @brief Add prefix to the license based on SPDX2 standards
   * @param string $license
   * @return string
   */
  static public function addPrefixOnDemand($license)
  {
    if (empty($license) || $license === "NOASSERTION") {
      return "NOASSERTION";
    }

    if (strpos($license, " OR ") !== false) {
      return "(" . $license . ")";
    }

    $license = preg_replace('/[^a-zA-Z0-9\-\_\.\+]/','-',$license);
    if (strpos($license, LicenseRef::SPDXREF_PREFIX) !== false) {
      // License ref can not end with a '+'
      $license = preg_replace('/\+$/', '-or-later', $license);
    }
    return preg_replace('/\+(?!$)/','-',$license);
  }

  /**
   * @brief Add prefix to license keys
   * @param array $licenses
   * @return string[]
   */
  static public function addPrefixOnDemandKeys($licenses)
  {
    $ret = array();
    foreach ($licenses as $license=>$text) {
      $ret[self::addPrefixOnDemand($license)] = $text;
    }
    return $ret;
  }

  /**
   * @brief Add prefix to license list
   * @param array $licenses
   * @return array
   */
  static public function addPrefixOnDemandList($licenses)
  {
    return array_map(function ($license)
    {
      return SpdxUtils::addPrefixOnDemand($license);
    },$licenses);
  }

  /**
   * @brief Is this name the dual-license marker rather than a license?
   *
   * The marker records that a file offers a choice; it is not a license and
   * has no terms. It is matched in its bare form and under either reference
   * prefix, since which one a row carries depends on how it was stored.
   * @param string $license
   * @return bool
   */
  static public function isDualLicenseMarker($license)
  {
    return StringOperation::stringStartsWith($license, "Dual-license") ||
      StringOperation::stringStartsWith($license,
        LicenseRef::SPDXREF_PREFIX . "Dual-license") ||
      StringOperation::stringStartsWith($license,
        LicenseRef::SPDXREF_PREFIX_FOSSOLOGY . "Dual-license");
  }

  /**
   * @brief Implode licenses into one SPDX expression.
   *
   * The dual-license marker says the file offers a choice, so the licenses
   * beside it are joined with `OR`; without it they are obligations and are
   * joined with `AND`.
   *
   * The marker is removed either way, because it is not a license. Left among
   * the members it was exported as a conjunct -- `A AND B AND Dual-license` --
   * which is not a valid expression and tells a reader to obey the terms of a
   * license that does not exist. That happened for every count except exactly
   * three, which was the only case handled.
   *
   * **A flat `OR` is the most these rows can say.** A marker beside three
   * licenses is usually `A OR B OR C` but can be `(A OR B) AND C`, and a set
   * of per-license rows cannot tell the two apart: the relation was never
   * stored, only the members and the fact that some choice existed. Where that
   * distinction matters the expression has to be carried as text from the
   * scanner through to the decision, which is a schema change and not this
   * function's business.
   *
   * @param string[] $licenses
   * @return string
   */
  static public function implodeLicenses($licenses)
  {
    if (!$licenses || !is_array($licenses) || sizeof($licenses) == 0) {
      return "";
    }

    $licenses = self::addPrefixOnDemandList($licenses);
    sort($licenses, SORT_NATURAL | SORT_FLAG_CASE);

    $members = array_values(array_filter($licenses, function ($lic) {
      return !SpdxUtils::isDualLicenseMarker($lic);
    }));
    $isChoice = count($members) < count($licenses);

    if (count($members) == 0) {
      // the marker on its own names no license, and saying "Dual-license"
      // here would export the marker as the answer
      return "";
    }
    if (count($members) == 1) {
      return $members[0];
    }
    return implode($isChoice ? " OR " : " AND ", $members);
  }

  /**
   * Clean an array of strings by trimming the elements and removing empty
   * strings.
   * @param string[] $texts Array of texts to be concatenated.
   * @return string[] String array with all trimmed string elements.
   */
  static public function cleanTextArray($texts): array
  {
    if (!$texts || !is_array($texts) || sizeof($texts) == 0) {
      return [];
    }

    sort($texts, SORT_NATURAL | SORT_FLAG_CASE);

    $cleanArray = [];
    foreach ($texts as $text) {
      $text = trim($text);
      if (empty($text)) {
        continue;
      }
      $cleanArray[] = $text;
    }
    return $cleanArray;
  }

  /**
   * Remove empty and 'NOASSERTION' licenses from list.
   * @param string[] $licenses List of licenses.
   * @return array List of licenses removing empty and 'NOASSERTION's.
   */
  public static function removeEmptyLicenses($licenses): array
  {
    $newList = [];
    foreach ($licenses as $license) {
      if (empty($license) || $license === "NOASSERTION") {
        continue;
      }
      $newList[] = $license;
    }
    return $newList;
  }
}
