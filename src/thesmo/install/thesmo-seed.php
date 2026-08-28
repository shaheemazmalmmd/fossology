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
 * Usage: php thesmo-seed.php <model.json> [--dry-run|--list]
 *
 * --list prints every name the agent can report, and which kind it is, without
 * opening a database: what the seeder is answerable for, separately from what
 * any one database happens to hold.
 */

$modelPath = $argv[1] ?? null;
$dryRun = in_array('--dry-run', $argv, true);
$listOnly = in_array('--list', $argv, true);
if (!$modelPath || !is_readable($modelPath)) {
  fwrite(STDERR, "usage: thesmo-seed.php <model.json> [--dry-run]\n");
  exit(1);
}
$model = json_decode(file_get_contents($modelPath), true);
if (!is_array($model) || !isset($model['classes']) || !is_array($model['classes'])
    || !$model['classes']) {
  fwrite(STDERR, "thesmo-seed: no usable 'classes' in $modelPath\n");
  exit(1);
}

/* The learned classes are not everything the agent reports: the referential
 * table beside the model also names families and pointers -- "LGPL" where no
 * version is stated, "See-file.LICENSE" where the file says where its terms
 * are. Those are listed here so an operator can see them, and deliberately
 * NOT seeded. They are FOSSology's own vocabulary, not this model's: nomos
 * emits nearly all of them and mints the row itself through
 * add2license_ref(), and nomos is a dependency of this agent, so it has run
 * over the same upload before thesmo writes anything. Seeding them here would
 * add a second owner for a name FOSSology already manages.
 *
 * A name beginning NOT- is a denial the rules read as a guard and never
 * report, so it is not listed at all. */
$referential = [];
$refPath = dirname($modelPath) . '/referential.tsv';
if (is_readable($refPath)) {
  foreach (file($refPath, FILE_IGNORE_NEW_LINES) as $line) {
    if ($line === '' || $line[0] === '#') {
      continue;
    }
    $name = explode("\t", $line)[0];
    if ($name === '' || strncmp($name, 'NOT-', 4) === 0) {
      continue;
    }
    $referential[$name] = true;
  }
} else {
  fwrite(STDERR, "thesmo-seed: no referential.tsv beside the model, so the "
    . "families and pointers it names are not checked\n");
}

/* Every kind the agent can report, and which kind each name came from, so a
 * missing learned class reads differently from a missing pointer. */
$kinds = [];
foreach ($model['classes'] as $name) {
  $kinds[$name] = 'class';
}
foreach (array_keys($referential) as $name) {
  if (!isset($kinds[$name])) {
    $kinds[$name] = 'referential';
  }
}
$declared = array_keys($kinds);
/* Only the learned classes are this model's to seed. */
$seedable = $model['classes'];

if ($listOnly) {
  foreach ($declared as $name) {
    printf("%s\t%s\n", $kinds[$name], $name);
  }
  printf("thesmo-seed: %d names (%d learned, %d referential)\n",
    count($declared), count($model['classes']),
    count($declared) - count($model['classes']));
  exit(0);
}

// <repo>/src/thesmo/install and <moddir>/thesmo/install both sit two levels
// under the directory that holds lib/php.
require_once dirname(__DIR__, 2) . '/lib/php/bootstrap.php';
$dbManager = $GLOBALS['container']->get('db.manager');

$missing = [];
$unseeded = [];
foreach (array_keys($referential) as $name) {
  if (isset($kinds[$name]) && $kinds[$name] === 'referential') {
    $unseeded[] = $name;
  }
}
foreach ($seedable as $name) {
  $row = $dbManager->getSingleRow(
    'SELECT rf_pk FROM license_ref WHERE rf_shortname = $1',
    [$name], 'thesmo.seed.lookup');
  if (empty($row)) {
    $missing[] = $name;
  }
}

printf("thesmo-seed: %d learned classes, %d already present, %d missing\n",
  count($seedable), count($seedable) - count($missing), count($missing));
if ($unseeded) {
  printf("thesmo-seed: %d families and pointers are left to FOSSology's own "
    . "vocabulary and are not seeded here\n", count($unseeded));
}

if (!$missing) {
  exit(0);
}
if ($dryRun) {
  foreach ($missing as $name) {
    printf("  would insert: %-40s (%s)\n", $name, $kinds[$name]);
  }
  exit(0);
}
$inserted = 0;
$failed = [];
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
    $failed[] = $name;
    continue;
  }
  if ($dbManager->booleanFromDb($row['created'])) {
    $inserted++;
    printf("  inserted: %-40s (%s)\n", $name, $kinds[$name]);
  }
}
printf("thesmo-seed: %d inserted, %d failed\n", $inserted, count($failed));
/* A seeder that cannot write is a packaging step that did not happen, and the
 * agent will skip every finding under the names it left unresolved. Say so in
 * the exit status, or an installer reports success and the loss shows up later
 * as findings nobody missed. */
if ($failed) {
  fwrite(STDERR, "thesmo-seed: unresolved: " . implode(', ', $failed) . "\n");
  exit(1);
}
