#!/bin/bash
# SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD
# SPDX-License-Identifier: GPL-2.0-only
#
# Which names this model is answerable for, and which it leaves alone.
#
# Every learned class needs a license_ref row, because the agent skips a
# finding it cannot name and does not mint rows the way nomos does through
# add2license_ref(), so the finding would simply be lost.
#
# The families and the pointers the referential table names -- "LGPL" with no
# version, "See-file.LICENSE" -- are deliberately NOT ours to seed. They are
# FOSSology's own vocabulary: nomos emits nearly all of them and mints the row
# itself, and nomos is a dependency of this agent, so it has run over the same
# upload first. A second owner for a name FOSSology already manages is how a
# database ends up with two rows for one name and clearance state split
# between them.
#
# A name beginning NOT- is a denial the rules read as a guard and never
# report, so it is not listed at all.
#
# This reads the shipped seed data and the model, and needs no database.

HERE=$(cd "$(dirname "$0")" && pwd)
SEED="$HERE/../../install/thesmo-seed.php"
REF="$HERE/../../../../install/db/licenseRef.json"
MODEL=${1:-$THESMO_MODEL_DIR}

if [ -z "$MODEL" ] || [ ! -r "$MODEL/model.json" ]; then
  echo "skip     - no model; pass its directory or set THESMO_MODEL_DIR"
  exit 0
fi
if [ ! -r "$REF" ]; then
  echo "NOT ok   - cannot read $REF"
  exit 1
fi
command -v php >/dev/null 2>&1 || { echo "skip     - no php"; exit 0; }

FAIL=0

# What the seeder says it is answerable for.
names=$(php "$SEED" "$MODEL/model.json" --list 2>/dev/null | sed -n 's/^\(class\|referential\)\t//p')
count=$(echo "$names" | grep -c .)
if [ "$count" -lt 100 ]; then
  echo "NOT ok   - the seeder listed $count names; an empty read would pass everything below"
  exit 1
fi
echo "ok       - the seeder lists $count reportable names"

refcount=$(php "$SEED" "$MODEL/model.json" --list 2>/dev/null | grep -c '^referential')
if [ "$refcount" -gt 0 ]; then
  echo "ok       - $refcount of them come from the referential table, not the classes"
else
  echo "NOT ok   - no referential names listed, so the families and pointers are uncovered"
  FAIL=1
fi

# A denial is a guard, never a finding.
if php "$SEED" "$MODEL/model.json" --list 2>/dev/null | grep -q 'NOT-'; then
  echo "NOT ok   - a NOT- denial is listed for seeding; it is never reported"
  FAIL=1
else
  echo "ok       - NOT- denials are not offered for seeding"
fi

# Every learned class has to be in the shipped seed data. Checked against the
# file rather than a database, so this fails in review and not in production.
want=$(mktemp) || exit 1
trap 'rm -f "$want"' EXIT
php "$SEED" "$MODEL/model.json" --list 2>/dev/null | sed -n 's/^class\t//p' > "$want"
learned=$(grep -c . "$want")
if [ "$learned" -lt 100 ]; then
  echo "NOT ok   - only $learned learned classes listed; an empty read would pass below"
  exit 1
fi
echo "ok       - $learned of them are this model's learned classes"
missing=$(python3 -c '
import json, sys
shipped = {r["rf_shortname"] for r in json.load(open(sys.argv[1], encoding="utf-8"))}
with open(sys.argv[2], encoding="utf-8") as fh:
    want = [l.strip() for l in fh if l.strip()]
print("\n".join(n for n in want if n not in shipped))
' "$REF" "$want")
if [ -z "$missing" ]; then
  echo "ok       - every learned class is in licenseRef.json"
else
  n=$(echo "$missing" | grep -c .)
  echo "NOT ok   - $n learned classes are not in licenseRef.json:"
  echo "$missing" | sed 's/^/           /'
  FAIL=1
fi

# And the families and pointers are left alone: a row added here would make
# this model a second owner of a name FOSSology already manages.
php "$SEED" "$MODEL/model.json" --list 2>/dev/null | sed -n 's/^referential\t//p' > "$want"
ours=$(python3 -c '
import json, sys
shipped = {r["rf_shortname"] for r in json.load(open(sys.argv[1], encoding="utf-8"))}
with open(sys.argv[2], encoding="utf-8") as fh:
    marks = [l.strip() for l in fh if l.strip()]
# the ones FOSSology already ships are its own and are not the question here
print("\n".join(n for n in marks if n in shipped and n not in (
    "BSD", "GPL", "LGPL", "MIT-style", "Dual-license", "Public-domain",
    "See-file", "UnclassifiedLicense")))
' "$REF" "$want")
if [ -z "$ours" ]; then
  echo "ok       - no family or pointer was seeded by this model"
else
  echo "NOT ok   - this model seeded names FOSSology owns:"
  echo "$ours" | sed 's/^/           /'
  FAIL=1
fi

exit $FAIL
