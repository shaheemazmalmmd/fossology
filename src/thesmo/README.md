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

## What auto-clearing on this agent means

`RULES_THESMO_NO_CONTRADICTION` is **opt-in**: it is not in the decider's
`RULES_ALL`, and turning it on schedules nomos and monk as well, because a
scanner that did not run contradicts nothing and would make the check weaker
than its name. It concludes only where thesmo's identifications are exactly
nomos's and monk's; a family name or a pointer from any of them takes no part.

Their agreeing is **not** two independent witnesses. The methods are
independent -- a model over text against hand-written footprints -- but both
vocabularies were authored partly by reading the same public licence corpora,
so a shared blind spot stays blind. Read the rule as "no scanner contradicts
this", which is what it checks, and not as corroboration from unrelated
sources.

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

### What the score means, and what it does not

`rf_match_pct` on this agent's rows is **not a text-similarity percentage**.
Monk puts a Jaccard index in that column -- how much of the licence's text the
file matched -- and the same number from this agent is a calibrated
probability: the measured precision of findings in that band, so 83 means
about 83 in 100 findings scored 83 were right. The two are not comparable, and
a mixed list sorted by that column is not ordered by anything.

`min_score` (70 in the shipped model) is a floor, not a preference: a scored
finding below it is not reported at all. So a scored finding that reaches a
reviewer is always `MEDIUM` or `HIGH`, since `status` follows the score -- 90
and over is `HIGH`, 70 and over `MEDIUM`.

**A zero does not mean weak, it means unscored.** Some findings do not come
from the head at all, so there is no margin for the verifier to turn into a
probability, and the *kind* of evidence is what makes them believable.
`status` is the field that says how strong they are:

| `match_pct` | `status` | `method` | what it is |
|---|---|---|---|
| >0 | `HIGH` / `MEDIUM` | `ml`, `ml+resolved`, ... | scored by the verifier; the number is its probability |
| 0 | `HIGH` | `spdx-tag`, `dedication`, `verbatim`, `module-licence` | the file states its licence; the statement is the evidence |
| 0 | `LOW` | `referential`, `exception-only` | the file points at a licence rather than naming one |

So an `SPDX-License-Identifier` line -- the most reliable thing a file can
carry -- is stored with `rf_match_pct` of 0 and `status` of `HIGH`. Anything
that ranks this agent's findings by the percentage alone puts that at the
bottom. Rank by `status` first, and read `method` for why.

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

## Which detector answered

`model.json` carries a version the trainer sets, and it does not move when a
table row or a threshold does, so it cannot say what the detector answers
with. That is a digest of the model directory, and the agent records engine and
model together as the `agent_rev` of its own `agent` row:

```
$ thesmo --identity -m /path/to/model
engine     4.7.1.180.ceee96
model      /path/to/model
detector   4.7.1.180.ceee96.mce19e8570c41db96
DIGEST     ce19e8570c41db96 (agrees)
```

Because the row is the detector and not just the name, a replaced model is a
new row: the ARS check finds no successful scan of its own and the upload is
offered again, the file selection offers every file rather than skipping the
ones whose answers moved, and the findings the previous model wrote stay under
its own id, so a clearance keeps pointing at the detector that produced it.

The agent takes the digest from the artifact bytes every time it starts.
`DIGEST` beside the model carries the same number for readers that cannot
afford to pass over every artifact -- the web UI reads it to tell you that
stored results came from another model, before anything has run. It never
overrides the bytes, so a stale one costs a rescan and can never mislabel a
finding; the agent logs an error where the two disagree, and `--identity` exits
non-zero, which makes it usable as a check. A model built here ships with one.
After building your own:

```
$ thesmo --write-digest -m /path/to/model
```
