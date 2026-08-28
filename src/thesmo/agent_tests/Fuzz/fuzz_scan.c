/*
 SPDX-FileCopyrightText: © 2026 Fossology contributors

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief libFuzzer entry point for the scan path.
 *
 * Every byte the agent classifies came from an uploaded file, so this is the
 * one interface an attacker chooses the input to. The model is loaded once and
 * kept: loading it per case would spend the whole run in thLoad and reach the
 * classifier a few thousand times instead of a few million.
 *
 * THESMO_MODEL_DIR names the model, because a fuzzer gets no arguments. With
 * none the harness aborts rather than return, so a misconfigured run fails
 * instead of reporting millions of executions over no model at all.
 *
 *     clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *         agent_tests/Fuzz/fuzz_scan.c agent/thesmo_model.c ... -o fuzz_scan
 *     THESMO_MODEL_DIR=/path/to/model ./fuzz_scan corpus/ -max_total_time=120
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "thesmo_model.h"

static thModel *model;

/** Load once, on the first case. */
static void setup(void)
{
  const char *dir = getenv("THESMO_MODEL_DIR");
  char err[256] = {0};
  if (!dir || !*dir) {
    fprintf(stderr, "fuzz_scan: set THESMO_MODEL_DIR to a model directory\n");
    abort();
  }
  model = thLoad(dir, err, sizeof(err));
  if (!model) {
    fprintf(stderr, "fuzz_scan: cannot load %s: %s\n", dir, err);
    abort();
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  /* The classifier takes a C string, so the case is copied and terminated:
     fuzzer input is not NUL terminated and reading past it is the harness's
     bug rather than the agent's. */
  thResult out[TH_MAX_FINDINGS];
  char names[8192];
  char *text;
  int dropped = 0;

  if (!model) setup();
  /* A case larger than the agent would ever read teaches nothing: the caller
     reads at most this much of a file, so beyond it the input is the same
     input twice. */
  if (size > (1u << 20)) size = 1u << 20;
  text = (char *) malloc(size + 1);
  if (!text) return 0;
  memcpy(text, data, size);
  text[size] = '\0';

  memset(out, 0, sizeof(out));
  thClassifyAll(model, text, out, TH_MAX_FINDINGS, names, sizeof(names),
                &dropped);

  free(text);
  return 0;
}
