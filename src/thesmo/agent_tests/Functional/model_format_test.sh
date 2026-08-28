#!/bin/bash
# SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD
# SPDX-License-Identifier: GPL-2.0-only
#
# What the model file format promises, and what happens when a file breaks it.
#
# The numbers are little-endian on every platform. That was not always
# explicit: the headers were packed little-endian while the weights were read
# in the machine's own order, so on a big-endian build the shape parsed, every
# check passed, and the scores were quietly wrong. Both engines now convert,
# and this pins the part that can be tested on one machine -- a shape the file
# cannot really have is refused rather than used to size an allocation.
#
# Needs the built agent and a model; it copies the model and corrupts the copy.

HERE=$(cd "$(dirname "$0")" && pwd)
AGENT=${THESMO_C_AGENT:-$1}
MODEL=${2:-$THESMO_MODEL_DIR}

if [ -z "$AGENT" ] || [ ! -x "$AGENT" ]; then
  echo "skip     - no agent; pass its path or set THESMO_C_AGENT"
  exit 0
fi
if [ -z "$MODEL" ] || [ ! -r "$MODEL/lic.bin" ]; then
  echo "skip     - no model; pass its directory or set THESMO_MODEL_DIR"
  exit 0
fi
command -v python3 >/dev/null 2>&1 || { echo "skip     - no python3"; exit 0; }

WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT
echo "a file with no licence in it" > "$WORK/subject.txt"
FAIL=0

# A good model answers, so a refusal below is the corruption and not the setup.
if "$AGENT" -m "$MODEL" "$WORK/subject.txt" >/dev/null 2>&1; then
  echo "ok       - the shipped model loads and scans"
else
  echo "NOT ok   - the shipped model does not load; the cases below prove nothing"
  exit 1
fi

# Each case breaks one field of lic.bin and the agent must refuse. Reading a
# shape of 0 rows, or a row claiming more columns than exist, means a
# truncated or foreign file -- and both numbers size an allocation.
for case in zero_rows negative_cols row_longer_than_the_shape; do
  rm -rf "$WORK/model"
  cp -r "$MODEL" "$WORK/model" || exit 1
  python3 - "$WORK/model/lic.bin" "$case" <<'PY'
import struct, sys
path, case = sys.argv[1], sys.argv[2]
d = bytearray(open(path, "rb").read())
rows, cols = struct.unpack("<ii", bytes(d[:8]))
if case == "zero_rows":
    d[:8] = struct.pack("<ii", 0, cols)
elif case == "negative_cols":
    d[:8] = struct.pack("<ii", rows, -1)
else:
    at = 8 + 4 * rows                      # past the header and the bias
    d[at:at + 4] = struct.pack("<i", cols + 1)
open(path, "wb").write(bytes(d))
PY
  if "$AGENT" -m "$WORK/model" "$WORK/subject.txt" >/dev/null 2>&1; then
    echo "NOT ok   - $case was accepted"
    FAIL=1
  else
    echo "ok       - $case is refused"
  fi
done

# The headers say little-endian explicitly, which is what makes the format
# portable at all. A file whose header is byte-swapped is a foreign file.
rm -rf "$WORK/model"
cp -r "$MODEL" "$WORK/model" || exit 1
python3 - "$WORK/model/lic.bin" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
rows, cols = struct.unpack("<ii", bytes(d[:8]))
d[:8] = struct.pack(">ii", rows, cols)     # the other order
open(sys.argv[1], "wb").write(bytes(d))
PY
if "$AGENT" -m "$WORK/model" "$WORK/subject.txt" >/dev/null 2>&1; then
  echo "NOT ok   - a big-endian header was accepted"
  FAIL=1
else
  echo "ok       - a big-endian header is refused"
fi

exit $FAIL
