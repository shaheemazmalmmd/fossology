#!/bin/bash
# SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD
# SPDX-License-Identifier: GPL-2.0-only
#
# thesmo creates the license_ref rows its model names. rf_shortname is
# indexed but NOT unique, so "ON CONFLICT (rf_shortname)" has no constraint
# to match and every create failed with
#   there is no unique or exclusion constraint matching the ON CONFLICT
# which cost those licences their findings for the whole scan.
#
# Runs the statement the agent uses against the real schema, inside a
# transaction that is rolled back. Skips when no database is reachable.

CONF=${1:-$(ls /usr/local/etc/fossology/Db.conf /etc/fossology/Db.conf 2>/dev/null | head -1)}
if [ -z "$CONF" ] || [ ! -r "$CONF" ]; then
  echo "skip     - no Db.conf; database tests need a FOSSology database"
  exit 0
fi
strip() { sed -n "s/^$1=\(.*\)\$/\1/p" "$CONF" | tr -d ';' | head -1; }
PGPASSWORD=$(strip password); export PGPASSWORD
DB=$(strip dbname); U=$(strip user); H=$(strip host); H=${H:-localhost}
if ! psql -h "$H" -U "$U" -d "$DB" -tAc 'SELECT 1' >/dev/null 2>&1; then
  echo "skip     - cannot connect to $DB as $U"
  exit 0
fi

FAIL=0
q() { psql -h "$H" -U "$U" -d "$DB" -tA -v ON_ERROR_STOP=1 "$@"; }

