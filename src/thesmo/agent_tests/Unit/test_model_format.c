/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief What a weight file is allowed to say
 *
 * The model directory is named on the command line, so its bytes are input
 * like any other. Two properties of a sparse row are not checks the reader
 * can skip:
 *
 * Every index is a subscript. The column index is scatter-added into an
 * array sized by the declared column count, so an index outside that range
 * writes outside the allocation. Validating how *many* indices a row holds
 * is not the same check and does not stand in for it.
 *
 * Every index is in order. The dot product merges two runs by index, so a
 * row out of order silently drops terms: no crash, a wrong score, and a
 * licence reported or missed on the strength of it.
 *
 * The loader is reached by including the translation unit, because these are
 * properties of the file format rather than of the agent's interface.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "thesmo_model.c"

static int failures;

static void check(int ok, const char *what)
{
  printf("%s - %s\n", ok ? "ok      " : "NOT ok  ", what);
  if (!ok) failures++;
}

/** One row of `n` indices over `cols` columns, in the on-disk layout. */
static void writeRow(const char *path, int cols, int n, const int *idx)
{
  FILE *f = fopen(path, "wb");
  int hdr[2], i;
  float bias = 0.0f, v = 1.0f;
  if (!f) { perror("fopen"); exit(2); }
  hdr[0] = 1; hdr[1] = cols;
  fwrite(hdr, sizeof(int), 2, f);
  fwrite(&bias, sizeof(float), 1, f);
  fwrite(&n, sizeof(int), 1, f);
  for (i = 0; i < n; i++) fwrite(&idx[i], sizeof(int), 1, f);
  for (i = 0; i < n; i++) fwrite(&v, sizeof(float), 1, f);
  fclose(f);
}

/** Whether the loader accepted the file. Frees what a load leaves behind:
    the row arrays are released by the transpose, the column form is not. */
static int loads(const char *path)
{
  thLinear L;
  int rc, r;
  memset(&L, 0, sizeof(L));
  rc = thLoadSparse(&L, path);
  free(L.bias);
  if (L.idx) for (r = 0; r < L.rows; r++) { free(L.idx[r]); free(L.val[r]); }
  free(L.idx); free(L.val); free(L.nnz);
  free(L.colPtr); free(L.colRow); free(L.colVal); free(L.acc);
  return rc == 0;
}

int main(void)
{
  char path[] = "/tmp/thesmo_model_format_XXXXXX";
  int fd = mkstemp(path);
  int ordered[2]   = {0, 2};
  int none[1]      = {0};
  int farPast[1]   = {1073741824};    /* 0x40000000: past any real column */
  int negative[1]  = {-1000};
  int reversed[2]  = {2, 0};
  int repeated[2]  = {1, 1};
  int atTheEdge[1] = {4};             /* cols is 4, so 4 is one too far */

  if (fd < 0) { perror("mkstemp"); return 2; }
  close(fd);

  writeRow(path, 4, 2, ordered);
  check(loads(path), "a row of ordered indices inside the column space loads");

  /* A row carrying no weights is legal, and malloc(0) may answer NULL, so a
     reader that checks that answer refuses a file it should accept. */
  writeRow(path, 4, 0, none);
  check(loads(path), "a row of no weights loads");

  writeRow(path, 4, 1, farPast);
  check(!loads(path), "an index past the column space is refused");

  writeRow(path, 4, 1, atTheEdge);
  check(!loads(path), "an index one past the last column is refused");

  writeRow(path, 4, 1, negative);
  check(!loads(path), "a negative index is refused");

  writeRow(path, 4, 2, reversed);
  check(!loads(path), "indices out of order are refused");

  writeRow(path, 4, 2, repeated);
  check(!loads(path), "a repeated index is refused");

  unlink(path);
  printf("%s\n", failures ? "FAILED" : "the format admits only what it can read");
  return failures ? 1 : 0;
}
