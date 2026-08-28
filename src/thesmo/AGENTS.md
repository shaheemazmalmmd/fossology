<!-- SPDX-FileCopyrightText: © 2026 Fossology contributors

     SPDX-License-Identifier: GPL-2.0-only
-->
# Thesmo Agent Guide

Thesmo answers one question: which license does this file state? It is judged
on precision first, because a wrong identification costs a reviewer more than
a miss. Where it cannot say confidently, it must say nothing, or point
(`See-file.LICENSE`, `Same-license-as`), rather than guess. A change that
raises recall by lowering that bar is a regression even when the numbers go up.

## Read first

- [README.md](README.md) for what this agent does, what auto-clearing on it
  means, and where the time goes. It is current; do not restate it in code
  comments.
- [../../CONTRIBUTING.md](../../CONTRIBUTING.md) for the commit conventions,
  the DCO sign-off and the link to the project Coding Style wiki page.
- The Thesmo Python repository's `CONTRIBUTING.md`, which governs this
  directory as much as its own. The two are one detector.

## The rule that governs every other one

This agent and `Thesmo/src/thesmo/Thesmo.py` in the Thesmo repository are one
detector written twice. `thesmo_model.c` mirrors it rule for rule, in the same
order, with the same thresholds. **A change to one is a change to both**, and
`tools/validation/parity.py` over the corpus and the quick tier must report
zero differences.

Half a port is the characteristic failure here, and it is quiet. Python's
`_pd_instrument` has two arms: a scan of the license's name words, and an
explicit list of licenses whose own text *is* a public domain disclaimer
(`_PD_BY_ITS_TEXT`). The C side carried the first arm and not the second, so
SQLite's `blessing` was not recognized as such, the dedication was never
suppressed, and every file carrying that header reported two licenses instead
of one. Nothing failed: the agent ran, answered, and was wrong on one package.

When porting, port the whole condition, including the literal lists. When
reviewing a port, read both sides of every `if` rather than the diff.

## The model is a build artifact

Do not hand edit anything under the model directory. The tables are written by
`utility/build_*.py` in the Thesmo repository from the SPDX reference texts and
FOSSology's own license data; a hand edited table is a fix nobody can rebuild,
and `tests/test_model_tables.py` there fails when a table is not what its
builder writes.

`DIGEST` records the bytes of every other file in the directory, and
`--identity` checks it. A model file that changes without `DIGEST` being
rewritten (`thesmo --write-digest`) leaves the agent reporting a detector
identity that does not match what it loaded, so the two must travel in one
commit.

## Writing or changing a curated pattern

`referential.tsv` rows are compiled with POSIX `regcomp`, not with Python's
`re`:

```c
regcomp(re + m->nRef, pattern,
        where == 5 ? REG_EXTENDED : (REG_EXTENDED | REG_ICASE))
```

`where == 5` is `identify-raw`, the one kind read case sensitively because the
case of a name is part of the shape. Everything else gets `REG_ICASE`. The
loader spells out `\n` and `\t` into real control characters before compiling;
no other escape inside a bracket expression means anything to POSIX.

**A pattern that fails to compile is skipped without a word** (`if (regcomp(...)
!= 0) continue;`). The agent then runs normally and that rule simply does not
exist. This is worse than a crash: a lookahead guard once tested perfectly in
Python, compiled nowhere in C, and the only symptom was one package's accuracy
moving.

Worse still is a construct both engines accept and read differently. `\d`
compiles under `REG_EXTENDED` and matches the letter `d`. Nothing fails at all.

The Thesmo repository gates this, so run its suite after touching a pattern:

| Check | Catches |
|---|---|
| `tests/test_pattern_portability.py` | `(?...)` groups, lazy quantifiers, backreferences, `\A \Z \z`, stray backslashes inside brackets and `\d`, over `referential.tsv` and `model.json`'s grant and mention patterns; then compiles every one through libc `regcomp` with the flags above, so a construct nobody enumerated is still caught |
| `tests/test_validation_tools.py::test_every_referential_row_is_posix_ere` | the same syntax rules over `referential.tsv` |

## Database and scheduler

- Findings are stored in `license_file`; the agent registers itself in `agent`
  keyed on `(agent_name, agent_rev)`.
- A license name that has no `license_ref` row cannot be stored. New names are
  FOSSology's and SPDX's to mint, not this agent's: a new shortname creates a
  `license_ref` row and fragments clearance state across installations.
  `--create-missing` exists for controlled use, and is not the default for a
  reason. See "Unseeded shortnames" in the README.
- When joining findings to `ars_master` descendants, do not join on `agent_fk`
  alone: deduplicated pfiles lose findings that way, and a clean local database
  does not show it.

## Limits you will meet

Declared in `agent/thesmo_model.h`, and they bound behavior as much as
performance:

- `TH_MAX_FINDINGS 8` licenses reported per file, from `TH_KEEP_MAX 64`
  collected before the limit applies. The cap both bounds the work and blinds
  the agent on catalog-like files.
- `TH_WHOLE_MAX 65536` governs scoring, not the containment test.
- `TH_TEXT_GAP`, `TH_TEXT_RUN` and `TH_TEXT_DENSITY` define what counts as a
  run of one license's wording. Changing any of them changes what every file
  matches.

## Build and test

Build this agent alone, from the repository root:

```sh
cmake -DCMAKE_BUILD_TYPE=Debug -GNinja -DTESTING=ON -DAGENTS="thesmo" -S . -B ./build
cmake --build build --config Debug --parallel
cd build && ctest -R thesmo --output-on-failure
```

Registered tests: `thesmo_functional_cli_test`, `thesmo_db_test`,
`thesmo_vocabulary_test`, `thesmo_commit_test`, `thesmo_model_format_test`,
`thesmo_scoped_rules_test`. The database tests need a PostgreSQL instance:
`./utils/prepare-test -afty -u postgres -p postgres` sets one up.

For accuracy work, build Release and hand the binary to the Thesmo
repository's tools with `--agent`. Two cautions that have each cost a day:
copy the binary somewhere stable before running gates against it, because a
rebuild under a running gate silently changes what is being measured; and the
current checkout is not a valid "before" binary, so build the comparison from
the commit you mean to compare against.

## Before you open a pull request

- Say what you expected to change in the numbers, and report the gates as they
  printed. A baseline moved to make a run green is refused.
- Ship the Python side of the same change, and the parity run that shows zero
  differences.
- Keep measurements, corpus names and benchmark numbers out of source
  comments. They belong in the pull request. A comment says what a rule reads
  and why, in the register of the code around it: one sentence, plain ASCII,
  American English, one space after the opener and no padding to align text
  across lines. Run `clang-format` (the repository ships `.clang-format`) on
  what you touched.
- Keep the SPDX header on every file, in the form the neighbouring sources use.
- Do not add a rule for one file. A rule has to say something true about how
  licenses are written, and be measured on more than the case that prompted it.
