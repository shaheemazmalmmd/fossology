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

left=$(q -c "SELECT count(*) FROM ONLY license_ref WHERE rf_shortname = '$name';")
[ "$left" = "0" ] && echo "ok       - the test left nothing behind" \
                  || { echo "NOT ok   - $left test rows remain"; FAIL=1; }
exit $FAIL
