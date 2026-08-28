<!-- SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD
     SPDX-License-Identifier: GPL-2.0-only -->

# Thesmo

*θεσμός — law, ordinance.*

A second-stage detector for the licence **references** nomos leaves unnamed.

Thesmo is a sibling agent, not a change to nomos. It writes its own
`license_file` rows under its own `agent_fk`, so a nomos finding can never be
overwritten by a model; `decider` combines the two through
`RULES_THESMO_NO_CONTRADICTION`.

## Where it fits

| Agent | Wins on |
|---|---|
| monk | whole licence text (`LICENSE`, `COPYING`) |
| ojo | explicit SPDX identifiers |
| nomos | licence references, names and short notices |
| **thesmo** | the references nomos cannot name |

It emits **nomos shortnames**, never SPDX ids, so its rows join `license_ref`
unchanged.

## Unseeded shortnames

nomos creates a `license_ref` row whenever it emits a name the database does not
know (`add2license_ref()`, `rf_detector_type = 2`, `rf_text` = "License by
Nomos."). It has to: its vocabulary is spread across 17k lines of `parse.c` and
is not knowable in advance. The cost is visible in any long-lived database —
1393 of 2327 rows on the development instance were created that way, carrying no
licence text, so monk cannot match them and reports show a bare name.

Thesmo does not have that problem: **its whole vocabulary is declared in
`model.json`**, so the rows can be seeded before it runs.

- `install/thesmo-seed.php <model.json> [--dry-run]` inserts exactly the
  declared names that are missing, and leaves existing rows untouched.
- At scan time an unseeded name costs **its own findings only** — the agent
  warns once at start, skips those findings, and reports how many were skipped.
  It does not abort the upload.
- `--create-missing` restores the nomos behaviour for anyone who wants parity.
  It is off by default.

## Pipeline

```
text ── normalise ──> grant gate ──> licence head ──> suffix head ──> verifier ──> rules
                          │                                              │
                     abstain if                                     abstain if
                     nothing is named                            the score is
                     or granted                                  under the floor
```

- **Grant gate** -- a binary "is a licence being *granted* here", over TF-IDF
  plus 14 dense features (a licence-name gazetteer, grant phrasings, mention
  markers, and an `enumeration_no_grant` flag). This is what keeps licence
  *enumerations* from being read as grants.
- **Licence head** -- linear, one-vs-rest over the model's classes, pruned to a
  sparse matrix and scored by the columns a window touches.
- **Suffix head** -- `only` vs `or-later`. The normaliser deliberately keeps `+`
  (as ` plus `), which the general one destroys.
- **Verifier** -- a calibrated score over the window's evidence; a finding under
  the floor is reported as nothing rather than as a guess.
- **Rules** -- what n-grams cannot represent: which of a containment family's
  conditions the file writes and how many it numbers, a licence held inside
  another, a version the file names, a choice it offers, a declaration it
  carries (SPDX tag, manifest field, LibreJS magnet), a dedication to the
  public domain, and where nothing can be named, the licence text the file
  carries in full or the place it says its licence is.

The Python package in the thesmo repository is the reference; this agent is
the same detector written in C, and `tools/validation/parity.py` there holds
the two to identical answers on every file it is given.

## Measured behaviour

The thesmo repository's README carries the current numbers, produced by the
tools under its `tools/validation/`: a corpus of dummy cases that CI must pass,
a redistributable set of real notices and texts, a local negative set the
agent must stay silent on, and 120 pinned packages scored against a
reviewer's per-file conclusions with a versioned baseline that CI fails
below. Every number there is reproduced by a command named beside it; none is
repeated here, where it would only go stale.

## Usage

```
thesmo [-m modeldir] [-J] [-v] file [file ...]
thesmo --scheduler_start -c <sysconfdir>
```

`-J` emits one JSON object per file carrying `engine`, `model_version`,
`license`, `status`, `gate`, `margin`, `match_pct`, `start`, `len`,
`exception` and `method`, so every finding is traceable to the model version,
the text it was read from and the stage that produced it. `method` reads as a
path -- `ml`, `verbatim+container`, `ml+clause-count` -- the stage first, then
each rule that changed the answer, as the Python package writes it; the
thesmo README lists the names.

### Where the time goes

`THESMO_PROFILE=1` prints, on exit, how the scan's time divided between the
phases -- normalising, the grant gate, the heads, the evidence and the
file-level rules -- so a slow tree can be read before it is guessed at. Loading
the model takes about a third of a second; a source file then costs a few
milliseconds, most of it in the windows the gate lets through. Nothing in the
agent's fast paths changes an answer: a pattern is run only where the text
carries a run of characters it cannot match without, a phrase is searched for
only where a table of the file's own runs allows it, and the head is scored by
the columns a window touches instead of every row's list -- each of which
answers exactly as the plain form does, which the parity run against the
Python reference is there to keep true.

## The model is a build artifact

Weights are **not** in this tree. Supply them at configure time:

```
cmake -DTHESMO_MODEL_DIR=/path/to/model ...
```

Without a model directory the agent builds and installs, and refuses to run with
a clear message. Training code, dataset manifests and the provenance record live
outside the agent; the model directory carries `model.json` with the version,
thresholds, class list and the metrics above.
