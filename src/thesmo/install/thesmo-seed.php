<?php
/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * @file
 * @brief Seed license_ref with the shortnames Thesmo can emit.
 *
 * Thesmo declares its whole vocabulary in model.json, so the rows it needs are
 * known before it ever runs. Seeding them here means the agent never has to
 * insert one at scan time the way nomos does through add2license_ref(), which
 * is how a long-lived database accumulates rows carrying no licence text.
 *
 * Names already present are left untouched, including their text and type.
 *
 * Usage: php thesmo-seed.php <model.json> [--dry-run]
 */

$modelPath = $argv[1] ?? null;
$dryRun = in_array('--dry-run', $argv, true);
if (!$modelPath || !is_readable($modelPath)) {
  fwrite(STDERR, "usage: thesmo-seed.php <model.json> [--dry-run]\n");
  exit(1);
}
$model = json_decode(file_get_contents($modelPath), true);
if (!isset($model['classes'])) {
  fwrite(STDERR, "thesmo-seed: no 'classes' in $modelPath\n");
  exit(1);
}

// <repo>/src/thesmo/install and <moddir>/thesmo/install both sit two levels
// under the directory that holds lib/php.
require_once dirname(__DIR__, 2) . '/lib/php/bootstrap.php';
$dbManager = $GLOBALS['container']->get('db.manager');

$missing = [];
foreach ($model['classes'] as $name) {
  $row = $dbManager->getSingleRow(
    'SELECT rf_pk FROM license_ref WHERE rf_shortname = $1',
    [$name], 'thesmo.seed.lookup');
  if (empty($row)) {
    $missing[] = $name;
  }
}

printf("thesmo-seed: %d declared, %d already present, %d missing\n",
  count($model['classes']), count($model['classes']) - count($missing), count($missing));

if (!$missing) {
  exit(0);
}
if ($dryRun) {
  foreach ($missing as $name) {
    echo "  would insert: $name\n";
  }
  exit(0);
}
$inserted = 0;
foreach ($missing as $name) {
  // rf_shortname is indexed but not unique, so ON CONFLICT has no constraint to
  // name and every insert would fail with SQLSTATE 42P10. Select-or-insert in
  // one statement, the same way the agent does it.
  $row = $dbManager->getSingleRow(
    "WITH existing AS (
       SELECT rf_pk FROM ONLY license_ref WHERE rf_shortname = $1
     ), created AS (
       INSERT INTO license_ref (rf_shortname, rf_text, rf_detector_type)
         SELECT $1, $2, 2 WHERE NOT EXISTS (SELECT 1 FROM existing)
         RETURNING rf_pk
     )
     SELECT rf_pk, TRUE AS created FROM created
     UNION ALL SELECT rf_pk, FALSE FROM existing",
    [$name, 'License text not yet imported (seeded for Thesmo).'],
    'thesmo.seed.insert');
  if (empty($row)) {
    fwrite(STDERR, "  FAILED: $name\n");
    continue;
  }
  if ($dbManager->booleanFromDb($row['created'])) {
    $inserted++;
    echo "  inserted: $name\n";
  }
}
printf("thesmo-seed: %d inserted\n", $inserted);