# The precondition that broke the old statement.
uniq=$(q -c "SELECT ix.indisunique FROM pg_index ix
             JOIN pg_class i ON i.oid = ix.indexrelid
             JOIN pg_class t ON t.oid = ix.indrelid
             WHERE t.relname = 'license_ref' AND i.relname = 'rf_shortname_idx';")
if [ "$uniq" = "f" ]; then
  echo "ok       - rf_shortname is not unique, so ON CONFLICT cannot be used"
else
  echo "ok       - rf_shortname is unique here (indisunique=$uniq)"
fi

name="Thesmo-db-test-$$"
out=$(q <<SQL 2>&1
BEGIN;
PREPARE th_create (text, text) AS
WITH existing AS (
  SELECT rf_pk FROM ONLY license_ref WHERE rf_shortname = \$1
), created AS (
  INSERT INTO license_ref (rf_shortname, rf_text, rf_detector_type)
  SELECT \$1, \$2, 2 WHERE NOT EXISTS (SELECT 1 FROM existing)
  RETURNING rf_pk
)
SELECT rf_pk FROM created UNION SELECT rf_pk FROM existing;
EXECUTE th_create('$name', 'License by Thesmo.');
EXECUTE th_create('$name', 'License by Thesmo.');
EXECUTE th_create('MIT', 'License by Thesmo.');
ROLLBACK;
SQL
)
if [ $? -ne 0 ]; then
  echo "NOT ok   - creating a license_ref row failed: $out"
  FAIL=1
else
  ids=$(echo "$out" | grep -E '^[0-9]+$')
  new=$(echo "$ids" | sed -n 1p); again=$(echo "$ids" | sed -n 2p); mit=$(echo "$ids" | sed -n 3p)
  [ -n "$new" ] && echo "ok       - a name absent from license_ref is created" \
                || { echo "NOT ok   - no row created"; FAIL=1; }
  [ "$new" = "$again" ] && echo "ok       - creating it twice returns the same row" \
                        || { echo "NOT ok   - not idempotent ($new vs $again)"; FAIL=1; }
  [ -n "$mit" ] && [ "$mit" != "$new" ] && echo "ok       - an existing name returns its own row" \
                || { echo "NOT ok   - existing name resolved wrongly ($mit)"; FAIL=1; }
fi

# A licence with an exception is one row spelled the way SPDX writes the
# expression, which is how FOSSology already ships thirteen of them.
pair=$(q -c "SELECT count(*) FROM ONLY license_ref
             WHERE rf_shortname = 'GPL-2.0-only WITH Autoconf-exception-3.0';")
[ "$pair" = "1" ] && echo "ok       - a licence with an exception is one row" \
                  || { echo "NOT ok   - the compound row is spelled differently ($pair)"; FAIL=1; }

# A file that was scanned and names nothing is a license_file row with a null
# rf_fk, which is what the schema means by it. The agent writes one so that a
# clean file is not offered again on every run, and so "scanned, found
# nothing" can be told from "never scanned".
nullable=$(q -c "SELECT is_nullable FROM information_schema.columns
                 WHERE table_name = 'license_file' AND column_name = 'rf_fk';")
[ "$nullable" = "YES" ] && echo "ok       - license_file.rf_fk is nullable, so a clean scan can be recorded" \
  || { echo "NOT ok   - rf_fk is not nullable ($nullable); a clean scan has nowhere to go"; FAIL=1; }

# The counts go in ars_status, which is the column's stated purpose, so no
# schema change is needed to make them visible.
st=$(q -c "SELECT data_type FROM information_schema.columns
           WHERE table_name = 'ars_master' AND column_name = 'ars_status';")
[ "$st" = "text" ] && echo "ok       - ars_status is text, so the per-upload counts fit" \
  || { echo "NOT ok   - ars_status is $st"; FAIL=1; }

# The whole retry story rests on this: the file list offers a pfile only while
# this agent holds no row for it. That makes a rolled-back file eligible again
# and a committed one filtered out -- and it is why a half-written file would
# be excluded for good. Run the agent's own selection against dummy rows.
#
# The first look is the control: with no rows written both files have to come
# back, or the scaffolding failed to insert and every check below would pass
# by selecting nothing.
sel() {
  cat <<SQL
SELECT 'offered$1:' || coalesce(string_agg(pfile_pk::text, ',' ORDER BY pfile_pk), 'none')
FROM (SELECT distinct(pfile_fk) AS PF FROM uploadtree
      WHERE upload_fk = 2000000001 AND (ufile_mode&x'3C000000'::int) = 0) AS SS
  LEFT OUTER JOIN license_file ON (PF = pfile_fk AND agent_fk = 999999)
  INNER JOIN pfile ON PF = pfile_pk
WHERE (fl_pk IS NULL OR agent_fk <> 999999);
SQL
}
out=$(q <<SQL 2>&1
BEGIN;
INSERT INTO upload (upload_pk, upload_filename, upload_mode, upload_origin)
  VALUES (2000000001, 'thesmo-db-test', 104, 'thesmo-db-test');
INSERT INTO pfile (pfile_pk, pfile_md5, pfile_sha1, pfile_size) VALUES
  (2000000001, '00000000000000000000000000000001',
               '0000000000000000000000000000000000000001', 1),
  (2000000002, '00000000000000000000000000000002',
               '0000000000000000000000000000000000000002', 2);
INSERT INTO uploadtree (uploadtree_pk, upload_fk, pfile_fk, ufile_mode, ufile_name) VALUES
  (2000000001, 2000000001, 2000000001, 0, 'held.txt'),
  (2000000002, 2000000001, 2000000002, 0, 'fresh.txt');
$(sel 0)
-- a finding for the first file
INSERT INTO license_file (rf_fk, agent_fk, pfile_fk) VALUES (1, 999999, 2000000001);
$(sel 1)
-- and for the second the clean marker, which must hold it just the same
INSERT INTO license_file (agent_fk, pfile_fk) VALUES (999999, 2000000002);
$(sel 2)
ROLLBACK;
SQL
)
got() { echo "$out" | sed -n "s/^offered$1://p"; }
expect() {
  if [ "$(got "$1")" = "$2" ]; then
    echo "ok       - $3"
  else
    echo "NOT ok   - $3 (offered '$(got "$1")', expected '$2')"
    echo "$out" | sed 's/^/           /'
    FAIL=1
  fi
}
expect 0 "2000000001,2000000002" "with nothing written both files are offered"
expect 1 "2000000002" "a pfile holding a finding is not offered again"
expect 2 "none" "a clean marker holds the file as a finding does"

# An agent is (agent_name, agent_rev) and the schema says so: that pair is
# unique. The shared helper takes a revision and uses it only in the INSERT it
# runs when no row carries the name at all, selecting the latest row by name
# ever after -- so a changed detector reuses the old row, the ARS check reports
# the upload already scanned, and the file selection skips exactly the files
# whose answers moved. The last look below is that behaviour, kept here as the
# reason the revision has to be in the predicate.
cons=$(q -c "SELECT count(*) FROM pg_constraint
             WHERE conname = 'agent_unique_name_rev';")
[ "$cons" = "1" ] && echo "ok       - (agent_name, agent_rev) is unique, so the insert can name it" \
  || { echo "NOT ok   - agent_unique_name_rev is missing; the insert has no constraint to name"; FAIL=1; }

an="thesmo-db-test-$$"
out=$(q <<SQL 2>&1
BEGIN;
PREPARE th_agent (text, text, text) AS
WITH made AS (
  INSERT INTO agent (agent_name, agent_desc, agent_enabled, agent_rev)
  VALUES (\$1, \$2, true, \$3)
  ON CONFLICT (agent_name, agent_rev) DO NOTHING
  RETURNING agent_pk
)
SELECT agent_pk FROM made
UNION ALL
SELECT agent_pk FROM agent WHERE agent_name = \$1 AND agent_rev = \$3
LIMIT 1;
EXECUTE th_agent('$an', 'thesmo test', '1.0.m1111111111111111');
EXECUTE th_agent('$an', 'thesmo test', '1.0.m1111111111111111');
EXECUTE th_agent('$an', 'thesmo test', '1.0.m2222222222222222');
SELECT 'latest:' || agent_pk FROM agent
  WHERE agent_name = '$an' ORDER BY agent_ts DESC LIMIT 1;
ROLLBACK;
SQL
)
if [ $? -ne 0 ]; then
  echo "NOT ok   - resolving an agent identity failed: $out"
  FAIL=1
else
  ids=$(echo "$out" | grep -E '^[0-9]+$')
  one=$(echo "$ids" | sed -n 1p); again=$(echo "$ids" | sed -n 2p)
  two=$(echo "$ids" | sed -n 3p)
  [ -n "$one" ] && echo "ok       - a detector with no row yet gets one" \
                || { echo "NOT ok   - no agent row created: $out"; FAIL=1; }
  [ "$one" = "$again" ] && echo "ok       - the same engine and model resolve to the same row" \
    || { echo "NOT ok   - not idempotent ($one vs $again)"; FAIL=1; }
  [ -n "$two" ] && [ "$two" != "$one" ] \
    && echo "ok       - a changed model is a different identity" \
    || { echo "NOT ok   - a changed model reused row $one"; FAIL=1; }
  # The shape the shared helper uses, kept here as the defect's own record: it
  # takes no revision, so one pk answers for every detector that shares the
  # name -- and where two rows carry the same agent_ts, as two registered in
  # one transaction do, which pk comes back is not even settled.
  latest=$(echo "$out" | sed -n 's/^latest://p')
  if [ "$latest" = "$one" ] || [ "$latest" = "$two" ]; then
    echo "ok       - the latest-by-name look answers $latest for both $one and $two"
  else
    echo "NOT ok   - the latest-by-name look returned $latest, neither $one nor $two"
    FAIL=1
  fi
fi
stray_agent=$(q -c "SELECT count(*) FROM agent WHERE agent_name = '$an';")
[ "$stray_agent" = "0" ] && echo "ok       - the dummy agent rows were rolled back" \
  || { echo "NOT ok   - $stray_agent dummy agent rows remain"; FAIL=1; }

left=$(q -c "SELECT count(*) FROM ONLY license_ref WHERE rf_shortname = '$name';")
[ "$left" = "0" ] && echo "ok       - the test left nothing behind" \
                  || { echo "NOT ok   - $left test rows remain"; FAIL=1; }
stray=$(q -c "SELECT count(*) FROM license_file WHERE agent_fk = 999999;")
[ "$stray" = "0" ] && echo "ok       - the dummy rows were rolled back" \
                   || { echo "NOT ok   - $stray dummy license_file rows remain"; FAIL=1; }
exit $FAIL
