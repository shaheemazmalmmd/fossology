/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief TF-IDF + linear inference for the Thesmo agent
 *
 * Mirrors sklearn: sublinear tf, idf weighting and L2 normalization per block,
 * blocks concatenated in the order the model was fitted.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <ctype.h>
#include <regex.h>
#include "thesmo_model.h"

/**
 * \brief realloc that does not leak the block it was handed when it fails.
 *
 * Every caller treats NULL as "stop", so freeing here is what stops the leak.
 */
static void *thGrow(void *p, size_t n)
{
  void *q = realloc(p, n);
  if (!q) free(p);
  return q;
}

/* ---------- string hash ---------- */

typedef struct thEntry { char *key; size_t len, hash; int val; struct thEntry *next; } thEntry;
/* Entries and keys are cut from chunks: a vocabulary is hundreds of thousands
   of tiny allocations, freed only all at once. */
typedef struct thChunk { struct thChunk *next; size_t used; char data[1]; } thChunk;
struct thHash { thEntry **b; size_t n, used; thChunk *chunk; };
#define TH_CHUNK (1u << 16)

static void *thArena(struct thHash *h, size_t bytes)
{
  bytes = (bytes + 15) & ~(size_t) 15;
  if (bytes > TH_CHUNK / 4) return NULL;
  if (!h->chunk || h->chunk->used + bytes > TH_CHUNK) {
    thChunk *c = malloc(sizeof(*c) + TH_CHUNK);
    if (!c) return NULL;
    c->next = h->chunk; c->used = 0; h->chunk = c;
  }
  { void *at = h->chunk->data + h->chunk->used;
    h->chunk->used += bytes;
    return at; }
}

/** \brief FNV-1a of len bytes: a term is looked up in place, uncopied */
static size_t thHashBytes(const char *s, size_t len)
{
  size_t h = 1469598103934665603UL, i;
  for (i = 0; i < len; i++) { h ^= (unsigned char) s[i]; h *= 1099511628211UL; }
  return h;
}

static size_t thHashStr(const char *s, size_t n)
{
  return thHashBytes(s, strlen(s)) & (n - 1);
}

static struct thHash *thHashNew(size_t cap)
{
  size_t n = 16;
  struct thHash *h;
  while (n < cap * 2) n <<= 1;
  h = calloc(1, sizeof(*h));
  if (!h) return NULL;
  h->b = calloc(n, sizeof(thEntry *));
  if (!h->b) { free(h); return NULL; }
  h->n = n;
  return h;
}

static int thHashPut(struct thHash *h, const char *k, int v)
{
  size_t len = strlen(k), hash = thHashBytes(k, len), i = hash & (h->n - 1);
  thEntry *e = thArena(h, sizeof(*e));
  if (!e) return -1;
  e->key = thArena(h, len + 1);
  if (!e->key) return -1;
  memcpy(e->key, k, len + 1);
  e->len = len; e->hash = hash;
  e->val = v; e->next = h->b[i]; h->b[i] = e; h->used++;
  return 0;
}

static int thHashGet(struct thHash *h, const char *k, size_t len)
{
  thEntry *e;
  size_t hash;
  if (len >= 512) return -1;
  hash = thHashBytes(k, len);
  for (e = h->b[hash & (h->n - 1)]; e; e = e->next)
    if (e->hash == hash && e->len == len && memcmp(e->key, k, len) == 0) return e->val;
  return -1;
}

static void thHashFree(struct thHash *h)
{
  thChunk *c;
  if (!h) return;
  for (c = h->chunk; c;) { thChunk *nx = c->next; free(c); c = nx; }
  free(h->b); free(h);
}

/* ---------- normalization ----------
   Must match the training normalizer, or the model sees text it was not fit on. */
char *thNormalise(const char *in)
{
  size_t n = strlen(in), i, j = 0;
  char *t = malloc(n * 6 + 16), *o;
  size_t ocap;
  if (!t) return NULL;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char) in[i];
    /* A comment marker made of letters survives into the text and splits the
       sentence it introduces: an m4 file writes "gives unlimited\ndnl
       permission", which without this reads "unlimited dnl permission" and
       matches no phrase spanning the line. */
    if ((i == 0 || in[i - 1] == '\n' || in[i - 1] == '\r')) {
      size_t k = i;
      unsigned char c0;
      while (in[k] == ' ' || in[k] == '\t') k++;
      /* Cheap first: normalise runs once per window, so a line start is
         visited many times over and two compares there are not free. */
      c0 = (unsigned char) in[k];
      if ((c0 == 'd' || c0 == 'D' || c0 == 'r' || c0 == 'R') &&
          ((!strncasecmp(in + k, "dnl", 3) && (in[k + 3] == ' ' || in[k + 3] == '\t')) ||
           (!strncasecmp(in + k, "rem", 3) && (in[k + 3] == ' ' || in[k + 3] == '\t')))) {
        i = k + 3;
        t[j++] = ' ';
        continue;
      }
    }
    /* Markup is not wording, and an element name left in splits the sentence
       it sits inside: docbook writes "<citetitle>GNU Free Documentation
       License</citetitle>, Version 1.3". Only an element name is taken, so a
       "<name of author>" placeholder stays as words. */
    if (c == '<') {
      size_t k = i + 1;
      if (in[k] == '/') k++;
      if (isalpha((unsigned char) in[k])) {
        size_t q = k;
        while (isalnum((unsigned char) in[q])) q++;
        /* A tag carrying an attribute -- <span class="comment"> on every
           line of a documentation rendering -- leaves "span class comment"
           beside each line's words. An attribute is told from a placeholder
           like "<name of author>" by its "=", so the placeholder stays. */
        if (in[q] == ' ' || in[q] == '\t' || in[q] == '\n' || in[q] == '\r'
            || in[q] == '\f' || in[q] == '\v') {
          size_t e = q;
          while (in[e] && in[e] != '<' && in[e] != '>') e++;
          if (in[e] == '>' && memchr(in + q, '=', e - q)) { i = e; t[j++] = ' '; continue; }
        }
        while (in[q] == ' ' || in[q] == '\t') q++;
        if (in[q] == '/') q++;
        if (in[q] == '>') { i = q; t[j++] = ' '; continue; }
      }
    }
    if (!strncasecmp(in + i, "http://", 7))  { i += 6; t[j++] = ' '; continue; }
    if (!strncasecmp(in + i, "https://", 8)) { i += 7; t[j++] = ' '; continue; }
    /* A notice carried inside a string -- a source map's sourcesContent, a
       bundle's embedded header -- writes its line breaks as the two
       characters "\n", which read as a letter and split "found in the
       LICENSE file" into "the n license file". The escape is whitespace. */
    if (c == '\\' && (in[i + 1] == 'n' || in[i + 1] == 't' || in[i + 1] == 'r')) {
      i++; t[j++] = ' '; continue;
    }
    /* A manual page writes its notice through troff: "the \s-1GNU\s0 Free
       Documentation License" for a smaller GNU, \fBbold\fR, \*(L" for a
       quote, \- for a hyphen. The escapes are typesetting, not wording. */
    if (c == '\\' && in[i + 1]) {
      unsigned char e = (unsigned char) in[i + 1];
      size_t k = 0;
      if (e == 's') {
        size_t q = i + 2;
        if (in[q] == '+' || in[q] == '-') q++;
        if (isdigit((unsigned char) in[q])) { while (isdigit((unsigned char) in[q])) q++; k = q - i; }
      } else if (e == 'f') {
        if (in[i + 2] == '(' && isalnum((unsigned char) in[i + 3]) && isalnum((unsigned char) in[i + 4])) k = 5;
        else if (in[i + 2] == 'B' || in[i + 2] == 'I' || in[i + 2] == 'R' || in[i + 2] == 'P') k = 3;
      } else if (e == '*') {
        if (in[i + 2] == '(' && in[i + 3] && in[i + 4]) k = 5;
        else if (isalpha((unsigned char) in[i + 2])) k = 3;
      } else if (e == '-' || e == '&') {
        k = 2;
      } else if (e == 'e' && !isalnum((unsigned char) in[i + 2])) {
        k = 2;
      }
      if (k) { i += k - 1; t[j++] = ' '; continue; }
    }
    if (c == '+') { memcpy(t + j, " plus ", 6); j += 6; continue; }
    if (isalnum(c)) t[j++] = (char) tolower(c);
    else t[j++] = ' ';
  }
  t[j] = '\0';
  /* collapse runs of spaces, and rewrite a bare v/ver/version before a digit */
  ocap = j + 16;
  o = malloc(ocap);
  if (!o) { free(t); return NULL; }
  {
    size_t k = 0, grown = 0; int sp = 1;
    char *p = t;
    while (*p) {
      if (*p == ' ') { if (!sp) { o[k++] = ' '; sp = 1; } p++; continue; }
      sp = 0;
      if ((p == t || p[-1] == ' ')) {
        size_t w = 0;
        while (p[w] && p[w] != ' ') w++;
        if ((w == 1 && p[0] == 'v') || (w == 3 && !strncmp(p, "ver", 3)) ||
            (w == 7 && !strncmp(p, "version", 7))) {
          size_t q = w; while (p[q] == ' ') q++;
          if (isdigit((unsigned char) p[q])) {
            /* The one rewrite that writes more than it reads, so the
               output buffer has to grow. */
            size_t need = j + 8 * ++grown + 1;
            if (need > ocap) {
              char *no;
              ocap = need > ocap * 2 ? need : ocap * 2;
              no = thGrow(o, ocap);
              if (!no) { free(t); return NULL; }
              o = no;
            }
            memcpy(o + k, "version ", 8); k += 8; p += q; sp = 0; continue;
          }
        }
      }
      o[k++] = *p++;
    }
    while (k && o[k - 1] == ' ') k--;
    o[k] = '\0';
  }
  free(t);
  return o;
}

/* ---------- where the time goes (THESMO_PROFILE=1) ---------- */

#include <time.h>

static struct { double normalise, gateDense, gateHead, licVec, licScore, fullDense, evidence, rules,
                gaz, grantRe; long windows; int on; } thProf;

static double thNow(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

void thProfileReport(FILE *to)
{
  double total = thProf.normalise + thProf.gateDense + thProf.gateHead + thProf.licVec
      + thProf.licScore + thProf.fullDense + thProf.evidence + thProf.rules;
  if (!thProf.on || total <= 0.0) return;
  fprintf(to, "thesmo profile: %ld windows, %.2fs in the phases below\n", thProf.windows, total);
#define TH_LINE(name, v) fprintf(to, "  %-12s %6.2fs %5.1f%%\n", name, v, 100.0 * (v) / total)
  TH_LINE("normalise", thProf.normalise);
  TH_LINE("gate-dense", thProf.gateDense);
  TH_LINE("gate-head", thProf.gateHead);
  TH_LINE("lic-vectors", thProf.licVec);
  TH_LINE("lic-score", thProf.licScore);
  TH_LINE("full-dense", thProf.fullDense);
  TH_LINE("evidence", thProf.evidence);
  TH_LINE("rules", thProf.rules);
  fprintf(to, "  of gate-dense: gazetteer %.2fs, grant patterns %.2fs\n", thProf.gaz, thProf.grantRe);
#undef TH_LINE
}

/* ---------- the trail a finding's method carries ---------- */

/**
 * \brief The method with a rule appended: "ml+container", "verbatim+contained".
 *
 * A method reads as a path, so a report says not only which stage produced
 * a finding but which rule last changed it, and why an answer differs from
 * the head's. The strings are interned, so a result copied by value keeps a
 * pointer that outlives the copy; the table is freed with the model.
 */
static char *thTrail[256];
static int thTrails = 0;

static const char *thWordsAt(const char *hay, const char *needle);

/**
 * \brief Whether the words that grant later versions are here: "or (at
 *        your option) any later version", "GPLv2+" (normalised to "plus"),
 *        "version 2 or above". Without one, a version named is that version.
 */
static int thLaterWording(const char *nt)
{
  /* Read generously, since it is absence that decides. "plus" and the
     comparatives are ordinary words -- "direct plus indirect" -- so those
     count only where a version runs into them, which is how a notice
     writes them: "GPLv2+" normalises to "gplv2 plus". */
  static const char *plain[] = {"later", "subsequent", NULL};
  static const char *after[] = {"plus", "or above", "or higher", "or greater",
                                "or newer", NULL};
  /* a comparative that names what it compares grants later versions
     wherever it stands: "version 2, or any higher version" */
  static const char *named[] = {"or any above version", "or any higher version",
                                "or any greater version", "or any newer version",
                                "or any above versions", "or any higher versions",
                                "or any greater versions", "or any newer versions",
                                NULL};
  int k;
  const char *p;
  for (k = 0; plain[k]; k++)
    if (thWordsAt(nt, plain[k])) return 1;
  for (k = 0; named[k]; k++)
    if (thWordsAt(nt, named[k])) return 1;
  for (k = 0; after[k]; k++)
    for (p = nt; (p = thWordsAt(p, after[k])) != NULL; p++) {
      const char *q = p;
      if (q == nt) continue;
      q--;                                   /* the space before the word */
      while (q > nt && *q == ' ') q--;
      if (*q >= '0' && *q <= '9') return 1;
    }
  return 0;
}

static const char *thMethodWith(const char *base, const char *rule)
{
  char buf[128];
  int i;
  snprintf(buf, sizeof(buf), "%s+%s", base ? base : "ml", rule);
  for (i = 0; i < thTrails; i++)
    if (strcmp(thTrail[i], buf) == 0) return thTrail[i];
  if (thTrails == (int) (sizeof(thTrail) / sizeof(thTrail[0]))) return base;
  thTrail[thTrails] = strdup(buf);
  return thTrail[thTrails] ? thTrail[thTrails++] : base;
}

static void thTrailsFree(void)
{
  while (thTrails > 0) free(thTrail[--thTrails]);
}

/* ---------- feature extraction ---------- */

typedef struct { int *idx; float *val; int n, cap; } thSparse;

static void thPush(thSparse *s, int i, float v)
{
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->idx = thGrow(s->idx, (size_t) s->cap * sizeof(int));
    s->val = thGrow(s->val, (size_t) s->cap * sizeof(float));
  }
  s->idx[s->n] = i; s->val[s->n] = v; s->n++;
}

/** accumulate a term count into a per-block dense scratch buffer */
static void thCount(float *acc, thBlock *b, const char *term, size_t len)
{
  int id = thHashGet(b->vocab, term, len);
  if (id < 0) return;
  if (acc[id] == 0.0f) {
    if (b->nTouched == b->touchedCap) {
      b->touchedCap = b->touchedCap ? b->touchedCap * 2 : 1024;
      b->touched = thGrow(b->touched, (size_t) b->touchedCap * sizeof(int));
    }
    b->touched[b->nTouched++] = id;
  }
  acc[id] += 1.0f;
}

/**
 * \brief Ascending term ids, by radix: a window's few hundred ids sort in a
 *        pass or two over 256 buckets, where a comparison sort spends most of
 *        its time calling the comparison.
 */
static void thSortIds(int *a, int *tmp, int n)
{
  int shift, hi = 0, i;
  for (i = 0; i < n; i++) if (a[i] > hi) hi = a[i];
  for (shift = 0; (hi >> shift) > 0; shift += 8) {
    int count[257] = {0};
    for (i = 0; i < n; i++) count[((a[i] >> shift) & 255) + 1]++;
    for (i = 0; i < 256; i++) count[i + 1] += count[i];
    for (i = 0; i < n; i++) tmp[count[(a[i] >> shift) & 255]++] = a[i];
    memcpy(a, tmp, (size_t) n * sizeof(int));
  }
}

/** \brief 1 + log(k) for the small counts a term has in a window. */
static double thSublinear(float count)
{
  static double tab[64];
  static int ready = 0;
  int k = (int) count;
  if (!ready) {
    int i;
    for (i = 1; i < 64; i++) tab[i] = 1.0 + log((double) i);
    ready = 1;
  }
  if (k > 0 && k < 64 && (float) k == count) return tab[k];
  return 1.0 + log((double) count);
}

/** \brief The term counts one padded word contributes, remembered. */
#define TH_WC_SLOTS 8192
#define TH_WC_WORD 64
typedef struct { char word[TH_WC_WORD]; int n; int *id; unsigned short *cnt; } thWcEntry;
struct thWordCache { thWcEntry slot[TH_WC_SLOTS]; };

typedef void (*thGramFn)(void *ctx, thBlock *b, const char *g, size_t len);

/** \brief the char n-grams of one padded word, in sklearn's order */
static void thCharGrams(thBlock *b, const char *w, size_t wl, thGramFn emit, void *ctx)
{
  int n;
  for (n = b->lo; n <= b->hi; n++) {
    size_t off = 0;
    /* sklearn emits the whole padded word once and stops as soon as the
       n-gram is no shorter than the word itself */
    if ((size_t) n >= wl) { emit(ctx, b, w, wl); break; }
    emit(ctx, b, w, (size_t) n);
    while (off + (size_t) n < wl) { off++; emit(ctx, b, w + off, (size_t) n); }
  }
}

static void thCountGram(void *acc, thBlock *b, const char *g, size_t len)
{
  thCount(acc, b, g, len);
}

/** the distinct terms of one word and how often each occurs in it */
typedef struct { int n, id[TH_WC_WORD * 3]; unsigned short cnt[TH_WC_WORD * 3]; } thWcCounts;

static void thCollectGram(void *ctx, thBlock *b, const char *g, size_t len)
{
  thWcCounts *c = ctx;
  int id = thHashGet(b->vocab, g, len), i;
  if (id < 0) return;
  for (i = 0; i < c->n; i++)
    if (c->id[i] == id) { c->cnt[i]++; return; }
  if (c->n < (int) (sizeof(c->id) / sizeof(c->id[0]))) { c->id[c->n] = id; c->cnt[c->n++] = 1; }
}

/** \brief replay a word's counts: what thCount would have left in acc */
static void thReplay(float *acc, thBlock *b, const thWcEntry *e)
{
  int i;
  for (i = 0; i < e->n; i++) {
    int id = e->id[i];
    if (acc[id] == 0.0f) {
      if (b->nTouched == b->touchedCap) {
        b->touchedCap = b->touchedCap ? b->touchedCap * 2 : 1024;
        b->touched = thGrow(b->touched, (size_t) b->touchedCap * sizeof(int));
      }
      b->touched[b->nTouched++] = id;
    }
    acc[id] += (float) e->cnt[i];
  }
}

/**
 * \brief Cut a word's n-grams once, and keep them under the word.
 *
 * A slot holds one word; a newer word takes it over.
 */
static const thWcEntry *thWcRemember(thBlock *b, const char *w, size_t wl)
{
  thWcEntry *e;
  thWcCounts c;
  if (!b->words) b->words = calloc(1, sizeof(*b->words));
  if (!b->words) return NULL;
  c.n = 0;
  thCharGrams(b, w, wl, thCollectGram, &c);
  e = b->words->slot + thHashStr(w, TH_WC_SLOTS);
  free(e->id); free(e->cnt);
  e->id = malloc((size_t) (c.n ? c.n : 1) * sizeof(int));
  e->cnt = malloc((size_t) (c.n ? c.n : 1) * sizeof(unsigned short));
  if (!e->id || !e->cnt) {
    free(e->id); free(e->cnt);
    e->id = NULL; e->cnt = NULL; e->n = 0; e->word[0] = '\0';
    return NULL;
  }
  memcpy(e->id, c.id, (size_t) c.n * sizeof(int));
  memcpy(e->cnt, c.cnt, (size_t) c.n * sizeof(unsigned short));
  e->n = c.n;
  memcpy(e->word, w, wl + 1);
  return e;
}

/**
 * \brief sklearn char_wb: pad each word with spaces, take n-grams inside it.
 *
 * A word shorter than n yields the padded word once, as sklearn does.
 */
static void thCharWb(float *acc, thBlock *b, const char *t)
{
  const char *p = t;
  char w[1024];
  while (*p) {
    size_t wl = 0;
    while (*p == ' ') p++;
    if (!*p) break;
    w[wl++] = ' ';
    while (*p && *p != ' ' && wl < sizeof(w) - 2) w[wl++] = *p++;
    while (*p && *p != ' ') p++; /* overlong word: skip the tail */
    w[wl++] = ' '; w[wl] = '\0';
    if (wl < TH_WC_WORD) {
      const thWcEntry *e = b->words ? b->words->slot + thHashStr(w, TH_WC_SLOTS) : NULL;
      if (!e || strcmp(e->word, w) != 0) e = thWcRemember(b, w, wl);
      if (e) { thReplay(acc, b, e); continue; }
    }
    thCharGrams(b, w, wl, thCountGram, acc);
  }
}

/** sklearn word analyzer with the default token pattern: 2+ alnum characters */
static void thWord(float *acc, thBlock *b, const char *t)
{
  const char *tok[8192];
  int len[8192], nt = 0;
  const char *p = t;
  while (*p && nt < 8192) {
    const char *s;
    while (*p == ' ') p++;
    if (!*p) break;
    s = p;
    while (*p && *p != ' ') p++;
    if (p - s >= 2) { tok[nt] = s; len[nt] = (int) (p - s); nt++; }
  }
  {
    int n, i;
    char buf[1024];
    for (n = b->lo; n <= b->hi; n++) {
      for (i = 0; i + n <= nt; i++) {
        int k, j = 0;
        for (k = 0; k < n; k++) {
          if (k) buf[j++] = ' ';
          if (j + len[i + k] >= (int) sizeof(buf) - 1) { j = -1; break; }
          memcpy(buf + j, tok[i + k], (size_t) len[i + k]); j += len[i + k];
        }
        if (j < 0) continue;
        buf[j] = '\0';
        thCount(acc, b, buf, (size_t) j);
      }
    }
  }
}

/** build the concatenated sparse vector: sublinear tf, idf, L2 per block */
static int thVectorise(thBlock *blocks, int nb, const char *t, thSparse *out)
{
  int k;
  out->n = 0;
  for (k = 0; k < nb; k++) {
    thBlock *b = blocks + k;
    float *acc;
    double nrm = 0.0;
    int i, start = out->n;
    if (!b->acc) b->acc = calloc((size_t) b->n, sizeof(float));
    acc = b->acc;
    if (!acc) return -1;
    b->nTouched = 0;
    if (b->isChar) thCharWb(acc, b, t); else thWord(acc, b, t);
    /* in ascending column order, as a walk over the vocabulary would give */
    if (b->sortCap < b->touchedCap) {
      b->sortCap = b->touchedCap;
      b->sortTmp = thGrow(b->sortTmp, (size_t) b->sortCap * sizeof(int));
      if (!b->sortTmp) return -1;
    }
    thSortIds(b->touched, b->sortTmp, b->nTouched);
    for (i = 0; i < b->nTouched; i++) {
      int id = b->touched[i];
      float v = (float) (thSublinear(acc[id]) * (double) b->idf[id]);
      thPush(out, (int) b->offset + id, v);
      nrm += (double) v * (double) v;
      acc[id] = 0.0f;
    }
    if (nrm > 0.0) {
      float inv = (float) (1.0 / sqrt(nrm));
      for (i = start; i < out->n; i++) out->val[i] *= inv;
    }
  }
  return 0;
}

/* ---------- dense grant/mention features ---------- */

/**
 * \brief Dense features, or only the two that decide whether to go on.
 *
 * A window that names no license and grants none is rejected on f[2] and
 * f[6] alone, and that is most of them: over a source tree, six windows in
 * seven never get past it. Counting the distinct gazetteer hits needs a hash
 * per window and the mention patterns are a regex apiece, so with `gateOnly`
 * neither is done until the window has earned it. The gazetteer pass is then
 * repeated for the few that survive, which is cheaper than paying for the
 * hash on all of them.
 *
 * \return 1 when every feature is filled, 0 when only f[2] and f[6] are --
 *         which happens only where the caller would have stopped anyway.
 */
static int thRegexec(const regex_t *re, const char *lit, int mixed, const char *s,
    size_t nmatch, regmatch_t *pmatch);
static int thClausesHere(thModel *m, const char *nt, char *here);
static int thClauseAllows(thModel *m, int ci, const char *text, const char *nt,
    const char *here, int n);
static int thBeyondSignature(thModel *m, int ci, const char *text, const char *nt,
    const char *here, int n);

static int thDenseEx(thModel *m, const char *t, float *f, int gateOnly)
{
  int nTok = 0, gaz = 0, distinct = 0, grant = 0, mention = 0, i;
  const char *tok[8192]; int len[8192];
  const char *p = t;
  struct thHash *seen;
  while (*p && nTok < 8192) {
    const char *s;
    while (*p == ' ') p++;
    if (!*p) break;
    s = p;
    while (*p && *p != ' ') p++;
    tok[nTok] = s; len[nTok] = (int) (p - s); nTok++;
  }
  if (gateOnly) {
    double p0 = thProf.on ? thNow() : 0.0;
    for (i = 0; i < nTok; i++) {
      char buf[512];
      int L, j = 0;
      for (L = 1; L <= m->gazMax && i + L <= nTok; L++) {
        if (L > 1) buf[j++] = ' ';
        if (j + len[i + L - 1] >= (int) sizeof(buf) - 1) break;
        memcpy(buf + j, tok[i + L - 1], (size_t) len[i + L - 1]); j += len[i + L - 1];
        buf[j] = '\0';
        if (thHashGet(m->gazPrefix, buf, (size_t) j) < 0) break;
        if (thHashGet(m->gaz, buf, (size_t) j) >= 0) gaz++;
      }
    }
    if (thProf.on) { double p1 = thNow(); thProf.gaz += p1 - p0; p0 = p1; }
    for (i = 0; i < m->nGrant; i++)
      if (thRegexec(((regex_t *) m->grantRe) + i, m->grantLit[i], 0, t, 0, NULL) == 0) grant++;
    if (thProf.on) thProf.grantRe += thNow() - p0;
    if (gaz == 0 && grant == 0) { f[2] = 0.0f; f[6] = 0.0f; return 0; }
    gaz = grant = 0;              /* recounted below, with the rest */
  }
  seen = thHashNew(64);
  for (i = 0; i < nTok; i++) {
    char buf[512];
    int L, j = 0;
    for (L = 1; L <= m->gazMax && i + L <= nTok; L++) {
      if (L > 1) buf[j++] = ' ';
      if (j + len[i + L - 1] >= (int) sizeof(buf) - 1) break;
      memcpy(buf + j, tok[i + L - 1], (size_t) len[i + L - 1]); j += len[i + L - 1];
      buf[j] = '\0';
      if (thHashGet(m->gazPrefix, buf, (size_t) j) < 0) break;
      if (thHashGet(m->gaz, buf, (size_t) j) >= 0) {
        gaz++;
        if (seen && thHashGet(seen, buf, (size_t) j) < 0) { thHashPut(seen, buf, 1); distinct++; }
      }
    }
  }
  thHashFree(seen);
  for (i = 0; i < m->nGrant; i++)
    if (thRegexec(((regex_t *) m->grantRe) + i, m->grantLit[i], 0, t, 0, NULL) == 0) grant++;
  for (i = 0; i < m->nMention; i++)
    if (thRegexec(((regex_t *) m->mentionRe) + i, m->mentionLit[i], 0, t, 0, NULL) == 0) mention++;
  {
    double n = nTok ? (double) nTok : 1.0;
    f[0]  = (float) nTok;
    f[1]  = (float) log1p((double) nTok);
    f[2]  = (float) gaz;
    f[3]  = (float) (gaz / n);
    f[4]  = (float) distinct;
    f[5]  = (float) (distinct / n);
    f[6]  = (float) grant;
    f[7]  = (float) (grant / n * 10.0);
    f[8]  = (float) mention;
    f[9]  = (float) (mention / n * 10.0);
    f[10] = grant > 0 ? 1.0f : 0.0f;
    f[11] = mention > 0 ? 1.0f : 0.0f;
    f[12] = (distinct >= 3 && grant == 0) ? 1.0f : 0.0f;
    f[13] = nTok <= 8 ? 1.0f : 0.0f;
  }
  return 1;
}

static void thDense(thModel *m, const char *t, float *f)
{
  thDenseEx(m, t, f, 0);
}

/* ---------- linear scoring ---------- */

static void thScore(thLinear *L, thSparse *x, const float *dense, int nDense,
    float *out)
{
  int r, i;
  for (r = 0; r < L->rows; r++) {
    double s = (double) L->bias[r];
    if (L->dense) {
      const float *w = L->dense + (size_t) r * (size_t) L->cols;
      for (i = 0; i < x->n; i++) s += (double) w[x->idx[i]] * (double) x->val[i];
      if (dense)
        for (i = 0; i < nDense; i++)
          s += (double) w[L->cols - nDense + i] * (double) dense[i];
    } else if (!L->colPtr) {
      /* sparse row: both sides are sorted by column, so walk them together */
      int a = 0, b = 0;
      while (a < L->nnz[r] && b < x->n) {
        if (L->idx[r][a] == x->idx[b]) { s += (double) L->val[r][a] * (double) x->val[b]; a++; b++; }
        else if (L->idx[r][a] < x->idx[b]) a++;
        else b++;
      }
    } else {
      break;  /* by column, below */
    }
    out[r] = (float) s;
  }
  if (L->colPtr) {
    int b, k;
    for (r = 0; r < L->rows; r++) L->acc[r] = (double) L->bias[r];
    for (b = 0; b < x->n; b++) {
      int c = x->idx[b];
      double v = (double) x->val[b];
      if (c < 0 || c >= L->cols) continue;
      for (k = L->colPtr[c]; k < L->colPtr[c + 1]; k++)
        L->acc[L->colRow[k]] += (double) L->colVal[k] * v;
    }
    for (r = 0; r < L->rows; r++) out[r] = (float) L->acc[r];
  }
}

/**
 * \brief Index a sparse head by column, and drop the rows.
 *
 * Within a column the rows are in ascending order, which does not matter
 * for the sums; across columns the order is ascending, which does.
 */
static int thIndexByColumn(thLinear *L)
{
  size_t total = 0, k;
  int r, c, *fill;
  for (r = 0; r < L->rows; r++) total += (size_t) L->nnz[r];
  L->colPtr = calloc((size_t) L->cols + 1, sizeof(int));
  L->colRow = malloc(total * sizeof(int));
  L->colVal = malloc(total * sizeof(float));
  L->acc = malloc((size_t) L->rows * sizeof(double));
  fill = calloc((size_t) L->cols, sizeof(int));
  if (!L->colPtr || !L->colRow || !L->colVal || !L->acc || !fill) { free(fill); return -1; }
  for (r = 0; r < L->rows; r++)
    for (k = 0; k < (size_t) L->nnz[r]; k++) L->colPtr[L->idx[r][k] + 1]++;
  for (c = 0; c < L->cols; c++) L->colPtr[c + 1] += L->colPtr[c];
  for (r = 0; r < L->rows; r++)
    for (k = 0; k < (size_t) L->nnz[r]; k++) {
      int c2 = L->idx[r][k], at = L->colPtr[c2] + fill[c2]++;
      L->colRow[at] = r;
      L->colVal[at] = L->val[r][k];
    }
  free(fill);
  for (r = 0; r < L->rows; r++) { free(L->idx[r]); free(L->val[r]); }
  free(L->idx); free(L->val); free(L->nnz);
  L->idx = NULL; L->val = NULL; L->nnz = NULL;
  return 0;
}

/* ---------- model loading ---------- */

/** read one "term\tidf" vocabulary; index is the line number */
static int thLoadVocab(thBlock *b, const char *path)
{
  FILE *f = fopen(path, "r");
  char *line = NULL;
  size_t cap = 0;
  int i = 0;
  ssize_t got;
  if (!f) return -1;
  b->vocab = thHashNew((size_t) (b->n ? b->n : 1024));
  b->idf = calloc((size_t) (b->n ? b->n : 1), sizeof(float));
  if (!b->vocab || !b->idf) { fclose(f); return -1; }
  while ((got = getline(&line, &cap, f)) > 0) {
    char *tab;
    if (line[got - 1] == '\n') line[--got] = '\0';
    tab = strrchr(line, '\t');
    if (!tab || i >= b->n) continue;
    *tab = '\0';
    thHashPut(b->vocab, line, i);
    b->idf[i] = (float) atof(tab + 1);
    i++;
  }
  free(line); fclose(f);
  return i == b->n ? 0 : -1;
}

static int thLoadDense(thLinear *L, const char *path)
{
  FILE *f = fopen(path, "rb");
  int hdr[2];
  if (!f) return -1;
  if (fread(hdr, sizeof(int), 2, f) != 2) { fclose(f); return -1; }
  L->rows = hdr[0]; L->cols = hdr[1]; L->nnz = NULL; L->idx = NULL; L->val = NULL;
  L->bias = malloc((size_t) L->rows * sizeof(float));
  L->dense = malloc((size_t) L->rows * (size_t) L->cols * sizeof(float));
  if (!L->bias || !L->dense) { fclose(f); return -1; }
  if (fread(L->bias, sizeof(float), (size_t) L->rows, f) != (size_t) L->rows ||
      fread(L->dense, sizeof(float), (size_t) L->rows * (size_t) L->cols, f)
        != (size_t) L->rows * (size_t) L->cols) { fclose(f); return -1; }
  fclose(f);
  return 0;
}

static int thLoadSparse(thLinear *L, const char *path)
{
  FILE *f = fopen(path, "rb");
  int hdr[2], r;
  if (!f) return -1;
  if (fread(hdr, sizeof(int), 2, f) != 2) { fclose(f); return -1; }
  L->rows = hdr[0]; L->cols = hdr[1]; L->dense = NULL;
  L->bias = malloc((size_t) L->rows * sizeof(float));
  L->idx = calloc((size_t) L->rows, sizeof(int *));
  L->val = calloc((size_t) L->rows, sizeof(float *));
  L->nnz = calloc((size_t) L->rows, sizeof(int));
  if (!L->bias || !L->idx || !L->val || !L->nnz) { fclose(f); return -1; }
  if (fread(L->bias, sizeof(float), (size_t) L->rows, f) != (size_t) L->rows) { fclose(f); return -1; }
  for (r = 0; r < L->rows; r++) {
    int n;
    if (fread(&n, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    L->nnz[r] = n;
    L->idx[r] = malloc((size_t) n * sizeof(int));
    L->val[r] = malloc((size_t) n * sizeof(float));
    if (!L->idx[r] || !L->val[r]) { fclose(f); return -1; }
    if (fread(L->idx[r], sizeof(int), (size_t) n, f) != (size_t) n ||
        fread(L->val[r], sizeof(float), (size_t) n, f) != (size_t) n) { fclose(f); return -1; }
  }
  fclose(f);
  return thIndexByColumn(L);
}

/* minimal JSON readers: the manifest is generated by our own exporter */
static char *thSlurp(const char *p)
{
  FILE *f = fopen(p, "rb");
  char *b; long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  b = malloc((size_t) n + 1);
  if (!b || fread(b, 1, (size_t) n, f) != (size_t) n) { free(b); fclose(f); return NULL; }
  b[n] = '\0'; fclose(f);
  return b;
}

static double thJnum(const char *j, const char *key, double dflt)
{
  char pat[64];
  const char *p;
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  p = strstr(j, pat);
  if (!p) return dflt;
  p = strchr(p + strlen(pat), ':');
  return p ? atof(p + 1) : dflt;
}

/** pull the n-th string array element; returns malloc'd copy or NULL */
static char *thJstrAt(const char **cur)
{
  const char *p = *cur, *s;
  char *o; size_t k = 0;
  while (*p && *p != '"') { if (*p == ']') return NULL; p++; }
  if (!*p) return NULL;
  s = ++p;
  while (*p && *p != '"') { if (*p == '\\') p++; p++; }
  o = malloc((size_t) (p - s) + 1);
  if (!o) return NULL;
  while (s < p) { if (*s == '\\') { s++; if (*s == 'n') { o[k++] = '\n'; s++; continue; } } o[k++] = *s++; }
  o[k] = '\0';
  *cur = p + 1;
  return o;
}

/**
 * \brief Read `"renderings": {"class": [[phrase, ...], ...]}` into `into`.
 *
 * A licence written more than one way keeps each way as its own phrase list,
 * so a file can be scored on the nearest rather than on their union.
 */
static void thRenderSection(thModel *m, const char *json, const char *key,
                            char ****into)
{
  const char *p = strstr(json, key);
  if (!p || (p = strchr(p, '{')) == NULL) return;
  p++;
  for (;;) {
    char *cname = thJstrAt(&p);
    const char *arr;
    int ci, rcap = 0, rcnt = 0;
    char ***sets = NULL;
    if (!cname) break;
    for (ci = 0; ci < m->nClasses; ci++)
      if (strcmp(m->classes[ci], cname) == 0) break;
    free(cname);
    arr = strchr(p, '[');
    if (!arr) break;
    p = arr + 1;
    for (;;) {
      int cap = 0, cnt = 0;
      char **list = NULL;
      while (*p == ' ' || *p == '\n' || *p == ',') p++;
      if (*p == ']') { p++; break; }
      if (*p != '[') break;
      p++;
      for (;;) {
        const char *save = p;
        char *ph;
        while (*p == ' ' || *p == '\n' || *p == ',') p++;
        if (*p == ']') { p++; break; }
        p = save;
        ph = thJstrAt(&p);
        if (!ph) break;
        if (cnt == cap) { cap = cap ? cap * 2 : 32;
          list = thGrow(list, (size_t) (cap + 1) * sizeof(char *)); }
        if (list) list[cnt++] = ph; else free(ph);
      }
      if (!list) continue;
      list[cnt] = NULL;
      if (rcnt == rcap) { rcap = rcap ? rcap * 2 : 4;
        sets = thGrow(sets, (size_t) (rcap + 1) * sizeof(char **)); }
      if (sets) sets[rcnt++] = list;
      else { int q; for (q = 0; q < cnt; q++) free(list[q]); free(list); }
    }
    if (sets) { sets[rcnt] = NULL;
      if (ci < m->nClasses) into[ci] = sets;
      else { int a, b;
        for (a = 0; a < rcnt; a++) {
          for (b = 0; sets[a][b]; b++) free(sets[a][b]);
          free(sets[a]);
        }
        free(sets); } }
    while (*p && *p != '"' && *p != '}') p++;
    if (*p == '}') break;
  }
}

/**
 * \brief Read one `"section": {"class": [phrase, ...]}` map into `into`.
 *
 * An unknown class is parsed and discarded, not skipped, so the scan stays in
 * step.
 */
static void thPhraseSection(thModel *m, const char *json, const char *section,
    char ***into)
{
  const char *p = strstr(json, section);
  if (!p || (p = strchr(p, '{')) == NULL) return;
  p++;
  for (;;) {
    char *cname = thJstrAt(&p);
    const char *arr;
    int ci, cap = 0, cnt = 0;
    char **list = NULL;
    if (!cname) break;
    for (ci = 0; ci < m->nClasses; ci++)
      if (strcmp(m->classes[ci], cname) == 0) break;
    free(cname);
    arr = strchr(p, '[');
    if (!arr) break;
    p = arr + 1;
    for (;;) {
      const char *save = p;
      char *ph;
      while (*p == ' ' || *p == '\n') p++;
      if (*p == ']') { p++; break; }
      p = save;
      ph = thJstrAt(&p);
      if (!ph) break;
      if (cnt == cap) { cap = cap ? cap * 2 : 8;
        list = thGrow(list, (size_t) (cap + 1) * sizeof(char *)); }
      if (list) list[cnt++] = ph; else free(ph);
    }
    if (list) { list[cnt] = NULL;
      if (ci < m->nClasses) into[ci] = list;
      else { int q; for (q = 0; q < cnt; q++) free(list[q]); free(list); } }
    while (*p == ',' || *p == ' ' || *p == '\n') p++;
    if (*p == '}') break;
  }
}


static int thLoadBlocks(const char *j, const char *key, const char *dir,
    const char *stem, thBlock **out, int *nOut)
{
  const char *p = strstr(j, key);
  thBlock *b;
  int n = 0, i;
  size_t off = 0;
  if (!p) return -1;
  { const char *q = p; while (*q && *q != ']') { if (!strncmp(q, "\"kind\"", 6)) n++; q++; } }
  b = calloc((size_t) n, sizeof(thBlock));
  if (!b) return -1;
  for (i = 0; i < n; i++) {
    const char *k = strstr(p, "\"kind\"");
    char path[1024];
    const char *tag;
    if (!k) { free(b); return -1; }
    b[i].isChar = strstr(k, "char_wb") && (strstr(k, "char_wb") < strchr(k, ','));
    b[i].lo = (int) thJnum(k, "lo", 1);
    b[i].hi = (int) thJnum(k, "hi", 1);
    b[i].n  = (int) thJnum(k, "n", 0);
    b[i].offset = off; off += (size_t) b[i].n;
    tag = b[i].isChar ? "c" : "w";
    snprintf(path, sizeof(path), "%s/%s_%s.vocab", dir, stem, tag);
    if (thLoadVocab(b + i, path) != 0) { free(b); return -1; }
    p = k + 6;
  }
  *out = b; *nOut = n;
  return 0;
}

/** \brief Past any quantifier: the atom before it is optional or repeated. */
static const char *thSkipQuant(const char *p)
{
  for (;;) {
    if (*p == '*' || *p == '+' || *p == '?') p++;
    else if (*p == '{') { while (*p && *p != '}') p++; if (*p) p++; }
    else return p;
  }
}

/** \brief Past a bracket expression, on its opening bracket. */
static const char *thSkipClass(const char *p)
{
  p++;
  if (*p == '^') p++;
  if (*p == ']') p++;
  while (*p && *p != ']') {
    if (*p == '[' && (p[1] == ':' || p[1] == '.' || p[1] == '=')) {
      char d = p[1];
      p += 2;
      while (*p && !(*p == d && p[1] == ']')) p++;
      if (*p) p += 2;
    } else p++;
  }
  return *p ? p + 1 : p;
}

/**
 * \brief The longest run of plain characters a pattern cannot match without.
 *
 * A pattern is run only over text that carries the run, which a substring
 * search settles far faster than the matcher. Read conservatively: a group,
 * a bracket expression, an escape and a quantified character each end the
 * run without joining it, and an alternation at the top level leaves nothing
 * required. Lowercased where the pattern is compiled case-insensitive.
 * \return the run, or NULL where none is long enough to be worth asking
 */
static char *thRequiredLiteral(const char *pat, int icase)
{
  char run[256], best[256];
  size_t n = 0, bn = 0;
  const char *p = pat;
#define TH_END_RUN do { if (n > bn) { memcpy(best, run, n); bn = n; } n = 0; } while (0)
  while (*p) {
    char c = *p;
    if (c == '|') return NULL;
    if (c == '\\') {
      p++;
      if (!*p) break;
      if (strchr("bBwWsSdD<>`'nt", *p) || (*p >= '0' && *p <= '9')) {
        TH_END_RUN; p++; continue;
      }
      c = *p;
    } else if (c == '[') {
      TH_END_RUN; p = thSkipQuant(thSkipClass(p)); continue;
    } else if (c == '(') {
      int depth = 1;
      p++;
      while (*p && depth) {
        if (*p == '\\' && p[1]) p += 2;
        else if (*p == '[') p = thSkipClass(p);
        else { if (*p == '(') depth++; else if (*p == ')') depth--; p++; }
      }
      TH_END_RUN; p = thSkipQuant(p); continue;
    } else if (c == '.' || c == '^' || c == '$' || c == ')') {
      TH_END_RUN; p++; continue;
    } else if (c == '*' || c == '+' || c == '?' || c == '{') {
      TH_END_RUN; p = thSkipQuant(p); continue;
    }
    p++;
    if ((unsigned char) c > 127 || n >= sizeof(run)) { TH_END_RUN; p = thSkipQuant(p); continue; }
    if (*p == '?' || *p == '*' || *p == '{') { TH_END_RUN; p = thSkipQuant(p); continue; }
    run[n++] = icase ? (char) tolower((unsigned char) c) : c;
    if (*p == '+') { TH_END_RUN; p = thSkipQuant(p); }
  }
  TH_END_RUN;
#undef TH_END_RUN
  if (bn < 3) return NULL;
  { char *out = malloc(bn + 1);
    if (!out) return NULL;
    memcpy(out, best, bn); out[bn] = '\0';
    return out; }
}

/**
 * \brief regexec, asked only where the pattern's required run is present.
 *
 * \param lit the pattern's required run, or NULL to ask the matcher outright
 * \param mixed whether the text may carry upper case: normalised text is all
 *        lower case and the run of a case-insensitive pattern is lowered, so
 *        there the plain search is the case-insensitive one
 */
static int thRegexec(const regex_t *re, const char *lit, int mixed, const char *s,
    size_t nmatch, regmatch_t *pmatch)
{
  if (lit && !(mixed ? strcasestr(s, lit) : strstr(s, lit))) return REG_NOMATCH;
  return regexec(re, s, nmatch, pmatch, 0);
}

static int thLoadPatterns(const char *j, const char *key, void **out, char ***lit,
    int *n)
{
  const char *p = strstr(j, key);
  regex_t *re = NULL;
  char **l = NULL;
  int cap = 0, k = 0;
  if (!p) return -1;
  p = strchr(p, '[');
  if (!p) return -1;
  p++;
  for (;;) {
    char *s = thJstrAt(&p);
    if (!s) break;
    if (k == cap) {
      cap = cap ? cap * 2 : 32;
      re = thGrow(re, (size_t) cap * sizeof(regex_t));
      l = thGrow(l, (size_t) cap * sizeof(char *));
    }
    if (regcomp(re + k, s, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0) { free(s); continue; }
    l[k] = thRequiredLiteral(s, 1);
    k++; free(s);
  }
  *out = re; *lit = l; *n = k;
  return 0;
}

/**
 * \brief One list of {base, declared, phrase} rows from declared_variants.json.
 *
 * The keys are read positionally, in the order the table writes them.
 * \return how many rows were read, at most TH_DECLARED_MAX
 */
static int thLoadDeclared(const char *rj, const char *key, char **base, char **to,
    char **phr, int *ahead, char **unless)
{
  const char *p = strstr(rj, key);
  int n = 0;
  if (!p || (p = strchr(p, '[')) == NULL) return 0;
  p++;
  /* Read each row by its keys rather than their order: a row may carry more
     than the three that are always there, and a field read out of turn takes
     every row after it with it. */
  while (n < TH_DECLARED_MAX) {
    const char *end = strchr(p, '}');
    char *b = NULL, *t = NULL, *f = NULL, *un = NULL;
    int ah = 0;
    if (!end) break;
    while (p < end) {
      char *k = thJstrAt(&p), *v;
      if (!k || p > end) { free(k); break; }
      v = thJstrAt(&p);
      if (!v) { free(k); break; }
      if (strcmp(k, "base") == 0) { free(b); b = v; }
      else if (strcmp(k, "declared") == 0) { free(t); t = v; }
      else if (strcmp(k, "phrase") == 0) { free(f); f = v; }
      else if (strcmp(k, "unless") == 0) { free(un); un = v; }
      else {
        if (strcmp(k, "when") == 0 && strcmp(v, "before the text") == 0) ah = 1;
        free(v);
      }
      free(k);
    }
    if (!b || !t || !f) { free(b); free(t); free(f); free(un); break; }
    base[n] = b; to[n] = t; phr[n] = f;
    if (ahead) ahead[n] = ah;
    if (unless) unless[n] = un; else free(un);
    n++;
    p = end + 1;
    if (!strchr(p, '{')) break;
  }
  return n;
}

thModel *thLoad(const char *dir, char *err, size_t errLen)
{
  char path[1024], *j;
  thModel *m = calloc(1, sizeof(thModel));
  thProf.on = getenv("THESMO_PROFILE") != NULL;
  if (!m) return NULL;
  snprintf(path, sizeof(path), "%s/model.json", dir);
  j = thSlurp(path);
  if (!j) { snprintf(err, errLen, "cannot read %s", path); free(m); return NULL; }
  m->thGate = (float) thJnum(j, "gate", 0.4);
  m->thMargin = (float) thJnum(j, "margin", 1.1);
  m->thSuffix = (float) thJnum(j, "suffix", 0.5);
  m->thException = (float) thJnum(j, "exception_score", 0.0);
  m->minScore = (int) thJnum(j, "min_score", 0);
  m->thNameMin = (float) thJnum(j, "name_min", 1.2);
  m->thAloneMargin = (float) thJnum(j, "alone_margin", 2.0);
  m->thNamedMargin = (float) thJnum(j, "named_margin", 0.8);
  /* Grant density at or below this, on a text that states an exception, means
     the license beside it is named rather than granted. -1 disables. */
  m->thExcGrant = (float) thJnum(j, "exception_grant_max", 2.0);
  m->thBodyMin = (float) thJnum(j, "body_min", 20.0);
  m->thSubsumed = (float) thJnum(j, "subsumed", 0.75);
  m->thCandidates = (int) thJnum(j, "candidates", 1);
  if (m->thCandidates < 1) m->thCandidates = 1;
  if (m->thCandidates > TH_CAND_MAX) m->thCandidates = TH_CAND_MAX;
  /* margin bands with the precision measured for each, so the reported score
     is an estimate of how often a finding like this is right */
  { const char *c = strstr(j, "\"calibration\"");
    int cap = 0;
    m->nCal = 0;
    while (c && (c = strstr(c, "\"margin_lo\"")) != NULL) {
      const char *hi = strstr(c, "\"margin_hi\"");
      const char *pr = strstr(c, "\"precision\"");
      if (!hi || !pr) break;
      if (m->nCal == cap) {
        cap = cap ? cap * 2 : 8;
        m->calLo = thGrow(m->calLo, (size_t) cap * sizeof(float));
        m->calHi = thGrow(m->calHi, (size_t) cap * sizeof(float));
        m->calScore = thGrow(m->calScore, (size_t) cap * sizeof(int));
      }
      m->calLo[m->nCal] = (float) atof(strchr(c, ':') + 1);
      m->calHi[m->nCal] = (float) atof(strchr(hi, ':') + 1);
      m->calScore[m->nCal] = (int) (atof(strchr(pr, ':') + 1) * 100.0 + 0.5);
      m->nCal++;
      c = pr;
    } }
  { char *v = NULL; const char *p = strstr(j, "\"version\"");
    if (p) { p = strchr(p, ':'); if (p) { p++; v = thJstrAt(&p); } }
    snprintf(m->version, sizeof(m->version), "%s", v ? v : "0.0.0"); free(v); }
  if (thLoadBlocks(j, "\"gate_vec\"", dir, "gate", &m->gateBlk, &m->nGateBlk) ||
      thLoadBlocks(j, "\"lic_vec\"",  dir, "lic",  &m->licBlk,  &m->nLicBlk)  ||
      thLoadBlocks(j, "\"sfx_vec\"",  dir, "sfx",  &m->sfxBlk,  &m->nSfxBlk)) {
    snprintf(err, errLen, "vocabulary load failed"); free(j); thFree(m); return NULL;
  }
  snprintf(path, sizeof(path), "%s/gate.bin", dir);
  if (thLoadDense(&m->gate, path)) { snprintf(err, errLen, "gate.bin"); free(j); thFree(m); return NULL; }
  snprintf(path, sizeof(path), "%s/sfx.bin", dir);
  if (thLoadDense(&m->sfx, path)) { snprintf(err, errLen, "sfx.bin"); free(j); thFree(m); return NULL; }
  snprintf(path, sizeof(path), "%s/lic.bin", dir);
  if (thLoadSparse(&m->lic, path)) { snprintf(err, errLen, "lic.bin"); free(j); thFree(m); return NULL; }
  { const char *p = strstr(j, "\"scaler_mean\"");
    int i;
    if (p) { p = strchr(p, '['); p++; for (i = 0; i < TH_DENSE; i++) { m->scMean[i] = (float) atof(p); p = strchr(p, ','); if (!p) break; p++; } }
    p = strstr(j, "\"scaler_scale\"");
    if (p) { p = strchr(p, '['); p++; for (i = 0; i < TH_DENSE; i++) { m->scScale[i] = (float) atof(p); p = strchr(p, ','); if (!p) break; p++; } } }
  /* the verifier, if this model was fitted with one */
  { char vp[1024];
    snprintf(vp, sizeof(vp), "%s/verifier.json", dir);
    { char *vj = thSlurp(vp);
      if (vj) {
        const char *p = strstr(vj, "\"weights\"");
        int i;
        if (p && (p = strchr(p, '['))) {
          p++;
          for (i = 0; i < TH_EVID; i++) {
            m->verifW[i] = (float) atof(p);
            p = strchr(p, ',');
            if (!p) break;
            p++;
          }
          m->verifB = (float) thJnum(vj, "bias", 0.0);
          m->nIso = 0;
          p = strstr(vj, "\"isotonic\"");
          if (p) {
            const char *x = strstr(p, "\"x\"");
            const char *y = strstr(p, "\"y\"");
            if (x && y && (x = strchr(x, '[')) && (y = strchr(y, '['))) {
              x++; y++;
              for (i = 0; i < TH_ISO; i++) {
                m->isoX[i] = (float) atof(x);
                m->isoY[i] = (float) atof(y);
                x = strchr(x, ','); y = strchr(y, ',');
                m->nIso = i + 1;
                if (!x || !y) break;
                x++; y++;
              }
            }
          }
          m->haveVerifier = 1;
        }
        free(vj);
      } } }
  thLoadPatterns(j, "\"grant_patterns\"", &m->grantRe, &m->grantLit, &m->nGrant);
  thLoadPatterns(j, "\"mention_patterns\"", &m->mentionRe, &m->mentionLit, &m->nMention);
  { const char *p = strstr(j, "\"classes\"");
    int cap = 0;
    if (p) {
      p = strchr(p, '[');
      if (p) { p++;
        for (;;) {
          char *s = thJstrAt(&p);
          if (!s) break;
          if (m->nClasses == cap) { cap = cap ? cap * 2 : 256; m->classes = thGrow(m->classes, (size_t) cap * sizeof(char *)); }
          m->classes[m->nClasses++] = s;
        } } } }
  m->isException = calloc((size_t) (m->nClasses ? m->nClasses : 1), 1);
  { const char *e = strstr(j, "\"exception_classes\"");
    if (e && m->isException) {
      e = strchr(e, '[');
      if (e) { e++;
        for (;;) {
          char *sname = thJstrAt(&e);
          int q;
          if (!sname) break;
          for (q = 0; q < m->nClasses; q++)
            if (strcmp(m->classes[q], sname) == 0) { m->isException[q] = 1; break; }
          free(sname);
        } } } }
  free(j);
  if (m->nClasses != m->lic.rows) {
    snprintf(err, errLen, "class count %d != model rows %d", m->nClasses, m->lic.rows);
    thFree(m); return NULL;
  }
  /* Two tables of one shape: required phrases settle near-duplicates, variant
     phrases separate a license from what SPDX derives from it. */
  snprintf(path, sizeof(path), "%s/required_phrases.json", dir);
  { char *rj = thSlurp(path);
    m->reqPhr = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char **));
    m->reqSib = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char **));
    if (rj && m->reqPhr && m->reqSib) {
      thPhraseSection(m, rj, "\"required\"", m->reqPhr);
      thPhraseSection(m, rj, "\"siblings\"", m->reqSib);
    }
    free(rj); }

  /* A licence written wholly inside another has no wording of its own, so the
     required-phrase table has no row for it and no rule above reaches it. */
  snprintf(path, sizeof(path), "%s/containment.json", dir);
  { char *rj = thSlurp(path);
    m->conIn = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char **));
    m->conNear = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char **));
    m->conDist = calloc((size_t) (m->nClasses ? m->nClasses : 1),
                        sizeof(char ***));
    if (rj && m->conIn) thPhraseSection(m, rj, "\"contained\"", m->conIn);
    /* A rule that reads the wording works from a looser relation than the
       grant rule, which settles a pair from the relation alone. An older
       table carries only the tight one, and the phrases align with it. */
    if (rj && m->conNear)
      thPhraseSection(m, rj, strstr(rj, "\"near\"") ? "\"near\"" : "\"contained\"",
                      m->conNear);
    if (rj && m->conDist) thRenderSection(m, rj, "\"distinct\"", m->conDist);
    /* the words naming each licence's holder, from its copyright lines */
    m->conHolder = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char **));
    if (rj && m->conHolder) thPhraseSection(m, rj, "\"holder\"", m->conHolder);
    /* the copyright statement printed above each licence's text */
    m->conStmt = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char **));
    if (rj && m->conStmt) thPhraseSection(m, rj, "\"statement\"", m->conStmt);
    free(rj); }

  /* SPDX gives a licence's first release an identifier carrying no version,
     and the version-less name belongs to it, so naming the licence and nothing
     else selects the earliest by default. */
  snprintf(path, sizeof(path), "%s/version_families.json", dir);
  { char *rj = thSlurp(path);
    m->unver = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char **));
    if (rj && m->unver) thPhraseSection(m, rj, "\"unversioned\"", m->unver);
    free(rj); }

  /* Kept out of required_phrases.json: the verifier reads that as a fitted
     feature, so widening it penalizes every newly covered license. */
  snprintf(path, sizeof(path), "%s/variant_phrases.json", dir);
  { char *rj = thSlurp(path);
    m->varPhr = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char **));
    if (rj && m->varPhr)
      thPhraseSection(m, rj, "\"required\"", m->varPhr);
    free(rj); }

  /* How each license has actually been written, one phrase list per rendering. */
  snprintf(path, sizeof(path), "%s/body_renderings.json", dir);
  { char *rj = thSlurp(path);
    m->rendPhr = calloc((size_t) (m->nClasses ? m->nClasses : 1),
                        sizeof(char ***));
    if (rj && m->rendPhr) thRenderSection(m, rj, "\"renderings\"", m->rendPhr);
    free(rj); }


  /* An identifier SPDX chooses by a declaration beside the notice rather than
     by the license's own words. A phrase here is absent from the licence's own
     body, or every file under it would carry the declaration. */
  snprintf(path, sizeof(path), "%s/declared_variants.json", dir);
  { char *rj = thSlurp(path);
    if (rj) {
      m->nDecl = thLoadDeclared(rj, "\"declared\"", m->declBase, m->declTo, m->declPhr,
                                m->declAhead, m->declUnless);
      /* the same words with a licensor named inside the terms */
      m->nLicensor = thLoadDeclared(rj, "\"licensor\"", m->licBase, m->licTo, m->licPhr,
                                    NULL, NULL);
      free(rj);
    } }

  /* Which numbered conditions each BSD-style license writes, read off SPDX's
     own text. The family contains itself, so the conditions present are the
     only thing that separates its members. */
  snprintf(path, sizeof(path), "%s/clause_signatures.json", dir);
  { char *rj = thSlurp(path);
    m->clauseSig = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char *));
    if (rj && m->clauseSig) {
      const char *p = strstr(rj, "\"clauses\"");
      if (p && (p = strchr(p, '{')) != NULL) {
        p++;
        while (m->nClause < TH_CLAUSE_MAX) {
          char *k = thJstrAt(&p), *v;
          if (!k) break;
          v = thJstrAt(&p);
          if (!v) { free(k); break; }
          m->clauseKey[m->nClause] = k;
          m->clausePhr[m->nClause] = v;
          m->nClause++;
          while (*p == ',' || *p == ' ' || *p == '\n') p++;
          if (*p == '}') break;
        }
      }
      p = strstr(rj, "\"families\"");
      if (p && (p = strchr(p, '{')) != NULL) {
        p++;
        while (m->nFamily < TH_FAMILY_MAX) {
          char *k = thJstrAt(&p), *v;
          if (!k) break;
          v = thJstrAt(&p);
          if (!v) { free(k); break; }
          m->famName[m->nFamily] = k;
          m->famKeys[m->nFamily] = v;
          m->nFamily++;
          while (*p == ',' || *p == ' ' || *p == '\n') p++;
          if (*p == '}') break;
        }
      }
      /* which names belong to a family whether or not SPDX has their text:
         a member without a signature cannot be checked against the
         conditions written, and is no candidate where a checked one was
         ruled out */
      p = strstr(rj, "\"members\"");
      if (p && (p = strchr(p, '{')) != NULL) {
        p++;
        for (;;) {
          char *k = thJstrAt(&p), *v;
          int q;
          if (!k) break;
          v = thJstrAt(&p);
          if (!v) { free(k); break; }
          for (q = 0; q < m->nFamily; q++)
            if (strcmp(m->famName[q], k) == 0 && !m->famMemberOk[q]
                && regcomp(&m->famMember[q], v, REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0)
              m->famMemberOk[q] = 1;
          free(k); free(v);
          while (*p == ',' || *p == ' ' || *p == '\n') p++;
          if (*p == '}') break;
        }
      }
      p = strstr(rj, "\"signatures\"");
      if (p && (p = strchr(p, '{')) != NULL) {
        p++;
        for (;;) {
          char *name = thJstrAt(&p), *sig;
          int ci;
          if (!name) break;
          sig = thJstrAt(&p);
          if (!sig) { free(name); break; }
          for (ci = 0; ci < m->nClasses; ci++)
            if (strcmp(m->classes[ci], name) == 0) break;
          free(name);
          if (ci < m->nClasses) m->clauseSig[ci] = sig; else free(sig);
          while (*p == ',' || *p == ' ' || *p == '\n') p++;
          if (*p == '}') break;
        }
      }
      /* the words of the grant that run into each licence's conditions list */
      m->clauseLead = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(char *));
      p = strstr(rj, "\"lead\"");
      if (p && m->clauseLead && (p = strchr(p, '{')) != NULL) {
        p++;
        for (;;) {
          char *name = thJstrAt(&p), *words;
          int ci;
          if (!name) break;
          words = thJstrAt(&p);
          if (!words) { free(name); break; }
          for (ci = 0; ci < m->nClasses; ci++)
            if (strcmp(m->classes[ci], name) == 0) break;
          free(name);
          if (ci < m->nClasses) m->clauseLead[ci] = words; else free(words);
          while (*p == ',' || *p == ' ' || *p == '\n') p++;
          if (*p == '}') break;
        }
      }
      /* how many conditions each member's own text numbers, and how long
         the list of a licence that numbers two or more runs */
      m->clauseCount = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(int));
      m->clauseSpan = calloc((size_t) (m->nClasses ? m->nClasses : 1), sizeof(int));
      { int t;
        for (t = 0; t < 2; t++) {
          int *into = t ? m->clauseSpan : m->clauseCount;
          p = strstr(rj, t ? "\"span\"" : "\"enumerated\"");
          if (!p || !into || (p = strchr(p, '{')) == NULL) continue;
          p++;
          for (;;) {
            char *name = thJstrAt(&p);
            int ci;
            if (!name) break;
            while (*p == ':' || *p == ' ') p++;
            for (ci = 0; ci < m->nClasses; ci++)
              if (strcmp(m->classes[ci], name) == 0) { into[ci] = (int) strtol(p, NULL, 10); break; }
            free(name);
            while (*p && *p != ',' && *p != '}') p++;
            if (*p == '}') break;
          }
        } }
    }
    free(rj); }

  /* A license and the variants SPDX names by extending its identifier, found
     by name because rank is the thing that is wrong when they are confused. */
  if (m->nClasses > 0) {
    int a, b;
    int *order = malloc((size_t) m->nClasses * sizeof(int));
    m->variant = calloc((size_t) m->nClasses, sizeof(int *));
    m->variantBase = malloc((size_t) m->nClasses * sizeof(int));
    if (m->variantBase)
      for (a = 0; a < m->nClasses; a++) m->variantBase[a] = -1;
    if (order && m->variant) {
      for (a = 0; a < m->nClasses; a++) order[a] = a;
      /* insertion order is the class order, which the exporter writes sorted */
      for (a = 0; a < m->nClasses; a++) {
        size_t la = strlen(m->classes[order[a]]);
        int n = 0;
        for (b = a + 1; b < m->nClasses; b++) {
          const char *nb = m->classes[order[b]];
          if (strncmp(nb, m->classes[order[a]], la) || nb[la] != '-') break;
          n++;
        }
        for (b = a + 1; b <= a + n; b++) {
          int x = order[a], y = order[b], *p1, *p2;
          int k1 = 0, k2 = 0;
          while (m->variant[x] && m->variant[x][k1] >= 0) k1++;
          while (m->variant[y] && m->variant[y][k2] >= 0) k2++;
          /* thGrow frees its argument on failure, so each slot takes the
             result at once or it is left dangling for thFree. */
          p1 = thGrow(m->variant[x], (size_t) (k1 + 2) * sizeof(int));
          m->variant[x] = p1;
          if (!p1) continue;
          p2 = thGrow(m->variant[y], (size_t) (k2 + 2) * sizeof(int));
          m->variant[y] = p2;
          if (!p2) continue;
          p1[k1] = y; p1[k1 + 1] = -1;
          p2[k2] = x; p2[k2 + 1] = -1;
          /* The nearest license it extends, where several do: what
             BSD-3-Clause-HP varies is BSD-3-Clause, not BSD. */
          if (m->variantBase
              && (m->variantBase[y] < 0
                  || strlen(m->classes[x])
                     > strlen(m->classes[m->variantBase[y]])))
            m->variantBase[y] = x;
        }
      }
    }
    free(order);
  }

  /* Evidence tables. Both are "class<TAB>...", one entry per line, in the same
     order the Python package writes them: strongest name variant first. */
  m->byName = thHashNew(1024);
  { int ci;
    for (ci = 0; ci < m->nClasses; ci++) thHashPut(m->byName, m->classes[ci], ci); }
  { size_t nc = (size_t) (m->nClasses ? m->nClasses : 1);
    int *nName = calloc(nc, sizeof(int)), *nBody = calloc(nc, sizeof(int));
    int tbl;
    m->nameVar = calloc(nc, sizeof(char **));
    m->nameW = calloc(nc, sizeof(float *));
    m->bodyPhr = calloc(nc, sizeof(char **));
    m->bodyW = calloc(nc, sizeof(float *));
    m->bodyFam = calloc(nc, sizeof(int *));
    if (!nName || !nBody || !m->nameVar || !m->nameW || !m->bodyPhr
        || !m->bodyW || !m->bodyFam) {
      free(nName); free(nBody);
      snprintf(err, errLen, "out of memory"); thFree(m); return NULL;
    }
    for (tbl = 0; tbl < 2; tbl++) {
      FILE *f;
      char *line = NULL; size_t cap = 0; ssize_t got;
      snprintf(path, sizeof(path), "%s/%s", dir,
               tbl ? "body_evidence.tsv" : "name_evidence.tsv");
      f = fopen(path, "r");
      if (!f) continue;
      while ((got = getline(&line, &cap, f)) > 0) {
        char *tab = strchr(line, '\t'), *phrase;
        float w = 0.0f;
        int ci, *count;
        if (line[got - 1] == '\n') line[--got] = '\0';
        if (!tab) continue;
        *tab = '\0';
        ci = thHashGet(m->byName, line, strlen(line));
        if (ci < 0) continue;
        phrase = tab + 1;
        { char *tab2 = strchr(phrase, '\t');
          if (!tab2) continue;
          *tab2 = '\0';
          w = (float) atof(phrase);
          phrase = tab2 + 1; }
        count = tbl ? nBody + ci : nName + ci;
        if (tbl) {
          /* The family count says how much the phrase narrows down. */
          char *tab3 = strchr(phrase, '\t');
          int fam;
          if (!tab3) continue;
          *tab3 = '\0';
          fam = atoi(phrase);
          phrase = tab3 + 1;
          m->bodyPhr[ci] = thGrow(m->bodyPhr[ci], (size_t) (*count + 2) * sizeof(char *));
          m->bodyW[ci] = thGrow(m->bodyW[ci], (size_t) (*count + 1) * sizeof(float));
          m->bodyFam[ci] = thGrow(m->bodyFam[ci], (size_t) (*count + 1) * sizeof(int));
          if (!m->bodyPhr[ci] || !m->bodyW[ci] || !m->bodyFam[ci]) continue;
          m->bodyPhr[ci][*count] = strdup(phrase);
          m->bodyW[ci][*count] = w;
          m->bodyFam[ci][*count] = fam;
          m->bodyPhr[ci][++(*count)] = NULL;
        } else {
          m->nameVar[ci] = thGrow(m->nameVar[ci], (size_t) (*count + 2) * sizeof(char *));
          m->nameW[ci] = thGrow(m->nameW[ci], (size_t) (*count + 1) * sizeof(float));
          if (!m->nameVar[ci] || !m->nameW[ci]) continue;
          m->nameVar[ci][*count] = strdup(phrase);
          m->nameW[ci][*count] = w;
          m->nameVar[ci][++(*count)] = NULL;
        }
      }
      free(line); fclose(f);
    }
    /* Strongest first, whatever order the table came in: the first hit is
       the one reported, and a row appended by hand was never seen. */
    { int ci;
      for (ci = 0; ci < m->nClasses; ci++) {
        int n = nName[ci], i, j;
        if (!m->nameVar[ci]) continue;
        for (i = 1; i < n; i++) {
          char *ph = m->nameVar[ci][i];
          float w = m->nameW[ci][i];
          for (j = i; j > 0 && m->nameW[ci][j - 1] < w; j--) {
            m->nameVar[ci][j] = m->nameVar[ci][j - 1];
            m->nameW[ci][j] = m->nameW[ci][j - 1];
          }
          m->nameVar[ci][j] = ph;
          m->nameW[ci][j] = w;
        }
      } }
    free(nName); free(nBody);
    { int ci, any = 0;
      for (ci = 0; ci < m->nClasses && !any; ci++) if (m->nameVar[ci]) any = 1;
      if (!any) { /* no table: keep the old, laxer behavior */
        free(m->nameVar); free(m->nameW);
        m->nameVar = NULL; m->nameW = NULL;
      } } }

  /* Referential patterns, one per line, most specific first. */
  snprintf(path, sizeof(path), "%s/referential.tsv", dir);
  { FILE *f = fopen(path, "r");
    char *line = NULL; size_t cap = 0; ssize_t got;
    int rcap = 0;
    regex_t *re = NULL;
    if (f) {
      while ((got = getline(&line, &cap, f)) > 0) {
        char *tab, *pattern;
        unsigned char where = 0;
        if (line[got - 1] == '\n') line[--got] = '\0';
        tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        pattern = tab + 1;
        /* "name <tab> where <tab> pattern", and the older two-column form. */
        { char *tab2 = strchr(pattern, '\t');
          if (tab2) {
            *tab2 = '\0';
            if (!strcmp(pattern, "raw")) where = 2;
            else if (!strcmp(pattern, "both")) where = 1;
            else if (!strcmp(pattern, "grant")) where = 3;
            else if (!strcmp(pattern, "identify")) where = 4;
            else if (!strcmp(pattern, "identify-raw")) where = 5;
            pattern = tab2 + 1;
          } else if (strstr(pattern, "(style|like|type|ish)")) {
            where = 1;
          } }
        if (m->nRef == rcap) {
          rcap = rcap ? rcap * 2 : 64;
          re = thGrow(re, (size_t) rcap * sizeof(regex_t));
          m->refName = thGrow(m->refName, (size_t) rcap * sizeof(char *));
          m->refLit = thGrow(m->refLit, (size_t) rcap * sizeof(char *));
          m->refWhere = thGrow(m->refWhere, (size_t) rcap);
          if (!re || !m->refName || !m->refLit || !m->refWhere) break;
        }
        /* offsets are needed: a match must have a license word beside it */
        /* A statement the file makes about itself is read as written: the
           case of a name is part of the shape. The table writes newline and
           tab as \n and \t, which POSIX takes literally -- inside a bracket
           above all -- so they are spelled out before compiling. */
        if (where == 5) {
          char *w = pattern, *r = pattern;
          while (*r) {
            if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r += 2; }
            else if (r[0] == '\\' && r[1] == 't') { *w++ = '\t'; r += 2; }
            else *w++ = *r++;
          }
          *w = '\0';
        }
        if (regcomp(re + m->nRef, pattern,
                    where == 5 ? REG_EXTENDED : (REG_EXTENDED | REG_ICASE)) != 0)
          continue;
        m->refName[m->nRef] = strdup(line);
        m->refLit[m->nRef] = thRequiredLiteral(pattern, where != 5);
        m->refWhere[m->nRef] = where;
        m->nRef++;
      }
      free(line); fclose(f);
    }
    m->refRe = re; }

  snprintf(path, sizeof(path), "%s/gazetteer.txt", dir);
  { FILE *f = fopen(path, "r");
    char *line = NULL; size_t cap = 0; ssize_t got;
    m->gaz = thHashNew(4096);
    /* every opening run of words of every name, so a phrase that cannot
       grow into one is abandoned at the first token */
    m->gazPrefix = thHashNew(8192);
    if (f) {
      while ((got = getline(&line, &cap, f)) > 0) {
        int w = 1;
        char *q;
        if (line[got - 1] == '\n') line[--got] = '\0';
        if (!got) continue;
        for (q = line; *q; q++) if (*q == ' ') w++;
        if (w > m->gazMax) m->gazMax = w;
        thHashPut(m->gaz, line, 1);
        for (q = line; *q; q++)
          if (*q == ' ') { *q = '\0'; thHashPut(m->gazPrefix, line, 1); *q = ' '; }
        thHashPut(m->gazPrefix, line, 1);
      }
      free(line); fclose(f);
    } }
  return m;
}

static void thBlockFree(thBlock *b)
{
  thHashFree(b->vocab); free(b->idf); free(b->acc); free(b->touched); free(b->sortTmp);
  if (b->words) {
    int i;
    for (i = 0; i < TH_WC_SLOTS; i++) { free(b->words->slot[i].id); free(b->words->slot[i].cnt); }
    free(b->words);
  }
}

void thFree(thModel *m)
{
  thProfileReport(stderr);
  thTrailsFree();
  int i;
  if (!m) return;
  for (i = 0; i < m->nClasses; i++) {
    int k;
    if (m->nameVar && m->nameVar[i]) {
      for (k = 0; m->nameVar[i][k]; k++) free(m->nameVar[i][k]);
      free(m->nameVar[i]);
    }
    if (m->nameW) free(m->nameW[i]);
    if (m->bodyPhr && m->bodyPhr[i]) {
      for (k = 0; m->bodyPhr[i][k]; k++) free(m->bodyPhr[i][k]);
      free(m->bodyPhr[i]);
    }
    if (m->bodyW) free(m->bodyW[i]);
    if (m->bodyFam) free(m->bodyFam[i]);
  }
  if (m->rendPhr) {
    int q;
    for (q = 0; q < m->nClasses; q++) {
      int a, b;
      if (!m->rendPhr[q]) continue;
      for (a = 0; m->rendPhr[q][a]; a++) {
        for (b = 0; m->rendPhr[q][a][b]; b++) free(m->rendPhr[q][a][b]);
        free(m->rendPhr[q][a]);
      }
      free(m->rendPhr[q]);
    }
    free(m->rendPhr);
  }
  if (m->conDist) {
    int q;
    for (q = 0; q < m->nClasses; q++) {
      int a, b;
      if (!m->conDist[q]) continue;
      for (a = 0; m->conDist[q][a]; a++) {
        for (b = 0; m->conDist[q][a][b]; b++) free(m->conDist[q][a][b]);
        free(m->conDist[q][a]);
      }
      free(m->conDist[q]);
    }
    free(m->conDist);
  }
  { int q;
    for (q = 0; q < m->nLicensor; q++) {
      free(m->licBase[q]); free(m->licTo[q]); free(m->licPhr[q]);
    }
    for (q = 0; q < m->nDecl; q++) {
      free(m->declBase[q]); free(m->declTo[q]); free(m->declPhr[q]); free(m->declUnless[q]);
    } }
  free(m->variantBase);
  free(m->nameVar); free(m->nameW); free(m->bodyPhr);
  free(m->bodyW); free(m->bodyFam);
  for (i = 0; i < m->nRef; i++) {
    regfree(((regex_t *) m->refRe) + i);
    free(m->refLit[i]);
    free(m->refName[i]);
  }
  free(m->refRe); free(m->refName); free(m->refLit); free(m->refWhere);
  if (m->variant) { for (i = 0; i < m->nClasses; i++) free(m->variant[i]); }
  free(m->variant);
  if (m->varPhr) {
    for (i = 0; i < m->nClasses; i++)
      if (m->varPhr[i]) { int q; for (q = 0; m->varPhr[i][q]; q++) free(m->varPhr[i][q]);
                          free(m->varPhr[i]); }
  }
  free(m->varPhr);
  thHashFree(m->byName);
  for (i = 0; i < m->nGateBlk; i++) thBlockFree(m->gateBlk + i);
  for (i = 0; i < m->nLicBlk; i++)  thBlockFree(m->licBlk + i);
  for (i = 0; i < m->nSfxBlk; i++)  thBlockFree(m->sfxBlk + i);
  free(m->gateBlk); free(m->licBlk); free(m->sfxBlk);
  free(m->gate.bias); free(m->gate.dense);
  free(m->sfx.bias); free(m->sfx.dense);
  free(m->lic.bias);
  if (m->lic.idx) { for (i = 0; i < m->lic.rows; i++) { free(m->lic.idx[i]); free(m->lic.val[i]); } }
  free(m->lic.idx); free(m->lic.val); free(m->lic.nnz);
  free(m->lic.colPtr); free(m->lic.colRow); free(m->lic.colVal); free(m->lic.acc);
  for (i = 0; i < m->nClasses; i++) free(m->classes[i]);
  free(m->classes);
  thHashFree(m->gaz); thHashFree(m->gazPrefix);
  { int q, r;
    for (q = 0; q < m->nClasses; q++) {
      if (m->reqPhr && m->reqPhr[q]) { for (r = 0; m->reqPhr[q][r]; r++) free(m->reqPhr[q][r]); free(m->reqPhr[q]); }
      if (m->reqSib && m->reqSib[q]) { for (r = 0; m->reqSib[q][r]; r++) free(m->reqSib[q][r]); free(m->reqSib[q]); }
      if (m->conIn && m->conIn[q]) { for (r = 0; m->conIn[q][r]; r++) free(m->conIn[q][r]); free(m->conIn[q]); }
      if (m->conHolder && m->conHolder[q]) { for (r = 0; m->conHolder[q][r]; r++) free(m->conHolder[q][r]); free(m->conHolder[q]); }
      if (m->conStmt && m->conStmt[q]) { for (r = 0; m->conStmt[q][r]; r++) free(m->conStmt[q][r]); free(m->conStmt[q]); }
      if (m->conNear && m->conNear[q]) { for (r = 0; m->conNear[q][r]; r++) free(m->conNear[q][r]); free(m->conNear[q]); }
      if (m->unver && m->unver[q]) { for (r = 0; m->unver[q][r]; r++) free(m->unver[q][r]); free(m->unver[q]); }
    } }
  free(m->reqPhr); free(m->reqSib); free(m->conIn); free(m->conNear); free(m->conHolder);
  free(m->conStmt); free(m->unver);
  { int q;
    for (q = 0; q < m->nClause; q++) { free(m->clauseKey[q]); free(m->clausePhr[q]); }
    for (q = 0; q < m->nFamily; q++) {
      free(m->famName[q]); free(m->famKeys[q]);
      if (m->famMemberOk[q]) regfree(&m->famMember[q]);
    }
    if (m->clauseSig) for (q = 0; q < m->nClasses; q++) free(m->clauseSig[q]);
    if (m->clauseLead) for (q = 0; q < m->nClasses; q++) free(m->clauseLead[q]);
    free(m->clauseCount); free(m->clauseSpan);
    free(m->clauseSig); free(m->clauseLead); }
  free(m->isException);
  free(m->calLo); free(m->calHi); free(m->calScore);
  for (i = 0; i < m->nGrant; i++) { regfree(((regex_t *) m->grantRe) + i); free(m->grantLit[i]); }
  for (i = 0; i < m->nMention; i++) { regfree(((regex_t *) m->mentionRe) + i); free(m->mentionLit[i]); }
  free(m->grantRe); free(m->mentionRe); free(m->grantLit); free(m->mentionLit);
  free(m);
}

/* ---------- decision ---------- */

/** split "Base-1.2-only" into base name, version and suffix */
static void thSplit(const char *n, char *base, size_t bl, char *ver, size_t vl,
    char *suf, size_t sl)
{
  const char *d;
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s", n);
  suf[0] = ver[0] = '\0';
  { size_t L = strlen(tmp);
    if (L > 5 && !strcmp(tmp + L - 5, "-only")) { snprintf(suf, sl, "only"); tmp[L - 5] = '\0'; }
    else if (L > 9 && !strcmp(tmp + L - 9, "-or-later")) { snprintf(suf, sl, "or-later"); tmp[L - 9] = '\0'; } }
  d = strrchr(tmp, '-');
  if (d && d[1] && (isdigit((unsigned char) d[1]))) {
    const char *q; int ok = 1;
    for (q = d + 1; *q; q++) {
      if (isdigit((unsigned char) *q) || *q == '.') continue;
      /* A trailing lowercase letter belongs to the version: LPPL-1.3c. */
      if (!q[1] && *q >= 'a' && *q <= 'z' && q > d + 1 &&
          isdigit((unsigned char) q[-1])) continue;
      ok = 0; break;
    }
    if (ok) { snprintf(ver, vl, "%s", d + 1); snprintf(base, bl, "%.*s", (int) (d - tmp), tmp); return; }
  }
  snprintf(base, bl, "%s", tmp);
}

/** normalize "2.0" to "2" so a text saying "version 2" matches */
static void thVerNorm(const char *v, char *o, size_t ol)
{
  char t[64];
  size_t L;
  snprintf(t, sizeof(t), "%s", v);
  L = strlen(t);
  while (L && t[L - 1] >= 'a' && t[L - 1] <= 'h') t[--L] = '\0'; /* 1.3c -> 1.3 */
  while (L > 2 && t[L - 1] == '0' && t[L - 2] == '.') { t[L - 2] = '\0'; L -= 2; }
  snprintf(o, ol, "%s", t);
}

/**
 * \brief Collect version tokens that appear in a license context.
 *
 * A number counts only after a license name or "version" within 28 characters;
 * a 19xx/20xx run is a copyright year. Raw text, punctuation intact.
 */
static const char *TH_LICWORD[] = {
  "license", "licence", "gpl", "lgpl", "agpl", "gfdl", "epl", "mpl", "cddl",
  "apache", "cc-by", "cc by", "artistic", "version", "clause", "apl", "osl",
  "afl", "zpl", "w3c", "python", "php", "eupl", "ecl", "npl", "sspl", "bsl",
  "rpsl", "qpl",
  /* Creative Commons puts its modifiers right before the version, so without
     these the version is never read as stated. */
  "noncommercial", "non commercial", "noderivs", "noderivatives",
  "no derivatives", "derivative works", "sharealike", "share alike",
  "attribution", "unported", NULL
};

/**
 * \brief Is this codepoint part of a word?
 *
 * An accented letter must count as one, or a name inside it reads as a word of
 * its own. Deciding per byte would make curly quotes letters too.
 */
static int thCpWord(unsigned cp)
{
  if (cp < 0x80) return isalnum((int) cp);
  if (cp <= 0xBF) return 0; /* C1 controls; Latin-1 punctuation */
  if (cp == 0xD7 || cp == 0xF7) return 0; /* multiplication, division */
  if (cp >= 0x2000 && cp <= 0x206F) return 0; /* quotes, dashes, ellipsis */
  if (cp >= 0x2E00 && cp <= 0x2E7F) return 0; /* supplemental punctuation */
  if (cp >= 0x3000 && cp <= 0x303F) return 0; /* CJK punctuation */
  if (cp == 0xFEFF) return 0; /* byte-order mark */
  return 1;
}

/** Decode the UTF-8 sequence at `i`; invalid bytes decode as themselves. */
static unsigned thCpAt(const char *s, size_t rl, size_t i)
{
  unsigned char c = (unsigned char) s[i];
  unsigned cp;
  size_t need, k;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; need = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; need = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; need = 3; }
  else return c;
  if (i + need >= rl) return c; /* truncated sequence */
  for (k = 1; k <= need; k++) {
    unsigned char d = (unsigned char) s[i + k];
    if ((d & 0xC0) != 0x80) return c;
    cp = (cp << 6) | (d & 0x3Fu);
  }
  return cp;
}

/** Is the character at byte offset `i` part of a word? */
static int thWordAt(const char *s, size_t rl, size_t i)
{
  if (i >= rl) return 0;
  return thCpWord(thCpAt(s, rl, i));
}

/** Is the character ending just before byte offset `i` part of a word? */
static int thWordBefore(const char *s, size_t i)
{
  size_t at;
  if (i == 0) return 0;
  at = i - 1;
  while (at > 0 && ((unsigned char) s[at] & 0xC0) == 0x80) at--;
  return thCpWord(thCpAt(s, i, at));
}

static int thCtxOk(const char *t, size_t at)
{
  char pre[32];
  size_t lo = at > 28 ? at - 28 : 0, n = at - lo, i;
  if (n == 0) return 0;
  for (i = 0; i < n; i++) pre[i] = (char) tolower((unsigned char) t[lo + i]);
  pre[n] = '\0';
  /* Separators may include whitespace: a wrapped notice splits "either
     version" from the number it states. */
  while (n && (isspace((unsigned char) pre[n - 1]) || pre[n - 1] == '-' ||
               pre[n - 1] == ',' || pre[n - 1] == ':' ||
               pre[n - 1] == '.')) pre[--n] = '\0';
  if (n == 0) return 0;
  if (pre[n - 1] == 'v') { /* a bare "v" prefix */
    static const char *const glued[] = {"gplv", "lgplv", "agplv", "gfdlv", NULL};
    /* A word character before the v means no version is stated: the V1 in
       "License_CeCILL-B_V1-fr.html" is part of a filename -- unless the word
       is the family's own: "GPLv3", "LGPLv2.1". */
    if (n == 1 || (!thWordBefore(pre, n - 1) && pre[n - 2] != '_'))
      return 1;
    for (i = 0; glued[i]; i++) {
      size_t L = strlen(glued[i]);
      if (n >= L && !strcmp(pre + n - L, glued[i])
          && (n == L || !isalnum((unsigned char) pre[n - L - 1]))) return 1;
    }
  }
  for (i = 0; TH_LICWORD[i]; i++) {
    size_t L = strlen(TH_LICWORD[i]);
    if (n >= L && !strcmp(pre + n - L, TH_LICWORD[i])) {
      /* a manifest's own field -- "Version: 1.0" at the start of a line --
         is the package's version, not a licence's */
      if (!strcmp(TH_LICWORD[i], "version") && at > 0) {
        size_t k = lo + n;            /* just past "version" in the raw text */
        size_t j = k;
        while (j < at && (t[j] == ' ' || t[j] == '\t')) j++;
        if (j < at && t[j] == ':') {
          size_t b = lo + n - L;
          while (b > 0 && (t[b - 1] == ' ' || t[b - 1] == '\t')) b--;
          if (b == 0 || t[b - 1] == '\n') return 0;
        }
      }
      return 1;
    }
  }
  return 0;
}

/** does `ver` occur in a license context anywhere in the raw text? */
static int thTextHasVersion(const char *raw, const char *ver)
{
  char want[64];
  size_t i = 0, n = strlen(raw);
  thVerNorm(ver, want, sizeof(want));
  while (i < n) {
    size_t s, e;
    char tok[64], norm[64];
    if (!isdigit((unsigned char) raw[i])) { i++; continue; }
    s = i;
    while (i < n && (isdigit((unsigned char) raw[i]) ||
           (raw[i] == '.' && i + 1 < n && isdigit((unsigned char) raw[i + 1])))) i++;
    e = i;
    if (e - s >= sizeof(tok)) continue;
    memcpy(tok, raw + s, e - s); tok[e - s] = '\0';
    /* a plain 19xx/20xx is a year */
    if (e - s == 4 && !strchr(tok, '.') &&
        ((tok[0] == '1' && tok[1] == '9') || (tok[0] == '2' && tok[1] == '0'))) continue;
    if (!thCtxOk(raw, s)) continue;
    thVerNorm(tok, norm, sizeof(norm));
    if (!strcmp(norm, want)) return 1;
  }
  return 0;
}

/** Calibrated confidence for a margin, as a percentage. */
/**
 * \brief Does `needle` occur as whole words inside the normalized `hay`?
 */
static int thPhraseIn(const char *hay, const char *needle)
{
  size_t nl = strlen(needle);
  const char *p = hay;
  while ((p = strstr(p, needle)) != NULL) {
    if ((p == hay || p[-1] == ' ') && (p[nl] == '\0' || p[nl] == ' ')) return 1;
    p++;
  }
  return 0;
}

/**
 * \brief Match `phrase` at `start`, allowing any punctuation between its words.
 */
static int thMatchAt(const char *phrase, const char *raw, size_t rl, size_t start,
    size_t *endOut)
{
  const char *p = phrase;
  size_t j = start;
  for (;;) {
    if (j >= rl || !isalnum((unsigned char) raw[j])) return 0;
    /* Normalization spells "v 2" and "ver 2" as "version 2", so the word
       "version" before a number stands for any of the three. */
    if (strncmp(p, "version ", 8) == 0 && isdigit((unsigned char) p[8])) {
      size_t k = j;
      while (k < rl && isalpha((unsigned char) raw[k])) k++;
      if ((k - j == 1 || k - j == 3 || k - j == 7)
          && strncasecmp(raw + j, "version", k - j) == 0
          && (k >= rl || !isalnum((unsigned char) raw[k]))) {
        j = k; p += 7;
      } else {
        return 0;
      }
    }
    while (*p && *p != ' ') {
      if (j >= rl || tolower((unsigned char) raw[j]) != *p) return 0;
      j++; p++;
    }
    if (!*p) {
      if (thWordAt(raw, rl, j)) return 0; /* a longer word */
      *endOut = j; return 1;
    }
    p++; /* the space */
    /* Normalization spells "+" as "plus", so "gplv3 plus" must match
       "GPLv3+" or every "+" variant in the name table matches nothing. */
    if (strncmp(p, "plus", 4) == 0 && (p[4] == '\0' || p[4] == ' ')) {
      size_t k = j;
      while (k < rl && !isalnum((unsigned char) raw[k]) && raw[k] != '+') k++;
      if (k < rl && raw[k] == '+') {
        p += 4;
        j = k + 1;
        if (!*p) {
          if (thWordAt(raw, rl, j)) return 0;             /* a longer word */
          *endOut = j; return 1;
        }
        p++;
        { size_t before = j;
          while (j < rl && !isalnum((unsigned char) raw[j])) j++;
          if (j == before) return 0; }
        continue;
      }
    }
    /* Only the ends of the phrase need a word boundary; between words an
       accented letter is a separator, but there must be one. */
    { size_t before = j;
      while (j < rl && !isalnum((unsigned char) raw[j])) j++;
      if (j == before) return 0;
    }
  }
}

/**
 * \brief Is the phrase written as a name here, not buried in an identifier?
 *
 * A hyphen or underscore with word characters on both sides means the match is
 * one segment of a compound. `/` and `.` are not glue, so a URL still counts.
 */
/** Is a whole word from `words` present in [p, end)? */
static int thWordIn(const char *p, const char *end, const char *const *words)
{
  const char *q;
  int w;
  for (q = p; q < end; q++) {
    if (q > p && (isalnum((unsigned char) q[-1]) || q[-1] == '_')) continue;
    for (w = 0; words[w]; w++) {
      size_t n = strlen(words[w]);
      if (q + n <= end && !strncasecmp(q, words[w], n)
          && (q + n == end || !(isalnum((unsigned char) q[n]) || q[n] == '_')))
        return 1;
    }
  }
  return 0;
}

/** Whether the alphanumeric run is a word that says the glued name is a license's. */
static int thGlueWord(const char *w, size_t n)
{
  static const char *const words[] = {"license", "licence", "licensed", "licenced",
      "licenses", "licences", "licensing", "licencing", NULL};
  int k;
  for (k = 0; words[k]; k++)
    if (strlen(words[k]) == n && strncasecmp(w, words[k], n) == 0) return 1;
  return 0;
}

/** The word ending at raw[end], read back over alphanumerics. */
static int thGlueWordBefore(const char *raw, size_t end)
{
  size_t k = end;
  while (k > 0 && isalnum((unsigned char) raw[k - 1])) k--;
  return thGlueWord(raw + k, end - k);
}

/** The word starting at raw[start], read on over alphanumerics. */
static int thGlueWordAt(const char *raw, size_t rl, size_t start)
{
  size_t k = start;
  while (k < rl && isalnum((unsigned char) raw[k])) k++;
  return thGlueWord(raw + start, k - start);
}

/**
 * \brief Does a copyright notice open this line before `at`?
 *
 * A notice, not merely the word: it must carry a year or a holder, or a name
 * in the holder is read as the license while `@copyright GPLv2` is lost.
 */
/** Where a copyright notice's content starts on this line before at, or NULL. */
static const char *thCopyrightNotice(const char *line, const char *at)
{
  static const char *const holder[] = {"inc", "corp", "corporation", "ltd",
      "llc", "gmbh", "foundation", "university", "institute", "consortium", NULL};
  const char *p;
  for (p = line; p < at; p++) {
    const char *after = NULL;
    if (!strncasecmp(p, "copyright", 9)) after = p + 9;
    else if (!strncasecmp(p, "(c)", 3)) after = p + 3;
    else if ((unsigned char) p[0] == 0xC2 && (unsigned char) p[1] == 0xA9) after = p + 2;
    if (!after || after > at) continue;
    if (after == at || (isalnum((unsigned char) *after) || *after == '_')) continue;
    while (after < at && !(isalnum((unsigned char) *after) || *after == '_')) after++;
    if (after + 4 <= at && (!strncmp(after, "19", 2) || !strncmp(after, "20", 2))
        && isdigit((unsigned char) after[2]) && isdigit((unsigned char) after[3]))
      return after;
    { const char *stop = after + 60 < at ? after + 60 : at;
      if (thWordIn(after, stop, holder)) return after; }
  }
  return NULL;
}

static int thWrittenAsName(const char *phrase, const char *raw, size_t *atOut,
    size_t *endOut)
{
  size_t rl = strlen(raw), i;
  for (i = 0; i < rl; i++) {
    size_t end;
    int left, right;
    if (!isalnum((unsigned char) raw[i])) continue;
    if (i && thWordBefore(raw, i)) continue;
    if (!thMatchAt(phrase, raw, rl, i, &end)) continue;
    /* An "@" before the name settles it: an address or a package scope, not
       a license. Glue on both sides was too weak a test. */
    if (i && raw[i - 1] == '@') continue;
    /* a macro, a variable, a directive: "%doc license.html" in an RPM spec
       says where a file goes, and the word after the sigil is not a name */
    if (i && strchr("%$\\", raw[i - 1])) continue;
    /* an extension -- "COPYING.gplv3" names a file -- where the name ends the
       token; "rock.mit-license.org" goes on and is a host */
    if (i >= 2 && raw[i - 1] == '.' && isalnum((unsigned char) raw[i - 2])
        && (end >= rl || !(isalnum((unsigned char) raw[end]) || strchr(".-_/", raw[end]))))
      continue;
    /* A directory inside a longer path names a place, not a license. Counted
       on both sides, because the rest of the path may sit either way: two
       segments, since one slash is an alternation, "MIT/X11". */
    { size_t k = end;
      int segments = 0;
      while (k < rl && !isspace((unsigned char) raw[k]) && !strchr("\"'>),;", raw[k])) {
        if (raw[k] == '/' && k + 1 < rl && isalnum((unsigned char) raw[k + 1]))
          segments++;
        k++;
      }
      k = i;
      while (k > 0 && !isspace((unsigned char) raw[k - 1])
             && !strchr("\"'<(,;", raw[k - 1])) {
        if (raw[k - 1] == '/' && k >= 2 && isalnum((unsigned char) raw[k - 2]))
          segments++;
        k--;
      }
      if (segments >= 2) continue;
    }
    /* a name inside a copyright notice is the holder's, not the license's */
    { static const char *const grant[] = {"license", "licence", "licensed",
          "licenced", "permission", "granted", "terms", "spdx", NULL};
      const char *bol = raw + i, *eol = raw + end, *notice;
      while (bol > raw && bol[-1] != '\n') bol--;
      while (*eol && *eol != '\n') eol++;
      notice = thCopyrightNotice(bol, raw + i);
      /* a grant word on either side of the name lifts the guard:
         "Copyright Example Corp; Licensed MIT" grants MIT */
      if (notice && !thWordIn(raw + end, eol, grant)
          && !thWordIn(notice, raw + i, grant)) continue; }
    /* A name glued to a word on either side is one segment of a compound --
       "mit-krb5" is a package, not a license -- unless the word it is glued
       to says what the name is: "MIT-licensed" and "mit-license.org" name
       the license. "MIT-style" does not: that names a family. */
    left = i >= 2 && (raw[i - 1] == '-' || raw[i - 1] == '_')
           && thWordBefore(raw, i - 1) && !thGlueWordBefore(raw, i - 1);
    right = end + 1 < rl && (raw[end] == '-' || raw[end] == '_' || raw[end] == '@')
            && thWordAt(raw, rl, end + 1) && !thGlueWordAt(raw, rl, end + 1);
    if (!(left || right)) { *atOut = i; *endOut = end; return 1; }
  }
  return 0;
}

/**
 * \brief Is this token used as a license name, or is it just a word here?
 *
 * "SPL", "VSL", "GD" and "DOC" are instructions and abbreviations as well as
 * license names, so a single token needs a license word beside it.
 */
/**
 * \brief The sentence mark in raw[lo:hi] nearest the name, or -1.
 *
 * The name is at hi reading back (direction -1) and at lo reading on (1); the
 * text's own edge is one where the reach touches it.
 */
static long thSentenceEdge(const char *raw, size_t lo, size_t hi, int direction,
    size_t len)
{
  long found = -1;
  size_t i;
  for (i = lo; i < hi; i++) {
    int mark = strchr(".;!?", raw[i]) != NULL && raw[i] != '\0';
    if (!mark && raw[i] == '\n') {
      size_t j = i + 1;
      while (j < hi && raw[j] != '\n' && isspace((unsigned char) raw[j])) j++;
      mark = j < hi && raw[j] == '\n';
    }
    if (!mark) continue;
    found = (long) i;
    if (direction > 0) break;
  }
  if (found < 0) {
    if (direction < 0 && lo == 0) found = 0;
    else if (direction > 0 && hi == len) found = (long) hi;
  }
  return found;
}

static int thLicenceContext2(const char *raw, size_t at, size_t end, int forName)
{
  /* Whole words, not a "licen" prefix. A copyright line names the holder, so
     it is context for a pointer but not for a name. */
  static const char *word[] = {"license", "licence", "licensing", "licencing",
      "copyright", "spdx", "terms", "distribut", "warrant", "permission", NULL};
  size_t rl = strlen(raw);
  size_t lo = at > 48 ? at - 48 : 0;
  size_t hi = end + 48 < rl ? end + 48 : rl;
  size_t i;
  long edge;
  int w;
  /* For a name the unit is the sentence it is in, not a distance: a
     parenthesised pointer -- "the BSD-style license (found in the LICENSE
     file in the root directory of this source tree) and the GPLv2" -- puts
     the license word well beyond any fixed span while leaving no doubt what
     "GPLv2" is. Read back and forward to the sentence's edge where one is in
     reach, the text's own edges included; a run-on paragraph has no edge in
     reach and is read over the fixed span. A pointer is read on normalised
     text, which has no sentences, and keeps the span. */
  if (forName) {
    edge = thSentenceEdge(raw, at > TH_SENTENCE_CAP ? at - TH_SENTENCE_CAP : 0, at, -1, rl);
    if (edge >= 0 && (size_t) edge < lo) lo = (size_t) edge;
    edge = thSentenceEdge(raw, end, end + TH_SENTENCE_CAP < rl ? end + TH_SENTENCE_CAP : rl, 1, rl);
    if (edge >= 0 && (size_t) edge > hi) hi = (size_t) edge;
  }
  for (i = lo; i < hi; i++)
    for (w = 0; word[w]; w++) {
      size_t n = strlen(word[w]);
      if (forName && strcmp(word[w], "copyright") == 0) continue;
      if (i + n <= hi && strncasecmp(raw + i, word[w], n) == 0) return 1;
    }
  return 0;
}

static int thLicenceContext(const char *raw, size_t at, size_t end)
{
  return thLicenceContext2(raw, at, end, 0);
}

static int thNameContext(const char *raw, size_t at, size_t end)
{
  return thLicenceContext2(raw, at, end, 1);
}

/**
 * \brief How surprising the strongest name of this license found here is.
 *
 * Names are matched whole: part of a name is not a name, so "patent" does not
 * say BSD-2-Clause-Patent. Variants are stored strongest first.
 */
static float thNameEvidence(thModel *m, int cls, const char *raw, const char *t)
{
  int k;
  if (!m->nameVar || !m->nameVar[cls] || !m->nameW || !m->nameW[cls]) return 0.0f;
  for (k = 0; m->nameVar[cls][k]; k++) {
    const char *phrase = m->nameVar[cls][k];
    size_t at, end;
    if (!thPhraseIn(t, phrase)) continue;
    if (!thWrittenAsName(phrase, raw, &at, &end)) continue;
    /* a bare word, used as a bare word, names nothing */
    if (!strchr(phrase, ' ') && !thNameContext(raw, at, end)) continue;
    return m->nameW[cls][k];
  }
  return 0.0f;
}

/** The strongest name of this license written here, or NULL. */
static const char *thNamePhrase(thModel *m, int cls, const char *raw, const char *t)
{
  int k;
  if (!m->nameVar || !m->nameVar[cls] || !m->nameW || !m->nameW[cls]) return NULL;
  for (k = 0; m->nameVar[cls][k]; k++) {
    const char *phrase = m->nameVar[cls][k];
    size_t at, end;
    if (!thPhraseIn(t, phrase)) continue;
    if (!thWrittenAsName(phrase, raw, &at, &end)) continue;
    if (!strchr(phrase, ' ') && !thNameContext(raw, at, end)) continue;
    return phrase;
  }
  return NULL;
}

/** Whether a name of this license that is not also the other's is written
    here: "GPLv2" names GPL-2.0-only apart from GPL-2.0-or-later, "GNU General
    Public License" names both. */
static int thNamedApart(thModel *m, int cls, int other, const char *raw, const char *t)
{
  const char *ph;
  int k;
  if (thNameEvidence(m, cls, raw, t) < m->thNameMin) return 0;
  ph = thNamePhrase(m, cls, raw, t);
  if (!ph || !m->nameVar || !m->nameVar[other]) return ph != NULL;
  for (k = 0; m->nameVar[other][k]; k++)
    if (strcmp(m->nameVar[other][k], ph) == 0) return 0;
  return 1;
}

/**
 * \brief How many words of this license's name are actually written here.
 *
 * Not how rare the name is, which says which license is unusual rather than
 * which one the text spells out.
 */
static int thNameSpan(thModel *m, int cls, const char *raw, const char *t)
{
  int k, best = 0;
  if (!m->nameVar || !m->nameVar[cls]) return 0;
  for (k = 0; m->nameVar[cls][k]; k++) {
    const char *phrase = m->nameVar[cls][k], *c;
    size_t at, end;
    int n = 1;
    for (c = phrase; *c; c++) if (*c == ' ') n++;
    if (n <= best) continue;
    if (!thPhraseIn(t, phrase)) continue;
    if (!thWrittenAsName(phrase, raw, &at, &end)) continue;
    if (!strchr(phrase, ' ') && !thNameContext(raw, at, end)) continue;
    best = n;
  }
  return best;
}

/**
 * \brief Whether a name of this license written here carries its version:
 *        the version's first number is a word of the longest name found.
 */
static int thVersionWrittenInName(thModel *m, int cls, const char *t)
{
  char base[192], ver[64], suf[16], vn[64], first[64];
  const char *longest = NULL;
  int k, best = 0;
  size_t i;
  thSplit(m->classes[cls], base, sizeof(base), ver, sizeof(ver), suf, sizeof(suf));
  if (!ver[0]) return 0;
  thVerNorm(ver, vn, sizeof(vn));
  for (i = 0; vn[i] && vn[i] != '.' && vn[i] != ' ' && i < sizeof(first) - 1; i++) first[i] = vn[i];
  first[i] = '\0';
  if (!first[0] || !m->nameVar || !m->nameVar[cls]) return 0;
  for (k = 0; m->nameVar[cls][k]; k++) {
    const char *phrase = m->nameVar[cls][k], *c;
    int n = 1;
    for (c = phrase; *c; c++) if (*c == ' ') n++;
    if (n > best && thPhraseIn(t, phrase)) { best = n; longest = phrase; }
  }
  if (!longest) return 0;
  { const char *w = longest; size_t fl = strlen(first);
    while (*w) {
      const char *e = w;
      while (*e && *e != ' ') e++;
      if ((size_t) (e - w) == fl && !strncmp(w, first, fl)) return 1;
      w = *e ? e + 1 : e;
    } }
  return 0;
}

/**
 * \brief How many of the license's own distinctive phrases are here.
 */
/**
 * \brief The text with runs of whitespace collapsed to one space.
 *
 * A compound like "MIT-style" survives normalization only by luck; matching it
 * needs the file as written, and only the line wrapping has to be undone.
 */
static char *thFlatten(const char *in)
{
  size_t n = strlen(in), i, j = 0;
  char *out = malloc(n + 1);
  if (!out) return NULL;
  for (i = 0; i < n; i++) {
    if (isspace((unsigned char) in[i])) {
      if (j && out[j - 1] == ' ') continue;
      out[j++] = ' ';
    } else {
      out[j++] = in[i];
    }
  }
  out[j] = '\0';
  return out;
}

/**
 * \brief How much of this license's own text is here, weighted.
 *
 * A phrase is worth -log of the share of families using it. With \a distinct
 * only rare phrases count, asking "is it this license" not "is license text
 * here".
 */
static float thBodyEvidence(thModel *m, int cls, const char *t, float cap,
    int distinct)
{
  int k;
  float total = 0.0f;
  if (!m->bodyPhr || !m->bodyPhr[cls] || !m->bodyW || !m->bodyW[cls]
      || !m->bodyFam || !m->bodyFam[cls]) return 0.0f;
  for (k = 0; m->bodyPhr[cls][k]; k++) {
    if (distinct && m->bodyFam[cls][k] > TH_DISTINCT) continue;
    if (strstr(t, m->bodyPhr[cls][k])) {
      total += m->bodyW[cls][k];
      if (total >= cap) return cap;
    }
  }
  return total;
}


/**
 * \brief Where a license's text is written out: the dense runs of its
 *        wording, as [start, end] in normalised characters.
 *
 * A run has no gap wider than TH_TEXT_GAP, is at least TH_TEXT_RUN long and
 * is covered by the license's phrases to TH_TEXT_DENSITY. \p map is scratch
 * of at least \p len + 1 bytes.
 */
static int thTextRuns(thModel *m, int cls, const char *nt, unsigned char *map,
    size_t len, long runs[][2], int cap)
{
  int k, n = 0;
  long start = -1, last = -1, hits = 0, i;
  memset(map, 0, len + 1);
  for (k = 0; m->bodyPhr[cls][k]; k++) {
    const char *ph = m->bodyPhr[cls][k];
    size_t pl = strlen(ph);
    const char *at = strstr(nt, ph);
    while (at) {
      memset(map + (size_t) (at - nt), 1, pl);
      at = strstr(at + 1, ph);
    }
  }
  for (i = 0; i <= (long) len; i++) {
    int on = i < (long) len && map[i];
    if (on && last >= 0 && i - last <= TH_TEXT_GAP) { last = i; hits++; continue; }
    if (!on && i < (long) len) continue;
    /* a run ends: at a gap wider than the reach, or at the end of the text */
    if (start >= 0 && last - start + 1 >= TH_TEXT_RUN
        && (double) hits / (double) (last - start + 1) >= TH_TEXT_DENSITY && n < cap) {
      runs[n][0] = start; runs[n][1] = last; n++;
    }
    if (on) { start = last = i; hits = 1; } else { start = last = -1; hits = 0; }
  }
  return n;
}

/** The version-less identifier of a license's family, or -1: the class
 *  whose "unversioned" row lists this one. */
static int thVersionlessOf(thModel *m, int cls)
{
  int i, k;
  if (cls < 0 || !m->unver) return -1;
  for (i = 0; i < m->nClasses; i++) {
    if (!m->unver[i] || i == cls) continue;
    for (k = 0; m->unver[i][k]; k++)
      if (!strcmp(m->unver[i][k], m->classes[cls])) return i;
  }
  return -1;
}

/**
 * \brief Whether the file states this version in words at all, and in
 *        *allLater whether every place it does grants later ones.
 *
 * "under the terms of the GNU Library General Public License" names no
 * version: -or-later is then the convention for a versionless GNU name, not
 * a grant of later versions the words could withhold.
 */
static int thVersionWritten(const char *nt, const char *ver, int *allLater)
{
  const char *p = nt;
  int found = 0;
  size_t vl = strlen(ver);
  if (allLater) *allLater = 1;
  if (!ver[0]) return 0;
  for (; *p; p++) {
    const char *q = p;
    size_t i;
    if (!((p == nt || p[-1] == ' ') &&
          (strncmp(p, "version ", 8) == 0 || strncmp(p, "ver ", 4) == 0
           || strncmp(p, "v", 1) == 0)))
      continue;
    q = p + (strncmp(p, "version", 7) == 0 ? 7 : strncmp(p, "ver", 3) == 0 ? 3 : 1);
    while (*q == ' ') q++;
    /* the version as the normaliser writes it: dots are spaces */
    for (i = 0; i < vl; i++) {
      char want = ver[i] == '.' ? ' ' : ver[i];
      if (q[i] != want && !(want == ' ' && q[i] == '.')) break;
    }
    if (i < vl || (q[vl] != '\0' && q[vl] != ' ')) continue;
    found = 1;
    /* from the version itself, not past it: "LGPLv2.1+" normalises to
       "lgplv2 1 plus", and the wording that reads the plus needs the digit
       it stands on. */
    { char tail[TH_VERSION_REACH + 64];
      size_t left = strlen(q);
      if (left > sizeof(tail) - 1) left = sizeof(tail) - 1;
      if (left > vl + TH_VERSION_REACH) left = vl + TH_VERSION_REACH;
      memcpy(tail, q, left); tail[left] = '\0';
      if (!thLaterWording(tail) && allLater) *allLater = 0; }
  }
  return found;
}

/**
 * \brief Whether every place the file states this version grants later ones.
 *
 * "GPLv2" beside "version 2 of the License, or ... any later version" states
 * it twice and grants once; the notice alone states it once.
 */
static int thAlwaysLater(const char *nt, const char *ver)
{
  int allLater = 1;
  return thVersionWritten(nt, ver, &allLater) && allLater;
}

/**
 * \brief Later versions are granted in words, so a file that writes none
 *        grants none.
 *
 * "under the terms of the GNU Free Documentation License, Version 1.1" is
 * version 1.1, whatever the suffix head made of the notices that usually
 * surround "or any later version". Read over the file, not the window: the
 * words may be in the sentence before the one that scored. And where both
 * suffixes of one version survive, because two windows of the same notice
 * read it two ways, the words decide between them -- only where the file
 * grants later versions every time it states one.
 * \return the number of findings kept
 */
static int thLaterIsWritten(thModel *m, const char *nt, thResult *out, int found)
{
  int a, b, w = 0, later = thLaterWording(nt);
  for (a = 0; a < found; a++) {
    char ba[192], va[64], sa[16];
    int pair = 0;
    if (!out[a].license) { out[w++] = out[a]; continue; }
    thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
    if (!sa[0]) { out[w++] = out[a]; continue; }
    for (b = 0; b < found; b++) {
      char bb[192], vb[64], sb[16];
      if (b == a || !out[b].license) continue;
      thSplit(out[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
      if (sb[0] && strcmp(ba, bb) == 0 && strcmp(va, vb) == 0) pair = 1;
    }
    if (pair) {
      if (strcmp(sa, "or-later") != 0 && thAlwaysLater(nt, va)) continue;
      out[w++] = out[a];
      continue;
    }
    if (!later && strcmp(sa, "or-later") == 0 && out[a].method
        && strstr(out[a].method, "suffix") && thVersionWritten(nt, va, NULL)) {
      char only[192 + 64 + 8];
      int ci;
      if (va[0]) snprintf(only, sizeof(only), "%s-%s-only", ba, va);
      else snprintf(only, sizeof(only), "%s-only", ba);
      ci = thHashGet(m->byName, only, strlen(only));
      if (ci >= 0) {
        out[a].license = m->classes[ci];
        out[a].method = thMethodWith(out[a].method, "only");
      }
    }
    out[w++] = out[a];
  }
  return w;
}

/**
 * \brief A license named but not quoted cannot settle which version it is.
 *
 * The version-less name belongs to the earliest release, and where a later
 * one exists that is a guess; the license's own text decides instead --
 * unless the name the file writes is one version's own, which is the version
 * it states, whichever side of the family the head came down on.
 */
static int thNoVersionFromAName(thModel *m, const char *text, const char *nt,
    thResult *out, int found)
{
      int a, w = 0;
      for (a = 0; a < found; a++) {
        int ci = thHashGet(m->byName, out[a].license, strlen(out[a].license));
        int older = thVersionlessOf(m, ci), k, named = -1, several = 0;
        char b2[192], v2[64], s2[16];
        if (ci < 0) { out[w++] = out[a]; continue; }
        /* a later version claimed on a name that carries no version -- "the
           SAX Public Domain Notice" for SAX-PD-2.0, "PNG Reference Library"
           for libpng-2.0 -- states none, and the version-less identifier is
           what the file wrote */
        thSplit(m->classes[ci], b2, sizeof(b2), v2, sizeof(v2), s2, sizeof(s2));
        if (older >= 0 && v2[0] && !thTextHasVersion(text, v2)
            && !strstr(out[a].method, "version")
            && !thVersionWrittenInName(m, ci, nt)) {
          out[a].license = m->classes[older];
          out[a].method = thMethodWith(out[a].method, "family");
          ci = older;
        }
        if (!m->unver[ci] || thBodyEvidence(m, ci, nt, m->thBodyMin, 0) >= m->thBodyMin) {
          out[w++] = out[a];
          continue;
        }
        /* and the converse: "SAX-PD 2.0" names SAX-PD-2.0 by a name that
           carries the version, so the file did state which. A longer alias
           that carries none ("PNG Reference Library" for libpng-2.0, written
           by every libpng header) states nothing. */
        for (k = 0; m->unver[ci][k]; k++) {
          int cj = thHashGet(m->byName, m->unver[ci][k], strlen(m->unver[ci][k]));
          if (cj < 0) continue;
          if (thVersionWrittenInName(m, cj, nt)) {
            if (named >= 0) several = 1;
            named = cj;
          }
        }
        if (named >= 0 && !several) {
          out[a].license = m->classes[named];
          out[a].method = thMethodWith(out[a].method, "version");
          out[w++] = out[a];
        }
      }
  return w;
}

/** Whether either license stands in the near relation with the other. */
static int thNearPair(thModel *m, int a, int b)
{
  int k;
  if (!m->conNear) return 0;
  if (m->conNear[a])
    for (k = 0; m->conNear[a][k]; k++)
      if (!strcmp(m->conNear[a][k], m->classes[b])) return 1;
  if (m->conNear[b])
    for (k = 0; m->conNear[b][k]; k++)
      if (!strcmp(m->conNear[b][k], m->classes[a])) return 1;
  return 0;
}

/**
 * \brief Whether license \p g's own wording -- phrases few families share
 *        and license \p c lacks -- is written within TH_SUBSUMED_REACH of
 *        the span [lo, hi]. A text under two names has no such wording, and
 *        nothing could tell them apart: true.
 */
static int thOwnWordingNear(thModel *m, int g, int c, const char *nt, size_t lo, size_t hi)
{
  int j, own = 0;
  size_t from = lo > TH_SUBSUMED_REACH ? lo - TH_SUBSUMED_REACH : 0, to = hi + TH_SUBSUMED_REACH;
  for (j = 0; m->bodyPhr[g][j]; j++) {
    const char *ph = m->bodyPhr[g][j], *at;
    int r, theirs = 0;
    if (m->bodyFam && m->bodyFam[g] && m->bodyFam[g][j] > TH_DISTINCT) continue;
    for (r = 0; m->bodyPhr[c][r]; r++) if (!strcmp(m->bodyPhr[c][r], ph)) { theirs = 1; break; }
    if (theirs) continue;
    own = 1;
    at = strstr(nt + from, ph);
    if (at && (size_t) (at - nt) <= to) return 1;
  }
  return !own;
}

/**
 * \brief How much of what tells this license from its siblings is here.
 */
static float thOwnShare(thModel *m, int cls, const char *t)
{
  int k, hit = 0, n = 0;
  if (!m->varPhr || cls < 0 || !m->varPhr[cls]) return 0.0f;
  for (k = 0; m->varPhr[cls][k]; k++) {
    n++;
    if (strstr(t, m->varPhr[cls][k])) hit++;
  }
  return n ? (float) hit / (float) n : 0.0f;
}

/**
 * \brief Tell a variant from what it extends by the text they do not share.
 *
 * A variant extends the generic identifier and shares almost every word, so
 * only the wording one of them lacks separates them. A share, not a count,
 * since a common fragment shows one or two by chance.
 */
static int thPreferVariant(thModel *m, int top, const char *t, int *swapped)
{
  int k, best = top;
  float most = thOwnShare(m, top, t);
  *swapped = 0;
  if (!m->variant || !m->variant[top]) return top;
  if (most < TH_VARIANT_SHARE) most = TH_VARIANT_SHARE;
  for (k = 0; m->variant[top][k] >= 0; k++) {
    float mine = thOwnShare(m, m->variant[top][k], t);
    if (mine > most) { best = m->variant[top][k]; most = mine; }
  }
  if (best != top) *swapped = 1;
  return best;
}

/**
 * \brief A license is claimed only when the text names it or quotes it.
 */
static int thHasEvidence(thModel *m, int cls, const char *raw, const char *t)
{
  int distinct;
  if (!m->nameVar) return 1; /* table absent: old behavior */
  if (thNameEvidence(m, cls, raw, t) >= m->thNameMin) return 1;
  /* An exception needs its own name or wording few families share: most of its
     text is the license it modifies, so plain body evidence over-fires. */
  distinct = m->isException && m->isException[cls];
  return thBodyEvidence(m, cls, t, m->thBodyMin, distinct) >= m->thBodyMin;
}

/**
 * \brief How likely this finding is to be right, as a percentage.
 *
 * A margin alone says nothing about whether the name was written or how strong
 * the winner was; the verifier weighs all of it through an isotonic map, so the
 * number is a probability. Feature order must match `EVIDENCE_NAMES`.
 */
static int thVerify(thModel *m, const char *pick, const char *raw,
                    const char *t, const float *dense, const float *ls,
                    int r0, int r1, int nCredible, const char *method,
                    float margin, float gate, float suffixAgree)
{
  float f[TH_EVID];
  double z = m->verifB, p;
  int i, ci, isExc = 0, present = 0, hasReq = 0;
  char bp[192], vp[64], sp[16], b2[192], v2[64], s2[16];
  size_t ntok = 1;
  const char *c;

  for (c = t; *c; c++) if (*c == ' ') ntok++;
  thSplit(pick, bp, sizeof(bp), vp, sizeof(vp), sp, sizeof(sp));
  for (ci = 0; ci < m->nClasses; ci++)
    if (strcmp(m->classes[ci], pick) == 0) {
      isExc = m->isException && m->isException[ci];
      if (m->reqPhr && m->reqPhr[ci]) {
        int r;
        hasReq = 1;
        for (r = 0; m->reqPhr[ci][r]; r++)
          if (strstr(t, m->reqPhr[ci][r])) { present = 1; break; }
      }
      break;
    }
  thSplit(m->classes[r1 >= 0 ? r1 : 0], b2, sizeof(b2), v2, sizeof(v2),
          s2, sizeof(s2));

  f[0]  = margin;
  f[1]  = gate;
  f[2]  = (float) log1p((double) ntok);
  f[3]  = thNameEvidence(m, ci < m->nClasses ? ci : 0, raw, t);
  f[4]  = (float) thNameSpan(m, ci < m->nClasses ? ci : 0, raw, t);
  f[5]  = (float) log1p((double) thBodyEvidence(m, ci < m->nClasses ? ci : 0,
                                                t, FLT_MAX, 0));
  f[6]  = (float) log1p((double) thBodyEvidence(m, ci < m->nClasses ? ci : 0,
                                                t, FLT_MAX, 1));
  f[7]  = (hasReq && present) ? 1.0f : 0.0f;
  f[8]  = (hasReq && !present) ? 1.0f : 0.0f;
  f[9]  = strcmp(b2, bp) == 0 ? 1.0f : 0.0f;
  f[10] = (float) (nCredible > 5 ? 5 : nCredible);
  f[11] = dense[4];
  f[12] = dense[6];
  f[13] = isExc ? 1.0f : 0.0f;
  f[14] = fabsf(margin - m->thNamedMargin) < 1e-6f ? 1.0f : 0.0f;
  f[15] = strstr(method, "version") ? 1.0f : 0.0f;
  f[16] = fabsf(margin - m->thAloneMargin) < 1e-6f ? 1.0f : 0.0f;
  f[17] = r0 >= 0 ? ls[r0] : 0.0f;
  f[18] = r1 >= 0 ? ls[r1] : 0.0f;
  f[19] = suffixAgree;

  for (i = 0; i < TH_EVID; i++) z += (double) m->verifW[i] * f[i];
  if (z < -30.0) z = -30.0;
  if (z > 30.0) z = 30.0;
  p = 1.0 / (1.0 + exp(-z));
  if (m->nIso > 1) { /* piecewise-linear isotonic map */
    int k;
    if (p <= m->isoX[0]) p = m->isoY[0];
    else if (p >= m->isoX[m->nIso - 1]) p = m->isoY[m->nIso - 1];
    else {
      for (k = 1; k < m->nIso; k++)
        if (p <= m->isoX[k]) {
          double span = m->isoX[k] - m->isoX[k - 1];
          double w = span > 0.0 ? (p - m->isoX[k - 1]) / span : 0.0;
          p = m->isoY[k - 1] + w * (m->isoY[k] - m->isoY[k - 1]);
          break;
        }
    }
  }
  return (int) (p * 100.0 + 0.5);
}

static int thScoreFor(thModel *m, float margin)
{
  int i;
  for (i = 0; i < m->nCal; i++)
    if (margin >= m->calLo[i] && margin < m->calHi[i]) return m->calScore[i];
  return m->nCal ? m->calScore[m->nCal - 1] : 50;
}


/** The name less its version and the words every license name has:
 *  "apache 2 0" and "apache license" are both "apache". */
static void thTechKey(const char *phrase, char *key, size_t cap)
{
  static const char *generic[] = {"license","licence","public","general","version",
      "software","free","open","source","only","later","plus","the","of","and","or",
      "v", NULL};
  size_t k = 0;
  const char *p = phrase;
  key[0] = '\0';
  while (*p) {
    const char *e = p;
    size_t n, i;
    int digits = 1, skip = 0;
    while (*e && *e != ' ') e++;
    n = (size_t) (e - p);
    /* a version token: "2", "v2", "3b" */
    for (i = (n > 1 && p[0] == 'v') ? 1 : 0; i < n; i++)
      if (!isdigit((unsigned char) p[i]) && !(i == n - 1 && i > 0 && isalpha((unsigned char) p[i])))
        { digits = 0; break; }
    if (n == 1 && !isdigit((unsigned char) p[0])) digits = 0;
    for (i = 0; generic[i] && !skip; i++)
      if (strlen(generic[i]) == n && !strncmp(p, generic[i], n)) skip = 1;
    if (n && !digits && !skip && k + n + 2 < cap) {
      if (k) key[k++] = ' ';
      memcpy(key + k, p, n); k += n; key[k] = '\0';
    }
    p = *e ? e + 1 : e;
  }
}

/** Whether \p phrase, present in \p t, sits beside "license": as
 *  "<name> license", or after "license", "licensed under the",
 *  "licensing:". */
static int thNameBesideLicence(const char *phrase, const char *t)
{
  size_t pl = strlen(phrase);
  const char *p;
  if (strstr(phrase, "licen")) return 1;
  for (p = t; (p = strstr(p, phrase)) != NULL; p += pl) {
    const char *after = p + pl, *before;
    if ((p != t && isalnum((unsigned char) p[-1])) || isalnum((unsigned char) *after))
      continue;
    while (*after == ' ') after++;
    if (!strncmp(after, "license", 7) || !strncmp(after, "licence", 7)) return 1;
    /* "license: <name>", "licensed under the <name>" */
    before = p;
    while (before > t && before[-1] == ' ') before--;
    if (before - t >= 4 && !strncmp(before - 4, " the", 4)) { before -= 4;
      while (before > t && before[-1] == ' ') before--; }
    if (before - t >= 6 && !strncmp(before - 6, " under", 6)) { before -= 6;
      while (before > t && before[-1] == ' ') before--; }
    if (before - t >= 3 && !strncmp(before - 3, "ing", 3)) before -= 3;
    else if (before - t >= 2 && !strncmp(before - 2, "ed", 2)) before -= 2;
    else if (before - t >= 1 && before[-1] == 'e') before -= 1;
    if (before - t >= 6 && (!strncmp(before - 6, "licens", 6) || !strncmp(before - 6, "licenc", 6))
        && (before - t == 6 || !isalnum((unsigned char) before[-7]))) return 1;
  }
  return 0;
}

/**
 * \brief A technology-named license claimed on the bare name alone.
 *
 * "import python" and "<?php" are code, "Zend Engine v3" a product and
 * "Markus Kuhn" an author. The name must sit next to "license", not merely
 * near it, since a window straddling a license block puts them a few words
 * apart. A license with such a name is no candidate until the text names it
 * -- by a name of its own, or by the technology's name written as a license.
 */
static int thTechWordOnly(thModel *m, int cls, const char *t)
{
  static const char *tech[] = {"python","php","ruby","perl","zlib","curl","json",
      "unicode","tcl","vim","icu","x11","sqlite","bzip2","openssl","ncurses",
      "freetype","fair","loop","doc",
      /* products, a project, a person: the name is written for them far
         more often than for the license named after them */
      "bitstream vera","zend engine","lzma sdk","info zip","open watcom",
      "markus kuhn","apache","tcl tk","curl haxx se","pcre",
      /* the DocBook DTD, schema and stylesheets are products first */
      "docbook","docbook xml","docbook dtd","docbook schema","docbook stylesheet", NULL};
  char key[128];
  size_t tl = strlen(t);
  int k, g, any = 0;
  if (!m->nameVar || !m->nameVar[cls]) return 0;
  for (k = 0; m->nameVar[cls][k]; k++) {
    const char *phrase = m->nameVar[cls][k];
    size_t pl = strlen(phrase);
    const char *at;
    int isTech = 0, here = 0;
    thTechKey(phrase, key, sizeof(key));
    for (g = 0; tech[g]; g++) if (!strcmp(key, tech[g])) { isTech = 1; break; }
    if (isTech) any = 1;
    for (at = t; (at = strstr(at, phrase)) != NULL; at += pl)
      if ((at == t || at[-1] == ' ') && (at + pl == t + tl || at[pl] == ' ')) { here = 1; break; }
    if (!here) continue;
    if (!isTech) return 0;              /* named by a name of its own */
    if (thNameBesideLicence(phrase, t)) return 0;
    /* the name's own word: "Zend Engine License" is written "the Zend license" */
    { const char *w = phrase, *e;
      char head[64];
      for (;;) {
        size_t n;
        int g2, generic = 0;
        static const char *gen[] = {"license","licence","public","general","version",
            "software","free","open","source","only","later","plus","the","of","and",
            "or","v", NULL};
        e = w; while (*e && *e != ' ') e++;
        n = (size_t) (e - w);
        for (g2 = 0; gen[g2]; g2++) if (strlen(gen[g2]) == n && !strncmp(w, gen[g2], n)) generic = 1;
        if (!generic || !*e) break;
        w = e + 1;
      }
      if ((size_t) (e - w) < sizeof(head) && (size_t) (e - w) < pl) {
        memcpy(head, w, (size_t) (e - w)); head[e - w] = '\0';
        if (thNameBesideLicence(head, t)) return 0;
      } }
  }
  return any;
}

/**
 * \brief A technology-named license resting on the bare word.
 *
 * The guard is about the name, so it has nothing to say about a file that
 * quotes the license: a provenance line rejected the license the file is.
 */
static int thTechWordMirage(thModel *m, int cls, const char *t)
{
  if (!thTechWordOnly(m, cls, t)) return 0;
  return thBodyEvidence(m, cls, t, m->thBodyMin, 0) < m->thBodyMin;
}

/**
 * \brief A license resting on the copyright statement printed above its text.
 *
 * The statement names the holder, and a file under the same holder carries
 * it under any license. Where every phrase of the license found here belongs
 * to its statement and the license is not named, the window quotes the
 * holder, not the license.
 */
static int thStatementMirage(thModel *m, int cls, const char *raw, const char *t)
{
  int k, found = 0;
  if (!m->conStmt || !m->conStmt[cls] || !m->bodyPhr || !m->bodyPhr[cls]) return 0;
  for (k = 0; m->bodyPhr[cls][k]; k++) {
    int s, in = 0;
    if (!strstr(t, m->bodyPhr[cls][k])) continue;
    found = 1;
    for (s = 0; m->conStmt[cls][s]; s++)
      if (strstr(m->conStmt[cls][s], m->bodyPhr[cls][k])) { in = 1; break; }
    if (!in) return 0;
  }
  if (!found) return 0;
  return thNameEvidence(m, cls, raw, t) <= 0.0f;
}

/**
 * \brief thClassify, handing the caller the window's normalised text.
 *
 * The rules that read a window after its classification normalise it again
 * otherwise; with \p norm set the copy is theirs to free.
 */
static int thClassifyN(thModel *m, const char *text, thResult *out, char **norm)
{
  double t0 = thProf.on ? thNow() : 0.0, t1;
  char *t = thNormalise(text);
  thSparse xg = {0}, xl = {0}, xs = {0};
  float dense[TH_DENSE], raw[TH_DENSE];
  float gscore = 0.0f, sscore = 0.0f;
  float *ls = NULL;
  int i, top = -1, second = -1, exc = -1;
  int r0 = -1, r1 = -1, nCredible = 0; /* what the verifier reads */
  float suffixAgree = 0.0f;
  static char pick[512];
  const char *method = "ml";

  memset(out, 0, sizeof(*out));
  out->status = "UNKNOWN";
  if (!t) return -1;
  if (thProf.on) { t1 = thNow(); thProf.normalise += t1 - t0; t0 = t1; thProf.windows++; }

  /* A file states a license by naming one or granting one; text that does
     neither is not a license, whatever its vocabulary resembles. Most windows
     stop here, so the features the rest of them need are not computed yet. */
  if (!thDenseEx(m, t, raw, 1)) {
    if (thProf.on) thProf.gateDense += thNow() - t0;
    out->grant = raw[6];
    out->method = "no-evidence";
    if (norm) *norm = t; else free(t);
    return 0;
  }
  if (thProf.on) { t1 = thNow(); thProf.gateDense += t1 - t0; t0 = t1; }
  out->grant = raw[6];
  if (raw[2] <= 0.0f && raw[6] <= 0.0f) {
    out->method = "no-evidence";
    if (norm) *norm = t; else free(t);
    return 0;
  }
  for (i = 0; i < TH_DENSE; i++)
    dense[i] = m->scScale[i] != 0.0f ? (raw[i] - m->scMean[i]) / m->scScale[i] : 0.0f;

  if (thVectorise(m->gateBlk, m->nGateBlk, t, &xg)) goto done;
  thScore(&m->gate, &xg, dense, TH_DENSE, &gscore);
  out->gate = gscore;
  if (thProf.on) { t1 = thNow(); thProf.gateHead += t1 - t0; t0 = t1; }
  if (gscore < m->thGate) { method = "gate-reject"; out->method = method; goto done; }

  if (thVectorise(m->licBlk, m->nLicBlk, t, &xl)) goto done;
  if (thProf.on) { t1 = thNow(); thProf.licVec += t1 - t0; t0 = t1; }
  ls = malloc((size_t) m->lic.rows * sizeof(float));
  if (!ls) goto done;
  thScore(&m->lic, &xl, NULL, 0, ls);
  if (thProf.on) { t1 = thNow(); thProf.licScore += t1 - t0; t0 = t1; }
  { int rank[TH_CAND_MAX + 3], nr = m->thCandidates + 3, q, r2, w;
    for (q = 0; q < TH_CAND_MAX + 3; q++) rank[q] = -1;
    for (i = 0; i < m->lic.rows; i++) {
      /* One license and one modifier: letting the modifier win the argmax
         reports it and loses the license. */
      if (m->isException && m->isException[i]) {
        if (exc < 0 || ls[i] > ls[exc]) exc = i;
        continue;
      }
      for (q = 0; q < nr; q++) {
        if (rank[q] < 0 || ls[i] > ls[rank[q]]) {
          for (r2 = nr - 1; r2 > q; r2--) rank[r2] = rank[r2 - 1];
          rank[q] = i;
          break;
        }
      }
    }
    /* A technology-named license without its context is a coincidence of
       vocabulary, and as pick or rival it wrecks the window. Drop it first. */
    /* The verifier reads the top two of the unfiltered ranking, so they are
       taken before the field is narrowed. */
    r0 = rank[0];
    r1 = rank[1] >= 0 ? rank[1] : rank[0];
    for (q = 0, w = 0; q < nr && rank[q] >= 0; q++)
      if (!thTechWordMirage(m, rank[q], t) && !thStatementMirage(m, rank[q], text, t))
        rank[w++] = rank[q];
    if (w == 0 && rank[0] >= 0) w = 1; /* never empty the field */
    for (q = w; q < nr; q++) rank[q] = -1;
    /* A license whose conditions the window rules out is not a candidate
       either, and yields only to a member of its own family the conditions
       allow, never to an unrelated license: the same family-restricted
       retry the whole-file pass makes, made where the window is scored. The
       ruled-out members leave the field, so the margin is measured against
       what remains. */
    if (m->clauseSig && m->nClause > 0 && rank[0] >= 0 && m->clauseSig[rank[0]]) {
      char here[TH_CLAUSE_MAX + 1];
      int nHere = thClausesHere(m, t, here);
      if (nHere > 0 && thBeyondSignature(m, rank[0], text, t, here, nHere)) {
        const char *sig = m->clauseSig[rank[0]], *colon = strchr(sig, ':');
        size_t fl = colon ? (size_t) (colon - sig) : 0;
        int kin[TH_CAND_MAX + 3], nk = 0, rest[TH_CAND_MAX + 3], nrest = 0;
        int fi;
        for (fi = 0; fi < m->nFamily; fi++)
          if (strlen(m->famName[fi]) == fl && strncmp(m->famName[fi], sig, fl) == 0) break;
        for (q = 0; q < w; q++) {
          const char *s2 = m->clauseSig[rank[q]], *c2 = s2 ? strchr(s2, ':') : NULL;
          int same = s2 && c2 && (size_t) (c2 - s2) == fl && strncmp(s2, sig, fl) == 0;
          /* a member SPDX has no text for has no signature, and cannot be
             checked against what is written */
          if (!s2 && fi < m->nFamily && m->famMemberOk[fi]
              && regexec(&m->famMember[fi], m->classes[rank[q]], 0, NULL, 0) == 0)
            continue;
          if (same && !thBeyondSignature(m, rank[q], text, t, here, nHere)) kin[nk++] = rank[q];
          else if (!same) rest[nrest++] = rank[q];
        }
        if (nk > 0) {
          for (q = 0; q < nk; q++) rank[q] = kin[q];
          for (q = 0; q < nrest && nk + q < nr; q++) rank[nk + q] = rest[q];
          w = nk + (nrest < nr - nk ? nrest : nr - nk);
          for (q = w; q < nr; q++) rank[q] = -1;
        }
      }
    }
    nr = m->thCandidates + 1;

    /* An exception is claimed on the same terms as a license: named or quoted.
       Its head scores are all negative, so a floor of zero vetoed every one. */
    if (exc >= 0 && ls[exc] >= m->thException
        && thHasEvidence(m, exc, text, t)) {
      out->exception = m->classes[exc];
      out->excScore = ls[exc];
    }
    if (rank[0] < 0) { method = "no-candidate"; out->method = method; goto done; }
    second = rank[1] >= 0 ? rank[1] : rank[0];

    /* Evidence decides whether a license is here, the margin which of the
       credible ones it is. An unnamed, unquoted class does not compete. */
    top = -1;
    for (q = 0; q < nr - 1 && rank[q] >= 0; q++)
      if (thHasEvidence(m, rank[q], text, t)) {
        if (top < 0) top = rank[q];
        nCredible++;
      }
    if (top < 0) {
      out->margin = 0.0f;
      out->alt = m->classes[rank[0]];
      method = "no-evidence-for-pick"; out->method = method; goto done;
    }
    /* A rival the text names less completely is not a serious alternative;
       letting it set the margin understates what the text was explicit about. */
    { int rival = -1, held = thNameSpan(m, top, text, t);
      char bt[192], vt[64], st[16], b2[192], v2[64], s2[16];
      thSplit(m->classes[top], bt, sizeof(bt), vt, sizeof(vt), st, sizeof(st));
      for (q = 0; q < nr - 1 && rank[q] >= 0; q++) {
        if (rank[q] == top) continue;
        thSplit(m->classes[rank[q]], b2, sizeof(b2), v2, sizeof(v2), s2, sizeof(s2));
        if (strcmp(bt, b2) == 0) continue;
        if (thNameSpan(m, rank[q], text, t) < held) continue;
        rival = rank[q]; break;
      }
      if (rival >= 0) { second = rival; out->margin = ls[top] - ls[rival]; }
      else if (top == rank[0]) {
        /* Only when the text names the pick more fully than the runner-up;
           everywhere else the ordinary margin stands. */
        if (held > 0 && thNameSpan(m, second, text, t) < held)
          out->margin = m->thNamedMargin;
        else out->margin = ls[top] - ls[second];
      }
      else out->margin = m->thAloneMargin;
    }
    out->alt = m->classes[second];
    out->altMargin = out->margin;
  }
  snprintf(pick, sizeof(pick), "%s", m->classes[top]);
  if (out->margin < m->thMargin) { method = "low-margin"; out->method = method; goto done; }

  /* suffix head: only / or-later */
  {
    char base[192], ver[64], suf[16];
    thSplit(pick, base, sizeof(base), ver, sizeof(ver), suf, sizeof(suf));
    if (suf[0]) {
      if (thVectorise(m->sfxBlk, m->nSfxBlk, t, &xs) == 0) {
        thScore(&m->sfx, &xs, NULL, 0, &sscore);
        suffixAgree = strcmp(suf, "or-later") == 0 ? sscore : -sscore;
        if (fabsf(sscore) >= m->thSuffix) {
          const char *want = sscore >= 0.0f ? "or-later" : "only";
          if (strcmp(want, suf) != 0) {
            if (ver[0]) snprintf(pick, sizeof(pick), "%s-%s-%s", base, ver, want);
            else snprintf(pick, sizeof(pick), "%s-%s", base, want);
            method = "ml+suffix";
          }
        }
      }
    }
  }

  /* The stated version is read whatever the margin: the margin measures
     the family against outsiders and says nothing about which member --
     the head is as sure of GPL-2.0-or-later on "either version 3 of the
     License" as on any notice, and the text is what decides. */
  {
    char base[192], ver[64], suf[16];
    int hits[TH_VER_HITS], hit = -1, nHit = 0;
    thSplit(pick, base, sizeof(base), ver, sizeof(ver), suf, sizeof(suf));
    for (i = 0; i < m->nClasses; i++) {
      char b2[192], v2[64], s2[16];
      thSplit(m->classes[i], b2, sizeof(b2), v2, sizeof(v2), s2, sizeof(s2));
      /* SPDX spells a family's first release and its later ones differently
         where the identifier is a project's name: "Libpng" and "libpng-2.0"
         are one family. */
      if (strcasecmp(b2, base) || !v2[0]) continue;
      if (thTextHasVersion(text, v2) && nHit < TH_VER_HITS) hits[nHit++] = i;
    }
    /* The stated version leaves both -only and -or-later, so keep whichever
       agrees with the suffix head. */
    if (nHit > 1 && suf[0]) {
      int k, kept = 0;
      for (k = 0; k < nHit; k++) {
        char b2[192], v2[64], s2[16];
        thSplit(m->classes[hits[k]], b2, sizeof(b2), v2, sizeof(v2), s2, sizeof(s2));
        if (strcmp(s2, suf) == 0) hits[kept++] = hits[k];
      }
      if (kept) nHit = kept;
    }
    hit = nHit ? hits[0] : -1;
    if (nHit == 1) {
      if (strcmp(m->classes[hit], pick) != 0) {
        char b2[192], v2[64], s2[16];
        thSplit(m->classes[hit], b2, sizeof(b2), v2, sizeof(v2), s2, sizeof(s2));
        if (s2[0] || !suf[0]) snprintf(pick, sizeof(pick), "%s", m->classes[hit]);
        else snprintf(pick, sizeof(pick), "%s-%s", m->classes[hit], suf);
        method = thMethodWith(method, "version");
      }
      /* The stated version rules the siblings out, so measuring confidence
         against one of them understates a version the text gave. */
      { int bestOther = -1;
        for (i = 0; i < m->lic.rows; i++) {
          char b2[192], v2[64], s2[16];
          if (i == top || (m->isException && m->isException[i])) continue;
          thSplit(m->classes[i], b2, sizeof(b2), v2, sizeof(v2), s2, sizeof(s2));
          if (strcmp(b2, base) == 0) continue; /* a sibling, now excluded */
          if (bestOther < 0 || ls[i] > ls[bestOther]) bestOther = i;
        }
        if (bestOther >= 0) {
          if (ls[top] - ls[bestOther] > out->margin)
            out->margin = ls[top] - ls[bestOther];
          method = thMethodWith(method, "resolved");
        }
      }
    }
  }

  /* The suffix and version heads build `pick` by concatenation; an undeclared
     name would be minted at scan time as a row with no text. */
  if (thHashGet(m->byName, pick, strlen(pick)) < 0) {
    snprintf(pick, sizeof(pick), "%s", m->classes[top]);
    method = "ml";
  }
  out->license = pick;
  out->score = m->haveVerifier
      ? thVerify(m, pick, text, t, raw, ls, r0, r1, nCredible, method,
                 out->margin, out->gate, suffixAgree)
      : thScoreFor(m, out->margin);
  out->status = out->score >= 90 ? "HIGH" : (out->score >= 70 ? "MEDIUM" : "LOW");
  out->method = method;
  /* Which member of the family, decided last and on its own evidence; the
     measured confidence in the family is left alone. */
  { int q, at = -1;
    for (q = 0; q < m->nClasses; q++)
      if (strcmp(m->classes[q], pick) == 0) { at = q; break; }
    if (at >= 0) {
      int swapped = 0, into = thPreferVariant(m, at, t, &swapped);
      if (swapped) {
        snprintf(pick, sizeof(pick), "%s", m->classes[into]);
        out->license = pick;
        out->method = method = "ml+variant";
      }
    } }
  if (out->score < m->minScore) {
    out->license = NULL;
    out->status = "UNKNOWN";
    out->method = "below-min-score";
  }

done:
  if (thProf.on) thProf.evidence += thNow() - t0;
  if (!out->method) out->method = method;
  free(ls);
  if (norm) *norm = t; else free(t);
  free(xg.idx); free(xg.val);
  free(xl.idx); free(xl.val);
  free(xs.idx); free(xs.val);
  return 0;
}

int thClassify(thModel *m, const char *text, thResult *out)
{
  return thClassifyN(m, text, out, NULL);
}

/**
 * \brief Window length with surrounding whitespace removed.
 *
 * The floor asks whether a window holds enough text to be a notice. A span of
 * blank lines and indentation is not, however many bytes it covers, and the
 * reference implementation measures the stripped string.
 */
static size_t thStrippedLen(const char *p, size_t len)
{
  size_t a = 0, b = len;
  while (a < b && isspace((unsigned char) p[a])) a++;
  while (b > a && isspace((unsigned char) p[b - 1])) b--;
  return b - a;
}

#define TH_LONG_LINE 400 /**< a line longer than this is a serialised file */
#define TH_LINE_PIECE 80
#define TH_LINE_PIECE_SLACK 20

/**
 * \brief Where each line of `text` begins, a long line broken into pieces.
 *
 * A line longer than TH_LONG_LINE is not a line but a serialised file --
 * minified code, a source map -- and is read in pieces the size of real
 * lines, cut at the last blank in the piece's final stretch where there is
 * one. Universal newlines, as the reference reads a file: a lone carriage
 * return ends a line too. Pieces are cut only where the file has a long line,
 * and then on "\n" alone, as the reference does.
 *
 * \return the starts, `*n` of them, to free; or NULL
 */
static const char **thLineStarts(const char *text, size_t *n)
{
  size_t cap = 1024, len = strlen(text), at, longChars = 0;
  const char **line = malloc(cap * sizeof(*line));
  const char *p;
  if (!line) return NULL;
  *n = 0;
  line[(*n)++] = text;
  for (p = text; *p; p++)
    if (*p == '\n' || (*p == '\r' && p[1] != '\n')) {
      if (*n == cap) {
        const char **t = thGrow(line, (cap *= 2) * sizeof(*line));
        if (!t) return NULL; /* thGrow freed it */
        line = t;
      }
      line[(*n)++] = p + 1;
    }
  /* Only a file that is mostly such lines is read in pieces: pieces shift
     the window grid for everything after them, and a configure script of
     forty thousand real lines with one long one among them lost the notice
     the grid had landed on before. */
  for (at = 0; at < len; ) {
    const char *nl = memchr(text + at, '\n', len - at);
    size_t end = nl ? (size_t) (nl - text) + 1 : len;
    if (end - at > TH_LONG_LINE) longChars += end - at;
    at = end;
  }
  if (longChars * 2 <= len) return line;
  /* rebuild, on "\n" alone, with the pieces */
  *n = 0;
  line[(*n)++] = text;
  for (at = 0; at < len; ) {
    const char *nl = memchr(text + at, '\n', len - at);
    size_t end = nl ? (size_t) (nl - text) + 1 : len;
    if (end - at > TH_LONG_LINE) {
      size_t piece = at;
      while (end - piece > TH_LINE_PIECE) {
        size_t cut = piece + TH_LINE_PIECE, k, blank = 0;
        for (k = cut; k > cut - TH_LINE_PIECE_SLACK; k--)
          if (text[k - 1] == ' ' || text[k - 1] == '\t') { blank = k; break; }
        if (blank > piece) cut = blank;
        if (*n == cap) {
          const char **t = thGrow(line, (cap *= 2) * sizeof(*line));
          if (!t) return NULL; /* thGrow freed it */
          line = t;
        }
        line[(*n)++] = text + cut;
        piece = cut;
      }
    }
    if (end < len) {
      if (*n == cap) {
        const char **t = thGrow(line, (cap *= 2) * sizeof(*line));
        if (!t) return NULL; /* thGrow freed it */
        line = t;
      }
      line[(*n)++] = text + end;
    }
    at = end;
  }
  return line;
}

#define TH_WIN_LINES 12 /**< about the size of a real license header */
#define TH_WIN_STEP   6

int thClassifyFile(thModel *m, const char *text, thResult *out,
    char *buf, size_t bufLen)
{
  const char **line;
  size_t n = 0, i;
  thResult best;
  double bestScore = -1e30;
  int found = 0;

  memset(out, 0, sizeof(*out));
  out->status = "UNKNOWN";
  out->method = "no-window";
  if (buf && bufLen) buf[0] = '\0';

  line = thLineStarts(text, &n);
  if (!line) return -1;

  if (n <= TH_WIN_LINES) {
    free(line);
    { int rc = thClassify(m, text, out);
      if (out->license && buf && bufLen) {
        snprintf(buf, bufLen, "%s", out->license);
        out->license = buf;
      }
      return rc; }
  }

  memset(&best, 0, sizeof(best));
  for (i = 0; i < n; i += TH_WIN_STEP) {
    size_t end = i + TH_WIN_LINES < n ? i + TH_WIN_LINES : n;
    size_t len = (end < n ? (size_t) (line[end] - line[i]) : strlen(line[i]));
    char *w;
    thResult r;
    if (len < 40) continue;
    w = malloc(len + 1);
    if (!w) break;
    memcpy(w, line[i], len); w[len] = '\0';
    if (thClassify(m, w, &r) == 0 && r.license) {
      /* the weakest of the two gates is what the decision really rested on */
      double sc = r.gate < r.margin ? r.gate : r.margin;
      if (sc > bestScore) {
        bestScore = sc; best = r; found = 1;
        if (buf && bufLen) snprintf(buf, bufLen, "%s", r.license);
      }
    } else if (!found && r.gate > out->gate) {
      out->gate = r.gate; /* report the best gate we did see */
    }
    free(w);
  }
  free(line);
  if (found) {
    *out = best;
    out->license = (buf && bufLen) ? buf : NULL;
    /* the status its score earned, not an assumption: this is a public entry
       point and the field is reported */
    out->status = out->score >= 90 ? "HIGH"
                : (out->score >= 70 ? "MEDIUM" : "LOW");
  }
  return 0;
}


/** Is any distinctive 1-3 word run of `name` present in the normalized text? */
/**
 * \brief Where a file says a license applies without saying which one.
 *
 * Not identifications: they say a reviewer has somewhere to look. A file under
 * "a BSD-style license in the LICENSE file" is not BSD-3-Clause.
 */
/** Whether \p name is a family with versioned members: GPL, LGPL, AGPL. */
static int thVersionedFamily(thModel *m, const char *name)
{
  int i;
  size_t L = strlen(name);
  for (i = 0; i < m->nClasses; i++) {
    const char *c = m->classes[i];
    char b[192], v[64], sf[16];
    if (strncmp(c, name, L) || c[L] != '-') continue;
    thSplit(c, b, sizeof(b), v, sizeof(v), sf, sizeof(sf));
    if (v[0] && !strcmp(b, name)) return 1;
  }
  return 0;
}

/**
 * \brief A version stated beside a licence name: "version 2", "v2", "2 or
 *        later", "2 0 of the license" -- read after the name, within a
 *        sentence's reach.
 */
static int thVersionStated(const char *after)
{
  static regex_t re;
  static int ready = 0;
  char tail[TH_VERSION_REACH + 1];
  size_t n = strlen(after);
  if (!ready) {
    if (regcomp(&re, "\\b(version|v|ver) ?[0-9]|\\b[0-9]( [0-9])? (or|of the)\\b",
                REG_EXTENDED | REG_NOSUB) != 0) return 0;
    ready = 1;
  }
  if (n > TH_VERSION_REACH) n = TH_VERSION_REACH;
  memcpy(tail, after, n); tail[n] = '\0';
  return regexec(&re, tail, 0, NULL, 0) == 0;
}

/**
 * \brief The member a grant row's family is granted at.
 *
 * A row that names a family steps aside where the file states a version,
 * since the family is not the answer once the text says which release. Where
 * that version names one member of the same family, the member is what the
 * file granted.
 */
static const char *thGrantedAtVersion(thModel *m, const char *name, const char *after)
{
  char base[192], ver[64], sfx[16], near[TH_VERSION_REACH + 1];
  const char *hit = NULL;
  size_t n = strlen(after);
  int i, later;
  thSplit(name, base, sizeof(base), ver, sizeof(ver), sfx, sizeof(sfx));
  if (n > TH_VERSION_REACH) n = TH_VERSION_REACH;
  memcpy(near, after, n); near[n] = '\0';
  later = thLaterWording(near);
  for (i = 0; i < m->nClasses; i++) {
    char b[192], v[64], sf[16];
    thSplit(m->classes[i], b, sizeof(b), v, sizeof(v), sf, sizeof(sf));
    if (!v[0] || strcasecmp(b, base) != 0) continue;
    if (strcmp(sf, later ? "or-later" : "only") != 0) continue;
    if (!thTextHasVersion(near, v)) continue;
    if (hit) return NULL;            /* two members answer to it: unsettled */
    hit = m->classes[i];
  }
  return hit;
}

static int thReferential(thModel *m, const char *text, thResult *out, int maxOut,
    char *buf, size_t bufLen, size_t *used)
{
  /* nomos's order: the most specific pointer wins, and a family reference is
     reported alongside it because the two say different things. */
  static const char *see[] = {"Same-license-as", "See-file.LICENSE", "See-file.COPYING",
      "See-file.README", "See-URL", "See-file", "See-doc.OTHER", NULL};
  char *t, *flat;
  const char *picked[3];
  int i, k, nPicked = 0, found = 0, seeRank = -1;
  const char *family = NULL, *dual = NULL, *pointer = NULL;
  int unclassified = 0;

  if (!m->nRef || maxOut <= 0) return 0;
  t = thNormalise(text);
  if (!t) return 0;

  flat = thFlatten(text);
  for (i = 0; i < m->nRef; i++) {
    const char *name = m->refName[i];
    regmatch_t hit;
    int isSee = 0;
    /* an identification by declaration, read elsewhere */
    if (m->refWhere && m->refWhere[i] >= 4) continue;
    /* A statement the work makes about itself -- "this file has been put
       into the public domain" -- is the licensing, and needs no license word
       beside it. */
    if (m->refWhere && m->refWhere[i] == 3) {
      if (thRegexec(((regex_t *) m->refRe) + i, m->refLit[i], 0, t, 1, &hit) != 0) continue;
      goto matched;
    }
    /* A compound or a field is punctuation someone wrote, so it must be in the
       file as written; normalizing it invents matches. */
    if (m->refWhere && m->refWhere[i]) {
      const char *in = text;
      if (thRegexec(((regex_t *) m->refRe) + i, m->refLit[i], 1, text, 1, &hit) != 0) {
        in = flat;
        if (!flat || thRegexec(((regex_t *) m->refRe) + i, m->refLit[i], 1, flat, 1, &hit) != 0)
          continue;
      }
      if (m->refWhere[i] == 2) {
        if (!thLicenceContext(in, (size_t) hit.rm_so, (size_t) hit.rm_eo))
          continue;
        goto matched;
      }
    }
    if (thRegexec(((regex_t *) m->refRe) + i, m->refLit[i], 0, t, 1, &hit) != 0) continue;
    /* A cross-reference is not a license pointer; standing alone these
       patterns need a license word beside them. */
    if (!thLicenceContext(t, (size_t) hit.rm_so, (size_t) hit.rm_eo)) continue;
    /* A row that names a version, or a family that has versions, yields to
       a version the file states: "distributed under the GNU General Public
       License, version 2, or any higher version" is not GPL-1.0-or-later,
       whatever the head made of its wording, and not the family either. */
    if ((strpbrk(name, "0123456789") || thVersionedFamily(m, name))
        && thVersionStated(t + hit.rm_eo)) {
      const char *member = thGrantedAtVersion(m, name, t + hit.rm_eo);
      if (!member) continue;
      name = member;
    }
matched:
    for (k = 0; see[k]; k++)
      if (strcmp(name, see[k]) == 0) { isSee = 1; break; }
    if (isSee) {
      /* Rank by the list above, not by table order: the more specific
         pointer is the one to report. */
      if (seeRank < 0 || k < seeRank) seeRank = k;
    } else if (strcmp(name, "UnclassifiedLicense") == 0) {
      unclassified = 1;
    } else if (strcmp(name, "Dual-license") == 0) {
      if (!dual) dual = name;
    } else if (!family) {
      family = name;
    }
  }
  free(t); free(flat);

  if (seeRank >= 0) pointer = see[seeRank];
  if (family) picked[nPicked++] = family;
  if (pointer) picked[nPicked++] = pointer;
  if (dual && nPicked < 3) picked[nPicked++] = dual;
  if (!nPicked && unclassified) picked[nPicked++] = "UnclassifiedLicense";

  for (i = 0; i < nPicked && found < maxOut; i++) {
    size_t need = strlen(picked[i]) + 1;
    if (!buf || *used + need > bufLen) break;
    memcpy(buf + *used, picked[i], need);
    memset(out + found, 0, sizeof(out[0]));
    out[found].license = buf + *used;
    out[found].status = "LOW";
    /* certain as a pointer, uninformative as an identification, so it carries
       no match percentage -- the same thing nomos records for these names */
    out[found].score = 0;
    out[found].method = "referential";
    *used += need;
    found++;
  }
  return found;
}


/* ---------- SPDX-License-Identifier ----------
   A tag is written by the file's author, so it is read rather than weighed:
   left to the classifier it is outweighed by the code beside it. */

/** The expression an SPDX tag introduces on this line, or NULL. */
static const char *thSpdxAt(const char *p)
{
  static const char lead[] = "/*#;!%+-<>~\\|'\".";
  /* comment leaders with blanks between them: "- * " is a removed line of
     a patch, "# #" a nested one */
  for (;;) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p && strchr(lead, *p)) { p++; continue; }
    if (!strncasecmp(p, "dnl", 3)) { p += 3; continue; }
    if (!strncasecmp(p, "rem", 3)) { p += 3; continue; }
    break;
  }
  if (strncasecmp(p, "spdx-licen", 10)) return NULL;
  p += 10;
  if (!strncasecmp(p, "se", 2) || !strncasecmp(p, "ce", 2)) p += 2;
  else return NULL;
  if (!strncasecmp(p, "id", 2)) p += 2;
  else if ((*p == '-' || *p == ' ') && !strncasecmp(p + 1, "identifier", 10))
    p += 11;
  else return NULL;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != ':') return NULL;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

/**
 * \brief A written identifier as this model names it, or -1.
 *
 * SPDX's own rewrites: case is not significant, "+" is "-or-later", and a bare
 * deprecated id is the "-only" it was split into.
 */
static int thSpdxResolve(thModel *m, const char *tok, size_t len)
{
  char buf[192], alt[224];
  size_t i = 0, j;
  int k;
  while (len && (tok[0] == ' ' || tok[0] == '\t')) { tok++; len--; }
  while (len && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) len--;
  while (len && strchr(".,;", tok[0])) { tok++; len--; }
  while (len && strchr(".,;", tok[len - 1])) len--;
  if (!len || len >= sizeof(buf)) return -1;
  for (j = 0; j < len; j++) buf[i++] = (char) tolower((unsigned char) tok[j]);
  buf[i] = '\0';
  if (!strcmp(buf, "none") || !strcmp(buf, "noassertion")) return -1;
  if (!strncmp(buf, "licenseref-", 11) || !strncmp(buf, "documentref-", 12))
    return -1;
  for (k = 0; k < m->nClasses; k++)
    if (!strcasecmp(m->classes[k], buf)) return k;
  if (buf[i - 1] == '+') {
    snprintf(alt, sizeof(alt), "%.*s-or-later", (int) (i - 1), buf);
    for (k = 0; k < m->nClasses; k++)
      if (!strcasecmp(m->classes[k], alt)) return k;
  }
  snprintf(alt, sizeof(alt), "%s-only", buf);
  for (k = 0; k < m->nClasses; k++)
    if (!strcasecmp(m->classes[k], alt)) return k;
  return -1;
}

/**
 * \brief The licenses an SPDX tag declares, in the order they are written.
 *
 * "WITH" attaches an exception to the license before it. "OR" and "AND" both
 * yield every license named, because license_file has no operator to tell a
 * choice from a conjunction.
 */
/**
 * \brief LibreJS's declaration on this line: "@license magnet:?xt=urn:btih:
 *        <hash>&dn=gpl-2.0.txt", the licence named by the text file the
 *        magnet points at. As much the author's statement as an SPDX tag,
 *        and written into generated HTML by doxygen.
 * \return the identifier's start, with its length in *len, or NULL
 */
static const char *thLibreJSAt(const char *line, const char *eol, size_t *len)
{
  const char *p = line;
  while ((p = strstr(p, "@license")) != NULL && p < eol) {
    const char *q = p + 8, *id;
    int k;
    if (*q != ' ' && *q != '\t') { p = q; continue; }
    while (*q == ' ' || *q == '\t') q++;
    if (strncasecmp(q, "magnet:?xt=urn:btih:", 20) != 0) { p = q; continue; }
    q += 20;
    for (k = 0; k < 40 && isxdigit((unsigned char) q[k]); k++) ;
    if (k < 40 || q[40] != '&') { p = q; continue; }
    q += 41;
    if (strncmp(q, "amp;", 4) == 0) q += 4;
    if (strncmp(q, "dn=", 3) != 0) { p = q; continue; }
    id = q + 3;
    for (q = id; isalnum((unsigned char) *q) || *q == '.' || *q == '+' || *q == '-'; q++) ;
    if (q - id > 4 && strncasecmp(q - 4, ".txt", 4) == 0 && q <= eol) {
      *len = (size_t) (q - 4 - id);
      return id;
    }
    p = q;
  }
  return NULL;
}

static int thSpdxTags(thModel *m, const char *text, thResult *out, int maxOut)
{
  const char *line = text;
  int found = 0;
  while (line && found < maxOut) {
    const char *expr = thSpdxAt(line), *eol, *stop, *p;
    const char *nl = strchr(line, '\n');
    if (expr) {
      int last = -1, op = 0, items = 0; /* op: 1 OR/AND, 2 WITH */
      eol = nl ? nl : expr + strlen(expr);
      for (stop = expr; stop + 1 < eol; stop++)
        if ((stop[0] == '*' && (stop[1] == '/' || stop[1] == ')')) ||
            (stop[0] == '-' && stop + 2 < eol && stop[1] == '-' && stop[2] == '>') ||
            (stop[0] == '}' && stop[1] == '}')) break;
      if (stop + 1 >= eol) stop = eol;
      /* A sentence about tags is not a tag, and counting first means a long
         one contributes nothing rather than its first few words. */
      for (p = expr; p < stop; ) {
        while (p < stop && strchr("() \t\r", *p)) p++;
        if (p < stop) items++;
        while (p < stop && !strchr("() \t\r", *p)) p++;
      }
      if (items > 12) { line = nl ? nl + 1 : NULL; continue; }
      for (p = expr; p < stop; ) {
        const char *q = p;
        while (q < stop && !strchr("() \t\r", *q)) q++;
        if (q > p) {
          size_t len = (size_t) (q - p);
          int ci;
          if (len == 2 && !strncasecmp(p, "or", 2)) { op = 1; p = q; continue; }
          if (len == 3 && !strncasecmp(p, "and", 3)) { op = 1; p = q; continue; }
          if (len == 4 && !strncasecmp(p, "with", 4)) { op = 2; p = q; continue; }
          ci = thSpdxResolve(m, p, len);
          if (ci >= 0 && m->isException && m->isException[ci]) {
            if (op == 2 && last >= 0) {
              int k;
              for (k = 0; k < found; k++)
                if (out[k].license == m->classes[last])
                  out[k].exception = m->classes[ci];
            }
          } else if (ci >= 0) {
            int k, seen = 0;
            for (k = 0; k < found; k++)
              if (out[k].license == m->classes[ci]) { seen = 1; break; }
            if (!seen && found < maxOut) {
              memset(out + found, 0, sizeof(out[0]));
              out[found].license = m->classes[ci];
              out[found].status = "HIGH";
              out[found].method = "spdx-tag";
              out[found].start = (long) (line - text);
              out[found].len = (long) (stop - line);
              found++;
            }
            last = ci;
          }
          op = 0;
        }
        p = q;
        while (p < stop && strchr("() \t\r", *p)) p++;
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  /* after the tags, as the reference reads them: LibreJS's declarations */
  for (line = text; line && found < maxOut; ) {
    const char *nl = strchr(line, '\n'), *eol = nl ? nl : line + strlen(line);
    size_t idLen;
    const char *id = thLibreJSAt(line, eol, &idLen);
    if (id) {
      int ci = thSpdxResolve(m, id, idLen);
      if (ci >= 0 && !(m->isException && m->isException[ci])) {
        int k, seen = 0;
        for (k = 0; k < found; k++)
          if (out[k].license == m->classes[ci]) { seen = 1; break; }
        if (!seen) {
          memset(out + found, 0, sizeof(out[0]));
          out[found].license = m->classes[ci];
          out[found].status = "HIGH";
          out[found].method = "spdx-tag";
          out[found].start = (long) (line - text);
          out[found].len = (long) (eol - line);
          found++;
        }
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  /* A file declaring licenses by the dozen is a catalog of expressions, not a
     file under all of them. */
  if (found > TH_TAG_MAX) found = 0;
  return found;
}


/** The licences an SPDX expression names, appended to `out` as declarations. */
static void thReadExpression(thModel *m, const char *expr, const char *stop,
    thResult *out, int *found, int maxOut, const char *method,
    long start, long len)
{
  const char *p;
  int last = -1, op = 0, items = 0;
  for (p = expr; p < stop; ) {
    while (p < stop && strchr("() \t\r", *p)) p++;
    if (p < stop) items++;
    while (p < stop && !strchr("() \t\r", *p)) p++;
  }
  if (items > 12) return;             /* a sentence about licences, not one */
  /* A value with no operator is one name however it is spaced: a manifest
     writes "GPL 2.0" where SPDX writes GPL-2.0, and split on the space
     neither half is a licence. */
  { int hasOp = 0, ci;
    char joined[128]; size_t n = 0;
    for (p = expr; p < stop && n < sizeof(joined) - 1; ) {
      const char *q = p;
      while (p < stop && strchr("() \t\r", *p)) p++;
      q = p;
      while (q < stop && !strchr("() \t\r", *q)) q++;
      if (q > p) {
        size_t l = (size_t) (q - p);
        if ((l == 2 && !strncasecmp(p, "or", 2)) || (l == 3 && !strncasecmp(p, "and", 3))
            || (l == 4 && !strncasecmp(p, "with", 4))) { hasOp = 1; break; }
        if (n && n < sizeof(joined) - 1) joined[n++] = '-';
        if (l > sizeof(joined) - 1 - n) { hasOp = 1; break; }
        memcpy(joined + n, p, l); n += l;
      }
      p = q;
    }
    joined[n] = 0;
    if (!hasOp && n && (ci = thSpdxResolve(m, joined, n)) >= 0
        && !(m->isException && m->isException[ci])) {
      int k, seen = 0;
      for (k = 0; k < *found; k++)
        if (out[k].license == m->classes[ci]) { seen = 1; break; }
      if (!seen && *found < maxOut) {
        memset(out + *found, 0, sizeof(out[0]));
        out[*found].license = m->classes[ci];
        out[*found].status = "HIGH";
        out[*found].method = method;
        out[*found].start = start;
        out[*found].len = len;
        (*found)++;
      }
      return;
    } }
  for (p = expr; p < stop; ) {
    const char *q = p;
    while (q < stop && !strchr("() \t\r", *q)) q++;
    if (q > p) {
      size_t l = (size_t) (q - p);
      int ci;
      if (l == 2 && !strncasecmp(p, "or", 2)) { op = 1; p = q; continue; }
      if (l == 3 && !strncasecmp(p, "and", 3)) { op = 1; p = q; continue; }
      if (l == 4 && !strncasecmp(p, "with", 4)) { op = 2; p = q; continue; }
      ci = thSpdxResolve(m, p, l);
      if (ci >= 0 && m->isException && m->isException[ci]) {
        if (op == 2 && last >= 0) {
          int k;
          for (k = 0; k < *found; k++)
            if (out[k].license == m->classes[last])
              out[k].exception = m->classes[ci];
        }
      } else if (ci >= 0) {
        int k, seen = 0;
        for (k = 0; k < *found; k++)
          if (out[k].license == m->classes[ci]) { seen = 1; break; }
        if (!seen && *found < maxOut) {
          memset(out + *found, 0, sizeof(out[0]));
          out[*found].license = m->classes[ci];
          out[*found].status = "HIGH";
          out[*found].method = method;
          out[*found].start = start;
          out[*found].len = len;
          (*found)++;
        }
        last = ci;
      }
      op = 0;
    }
    p = q;
    while (p < stop && strchr("() \t\r", *p)) p++;
  }
}

/** The quoted value of a "license"-style field at `p`, or NULL. */
static const char *thFieldValue(const char *p, const char **end)
{
  while (*p == ' ' || *p == '\t') p++;
  if (*p != ':') return NULL;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  /* npm once wrapped the expression in an object under "type" */
  if (*p == '{') {
    const char *t = strstr(p, "\"type\"");
    if (!t || t - p > 80) return NULL;
    p = t + 6;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
  }
  if (*p != '"') return NULL;
  p++;
  *end = p;
  while (**end && **end != '"' && **end != '\n') (*end)++;
  if (**end != '"' || *end - p > 120) return NULL;
  return p;
}

/**
 * \brief The licences a package manifest's license field declares.
 *
 * A manifest states its licence in a field, and that field is the author's
 * declaration exactly as an SPDX tag is: read, not weighed. The classifier
 * cannot reach it, since a manifest is mostly dependency names and the grant
 * gate rejects every window.
 *
 * A lockfile is the exception to the exception: it pins a licence for every
 * dependency it records, so it names dozens and is under none of them. Its
 * root entry, keyed by the empty string and written first, is the package's
 * own and the only one read.
 */
/** A kernel module's declaration and what each accepted string means, by the
 *  kernel's own rules: "GPL" is version 2 with the constraint left open, so
 *  the family is all the declaration states. "Proprietary" states no
 *  licence, and the Mozilla side of a dual is left unversioned. */
static const struct { const char *said; const char *fam[2]; } thModuleLicences[] = {
  {"gpl", {"GPL", NULL}}, {"gpl v2", {"GPL", NULL}},
  {"gpl and additional rights", {"GPL", NULL}},
  {"dual bsd/gpl", {"GPL", "BSD"}}, {"dual mit/gpl", {"GPL", "MIT"}},
  {"dual mpl/gpl", {"GPL", NULL}}, {"lgpl", {"LGPL", NULL}},
};

/**
 * \brief The families a kernel module's MODULE_LICENSE declares.
 *
 * The macro is defined in linux/module.h, so a file that writes it without
 * including that header is quoting it -- documentation, or a table of the
 * accepted strings -- and states nothing. One declaration speaks for a
 * module; several are a list.
 *
 * \return how many of fam[] were filled, at most 2
 */
static int thModuleLicence(const char *text, const char *fam[2])
{
  const char *p = text, *at = NULL;
  int n = 0, i;
  char said[64];
  /* "#include <linux/module.h>" at the start of a line, blanks allowed
     around the hash */
  for (p = text; (p = strstr(p, "include")); p += 7) {
    const char *h = p, *q = p + 7;
    while (h > text && (h[-1] == ' ' || h[-1] == '\t')) h--;
    if (h == text || h[-1] != '#') continue;
    h--;
    while (h > text && (h[-1] == ' ' || h[-1] == '\t')) h--;
    if (h > text && h[-1] != '\n') continue;
    while (*q == ' ' || *q == '\t') q++;
    if (strncmp(q, "<linux/module.h>", 16) == 0) break;
  }
  if (!p) return 0;
  for (p = text; (p = strstr(p, "MODULE_LICENSE")); p += 14) {
    const char *q = p + 14;
    if (p > text && (isalnum((unsigned char) p[-1]) || p[-1] == '_')) continue;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '(') continue;
    q++;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '"') continue;
    if (++n > 1) return 0;
    at = q + 1;
  }
  if (n != 1) return 0;
  /* the string, lower-cased with its blanks folded */
  for (i = 0; at[i] && at[i] != '"' && at[i] != '\n' && i < (int) sizeof(said) - 1; i++)
    said[i] = (char) tolower((unsigned char) at[i]);
  if (at[i] != '"') return 0;
  said[i] = 0;
  { char *w = said, *r = said; int blank = 0;
    for (; *r; r++) {
      if (*r == ' ' || *r == '\t') { blank = 1; continue; }
      if (blank && w > said) *w++ = ' ';
      blank = 0; *w++ = *r;
    }
    *w = 0; }
  for (i = 0; i < (int) (sizeof(thModuleLicences) / sizeof(thModuleLicences[0])); i++)
    if (strcmp(thModuleLicences[i].said, said) == 0) {
      fam[0] = thModuleLicences[i].fam[0];
      fam[1] = thModuleLicences[i].fam[1];
      return fam[1] ? 2 : 1;
    }
  return 0;
}

/**
 * \brief Whether only blanks separate `p` from the start of its line.
 *
 * A manifest writes its licence field indented -- `    license = "BSL-1.0"`
 * in a conan recipe, six spaces of it in a setup.py -- so a field read only
 * at column zero is read in almost no real manifest.
 */
static int thAtLineStart(const char *text, const char *p)
{
  while (p > text && (p[-1] == ' ' || p[-1] == '\t')) p--;
  return p == text || p[-1] == '\n';
}


static int thManifestTags(thModel *m, const char *text, thResult *out,
    int maxOut)
{
  int found = 0;
  const char *p, *v, *e;
  if (strstr(text, "\"lockfileVersion\"")) {
    const char *root = strstr(text, "\"packages\"");
    const char *brace = NULL;
    if (root) {
      const char *q = strstr(root, "\"\"");
      if (q) { brace = strchr(q, '{'); }
    }
    if (!brace) return 0;
    brace++;
    { /* only inside the root entry: past its closing brace lies the first
         dependency, whose licence is not the package's */
      int depth = 1;
      const char *end = brace, *limit = brace + 2000;
      while (*end && end < limit && depth) {
        if (*end == '{') depth++;
        else if (*end == '}') depth--;
        end++;
      }
      for (p = brace; p < end; p++) {
        if (*p != '"') continue;
        if (strncasecmp(p, "\"license\"", 9) && strncasecmp(p, "\"licence\"", 9))
          continue;
        v = thFieldValue(p + 9, &e);
        if (!v) continue;
        if (((size_t) (e - v) == 10 && !strncasecmp(v, "UNLICENSED", 10)) ||
            ((size_t) (e - v) == 4 && !strncasecmp(v, "NONE", 4)) ||
            ((size_t) (e - v) == 12 && !strncasecmp(v, "NOASSERTION", 11)))
          return 0;
        thReadExpression(m, v, e, out, &found, maxOut, "manifest-field",
                         (long) (v - text), (long) (e - v));
        return found;
      }
    }
    return 0;
  }
  for (p = text; *p; p++) {
    if (*p == '"') {
      if (strncasecmp(p, "\"license\"", 9) && strncasecmp(p, "\"licence\"", 9)
          && strncasecmp(p, "\"licenses\"", 10)
          && strncasecmp(p, "\"licences\"", 10)) continue;
      v = thFieldValue(p + (p[8] == 's' || p[8] == 'S' ? 10 : 9), &e);
    } else if (thAtLineStart(text, p)
               && (!strncasecmp(p, "license", 7) || !strncasecmp(p, "licence", 7))) {
      const char *q = p + 7;
      while (*q == ' ' || *q == '\t') q++;
      if (*q != '=') continue;
      q++;
      while (*q == ' ' || *q == '\t') q++;
      if (*q != '"') continue;
      v = q + 1; e = v;
      while (*e && *e != '"' && *e != '\n') e++;
      if (*e != '"' || e - v > 120) continue;
    } else continue;
    if (!v) continue;
    if (((size_t) (e - v) == 10 && !strncasecmp(v, "UNLICENSED", 10)) ||
        ((size_t) (e - v) == 4 && !strncasecmp(v, "NONE", 4)) ||
        ((size_t) (e - v) == 11 && !strncasecmp(v, "NOASSERTION", 11)))
      continue;
    thReadExpression(m, v, e, out, &found, maxOut, "manifest-field",
                     (long) (v - text), (long) (e - v));
  }
  /* A file declaring licences by the dozen is a catalogue, not a file under
     all of them. */
  if (found > TH_TAG_MAX) found = 0;
  return found;
}

/** Compare findings the way the reference sorts them: strongest first. */
static int thCmpFinding(const void *a, const void *b)
{
  const thResult *x = (const thResult *) a, *y = (const thResult *) b;
  if (x->score != y->score) return y->score - x->score;
  if (x->margin < y->margin) return 1;
  if (x->margin > y->margin) return -1;
  return 0;
}

/**
 * \brief Record a window's finding, keeping the better of two for one license.
 */
static void thOffer(thResult *out, int *found, int maxOut, char *buf,
    size_t bufLen, size_t *used, const thResult *r)
{
  int k, at = -1;
  for (k = 0; k < *found; k++)
    if (out[k].license && strcmp(out[k].license, r->license) == 0) { at = k; break; }
  if (at >= 0) {
    /* By score, then margin. The score is a calibrated probability, so
       ranking on margin alone preferred confident-looking weak windows. */
    if (r->score > out[at].score ||
        (r->score == out[at].score && r->margin > out[at].margin)) {
      const char *keep = out[at].license;
      float granted = r->grant > out[at].grant ? r->grant : out[at].grant;
      out[at] = *r;
      out[at].license = keep;
      /* Granting is a fact about the file, not about the window that scored
         best: a license text scores on its body and grants in its appendix. */
      out[at].grant = granted;
    } else if (r->grant > out[at].grant) {
      out[at].grant = r->grant;
    }
  } else if (*found < maxOut) {
    size_t need = strlen(r->license) + 1;
    if (buf && *used + need <= bufLen) {
      memcpy(buf + *used, r->license, need);
      out[*found] = *r;
      out[*found].license = buf + *used;
      *used += need;
      (*found)++;
    }
  }
}

/**
 * \brief The clauses in this window that offer a choice of license.
 *
 * Every pattern is a prefix, so the licenses it governs are the ones written
 * after it and only those. Returning the clause rather than a yes/no keeps the
 * second license tied to the sentence that offered it.
 */
static int thCoordClauses(thModel *m, const char *norm, char **out, int maxOut,
    int spoken)
{
  /* A work stating its own license. A choice read without the head has to be
     the file's own grant: a choice named in passing -- "the original is dual
     licensed" -- is a mention, and a license's text offering a choice is the
     license, not a file that took it. */
  static const char *selfGrant = "(^| )(this|the) (source code|source|code|file|"
      "files|software|program|library|package|work|module|header|project|"
      "document|material)( and [a-z ]{1,30})? (is|are) $";
  static regex_t selfRe;
  static int selfReady = 0;
  size_t nl = strlen(norm);
  int q, n = 0;
  if (spoken && !selfReady) {
    if (regcomp(&selfRe, selfGrant, REG_EXTENDED | REG_NOSUB) != 0) return 0;
    selfReady = 1;
  }
  for (q = 0; q < m->nRef && n < maxOut; q++) {
    size_t off = 0;
    if (strcmp(m->refName[q], "Dual-license") != 0) continue;
    if (m->refWhere && m->refWhere[q]) continue; /* norm-matched only */
    while (n < maxOut && off < nl) {
      regmatch_t hit;
      size_t so, eo, lo, hi;
      if (thRegexec(((regex_t *) m->refRe) + q, m->refLit[q], 0, norm + off, 1, &hit) != 0) break;
      so = off + (size_t) hit.rm_so;
      eo = off + (size_t) hit.rm_eo;
      if (thLicenceContext(norm, so, eo)) {
        char *c;
        int own = 1;
        lo = so > TH_COORD_BACK ? so - TH_COORD_BACK : 0;
        hi = eo + TH_COORD_REACH < nl ? eo + TH_COORD_REACH : nl;
        if (spoken) {
          char before[TH_COORD_BACK + 1];
          memcpy(before, norm + lo, so - lo);
          before[so - lo] = '\0';
          own = regexec(&selfRe, before, 0, NULL, 0) == 0;
        }
        if (!own) { off = eo > so ? eo : eo + 1; continue; }
        c = malloc(hi - lo + 1);
        if (!c) break;
        memcpy(c, norm + lo, hi - lo);
        c[hi - lo] = '\0';
        out[n++] = c;
      }
      /* every pattern ends on a word boundary, so resuming at the end of the
         match cannot invent one */
      off = eo > so ? eo : eo + 1;
    }
  }
  return n;
}

/**
 * \brief The other licenses a coordinating window names.
 *
 * A single-label head returns one license however many the text grants. The
 * second has no margin by construction, so the name decides and the finding
 * carries no match percentage. Among those named, the fullest name wins.
 *
 * With no first license the head was defeated by the choice itself -- two
 * licenses named in one sentence split its confidence -- and every license
 * the clause names is reported by its name, the clause being the file's own.
 */
static int thCoordinated(thModel *m, const char *window, const char *norm,
    const char *first, thResult *out, int maxOut)
{
  thSparse xl = {0};
  float *ls = NULL;
  char *t = NULL;
  char *clause[TH_COORD_CLAUSES];
  int nClause = 0;
  int rank[TH_EXPRESSION_DEPTH], nr = 0;
  int span[TH_EXPRESSION_DEPTH];
  int i, a, b, found = 0;
  char fb[192], fv[64], fs[16];
  char kept[TH_EXPRESSION_MAX][192];
  int nKept = 0;

  if (maxOut <= 0 || !m->nRef) return 0;
  t = norm ? (char *) norm : thNormalise(window);
  if (!t) return 0;
  nClause = thCoordClauses(m, t, clause, TH_COORD_CLAUSES, first == NULL);
  if (!nClause) goto done;
  if (thVectorise(m->licBlk, m->nLicBlk, t, &xl)) goto done;
  ls = malloc((size_t) m->lic.rows * sizeof(float));
  if (!ls) goto done;
  thScore(&m->lic, &xl, NULL, 0, ls);

  for (i = 0; i < m->lic.rows; i++) {
    int q, r2;
    if (m->isException && m->isException[i]) continue;
    for (q = 0; q < TH_EXPRESSION_DEPTH; q++) {
      if (q >= nr) { rank[nr++] = i; break; }
      if (ls[i] > ls[rank[q]]) {
        for (r2 = (nr < TH_EXPRESSION_DEPTH ? nr : TH_EXPRESSION_DEPTH - 1); r2 > q; r2--)
          rank[r2] = rank[r2 - 1];
        if (nr < TH_EXPRESSION_DEPTH) nr++;
        rank[q] = i;
        break;
      }
    }
  }
  /* The name decides, so rank by how much of it is written. */
  for (a = 0; a < nr; a++) {
    int j, most = 0;
    for (j = 0; j < nClause; j++) {
      int sp = thNameSpan(m, rank[a], window, clause[j]);
      if (sp > most) most = sp;
    }
    span[a] = thNameEvidence(m, rank[a], window, t) >= m->thNameMin ? most : 0;
    /* a technology's name in the clause's reach is not a license it offers:
       an editor modeline sits above many a choice of terms */
    if (span[a] && thTechWordOnly(m, rank[a], t)) span[a] = 0;
  }
  for (a = 1; a < nr; a++) {
    int r = rank[a], sp = span[a];
    for (b = a; b > 0 && (span[b - 1] < sp
                          || (span[b - 1] == sp && ls[rank[b - 1]] < ls[r])); b--) {
      rank[b] = rank[b - 1]; span[b] = span[b - 1];
    }
    rank[b] = r; span[b] = sp;
  }

  if (first) {
    thSplit(first, fb, sizeof(fb), fv, sizeof(fv), fs, sizeof(fs));
    snprintf(kept[nKept++], sizeof(kept[0]), "%s", fb);
  }
  for (a = 0; a < nr && found < maxOut && nKept < TH_EXPRESSION_MAX; a++) {
    char cb[192], cv[64], cs[16];
    int dup = 0;
    if (span[a] < 1) continue;
    thSplit(m->classes[rank[a]], cb, sizeof(cb), cv, sizeof(cv), cs, sizeof(cs));
    for (b = 0; b < nKept; b++) if (strcmp(kept[b], cb) == 0) { dup = 1; break; }
    if (dup) continue;
    snprintf(kept[nKept++], sizeof(kept[0]), "%s", cb);
    memset(out + found, 0, sizeof(out[0]));
    out[found].license = m->classes[rank[a]];
    out[found].status = "HIGH";
    out[found].score = 0;
    out[found].method = "coordinated";
    found++;
  }

done:
  for (a = 0; a < nClause; a++) free(clause[a]);
  free(ls);
  free(xl.idx); free(xl.val);
  if (t != norm) free(t);
  return found;
}

/**
 * \brief What a window offering a choice names, where the head named nothing.
 *
 * "This source code is licensed under both the BSD-style license and the
 * GPLv2" names two licenses in one sentence, and a single-label head asked for
 * one of them loses its margin between them and reports neither. The clause
 * still says what it says, when it is the file's own grant and offers a
 * choice: the licenses it names by name, and the family it names where the
 * member is elsewhere.
 */
static int thChoiceWithoutHead(thModel *m, const char *window, const char *norm,
    thResult *out, int maxOut)
{
  char *t;
  char *clause[TH_COORD_CLAUSES];
  int nClause, a, b, found = 0;

  if (maxOut <= 0 || !m->nRef) return 0;
  t = norm ? (char *) norm : thNormalise(window);
  if (!t) return 0;
  nClause = thCoordClauses(m, t, clause, TH_COORD_CLAUSES, 1);
  if (t != norm) free(t);
  if (!nClause) return 0;
  found = thCoordinated(m, window, norm, NULL, out, maxOut);
  for (a = 0; a < nClause; a++) {
    thResult ref[3];
    char names[3][64];
    size_t used = 0;
    int nr = thReferential(m, clause[a], ref, 3, names[0], sizeof(names), &used);
    for (b = 0; b < nr && found < maxOut; b++) {
      int dup = 0, c;
      const char *name = ref[b].license;
      if (strncmp(name, "See-", 4) == 0 || strcmp(name, "Same-license-as") == 0
          || strcmp(name, "Dual-license") == 0
          || strcmp(name, "UnclassifiedLicense") == 0) continue;
      for (c = 0; c < found; c++) {
        char cb[192], cv[64], cs[16];
        thSplit(out[c].license, cb, sizeof(cb), cv, sizeof(cv), cs, sizeof(cs));
        if (strcmp(cb, name) == 0 || strcmp(out[c].license, name) == 0) { dup = 1; break; }
      }
      if (dup) continue;
      /* the table's own name, which outlives the buffer the pointer names */
      for (c = 0; c < m->nRef; c++)
        if (strcmp(m->refName[c], name) == 0) { name = m->refName[c]; break; }
      out[found] = ref[b];
      out[found].license = name;
      found++;
    }
  }
  for (a = 0; a < nClause; a++) free(clause[a]);
  /* a choice of one is no choice */
  return found > 1 ? found : 0;
}

/**
 * \brief Between -only and -or-later, the grant decides, not the score.
 *
 * An exception clause names the license it amends without granting it, and
 * scored apart that bare mention wins. Fires only where one suffix is asserted
 * by a grant and the other is named nowhere that grants.
 */
static int thHasSuffix(const char *name, const char *suffix)
{
  size_t L = strlen(name), S = strlen(suffix);
  return L > S && strcmp(name + L - S, suffix) == 0;
}

/** A window that granted a license "or later": where, and which. */
typedef struct {
  size_t line;        /**< the window's first line */
  char license[192];  /**< copied: a result's name lives only as long as its window */
} thNotice;

/**
 * \brief What share of a license's own phrases this file carries.
 */
static float thPhraseShare(thModel *m, int ci, const char *nt)
{
  int k, here = 0, total = 0;
  if (!m->bodyPhr || ci < 0 || !m->bodyPhr[ci]) return 0.0f;
  for (k = 0; m->bodyPhr[ci][k]; k++) {
    total++;
    if (strstr(nt, m->bodyPhr[ci][k])) here++;
  }
  return total ? (float) here / (float) total : 0.0f;
}

/**
 * \brief The licenses granted "or later" ahead of their own text.
 *
 * A notice that grants "version 2 or later" and then quotes the license in
 * full is read as the license document, whose own "How to Apply" appendix
 * grants nothing; the notice does, and it is told from the appendix by where
 * it stands: before the text, not after it. The text has begun once the
 * windows so far carry as much of it as makes a file the license.
 *
 * \param isText  the suffixed findings whose text the file carries
 * \return the count written to noticed, each a (base, version) pair
 */
static int thGrantedBeforeTheText(thModel *m, const char **line, size_t n,
    const thNotice *notice, int nNotice, const thResult *out,
    const unsigned char *isText, int found, char noticed[][256], int maxOut)
{
  int cls[TH_KEEP_MAX];
  float carried[TH_KEEP_MAX];
  char key[TH_KEEP_MAX][256];
  int nKey = 0, a, k, nOut = 0;
  size_t at = 0, i;
  float bar = m->thBodyMin * TH_IS_THE_TEXT;

  for (a = 0; a < found && nKey < TH_KEEP_MAX; a++) {
    char b[192], v[64], sfx[16];
    int dup = 0;
    if (!isText[a]) continue;
    thSplit(out[a].license, b, sizeof(b), v, sizeof(v), sfx, sizeof(sfx));
    for (k = 0; k < nKey; k++)
      if (strcmp(key[k], b) == 0 && strcmp(key[k] + 128, v) == 0) { dup = 1; break; }
    if (dup) continue;
    snprintf(key[nKey], 128, "%.127s", b);
    snprintf(key[nKey] + 128, 128, "%.127s", v);
    cls[nKey] = thHashGet(m->byName, out[a].license, strlen(out[a].license));
    carried[nKey] = 0.0f;
    nKey++;
  }
  if (!nKey) return 0;
  for (a = 0; a < nNotice && nOut < maxOut; a++) {
    char b[192], v[64], sfx[16];
    int here = -1;
    thSplit(notice[a].license, b, sizeof(b), v, sizeof(v), sfx, sizeof(sfx));
    for (k = 0; k < nKey; k++)
      if (strcmp(key[k], b) == 0 && strcmp(key[k] + 128, v) == 0) { here = k; break; }
    if (here < 0) continue;
    /* the text of every license carried, over the windows between */
    for (i = at; i < notice[a].line; i += TH_WIN_STEP) {
      size_t end = i + TH_WIN_LINES < n ? i + TH_WIN_LINES : n;
      size_t len = (end < n ? (size_t) (line[end] - line[i]) : strlen(line[i]));
      char *w, *nw;
      if (thStrippedLen(line[i], len) < 40u && n > TH_WIN_LINES) continue;
      w = malloc(len + 1);
      if (!w) break;
      memcpy(w, line[i], len); w[len] = '\0';
      nw = thNormalise(w);
      free(w);
      if (!nw) break;
      for (k = 0; k < nKey; k++)
        if (carried[k] <= bar && cls[k] >= 0)
          carried[k] += thBodyEvidence(m, cls[k], nw, bar + 1.0f, 0);
      free(nw);
    }
    at = notice[a].line;
    if (carried[here] <= bar) {
      int dup = 0;
      for (k = 0; k < nOut; k++)
        if (strcmp(noticed[k], key[here]) == 0
            && strcmp(noticed[k] + 128, key[here] + 128) == 0) { dup = 1; break; }
      if (!dup) memcpy(noticed[nOut++], key[here], 256);
    }
  }
  return nOut;
}

/**
 * \brief Between -only and -or-later, the grant decides, not the score.
 *
 * Fires only where one suffix is asserted by a grant and the other is named
 * nowhere that grants, and only where the file grants the license rather than
 * being it: a license document's "How to Apply" appendix is text, not a grant
 * it makes. A file that is the license still grants it where a notice ahead
 * of the text says "or later", and that grant decides.
 */
static int thGrantDecidesSuffix(thModel *m, const char *nt, thResult *out,
    int found, const char **line, size_t n, const thNotice *notice, int nNotice)
{
  unsigned char isText[TH_KEEP_MAX], asserted[TH_KEEP_MAX], drop[TH_KEEP_MAX];
  char noticed[TH_KEEP_MAX][256];
  int nNoticed, a, b, w;

  if (found > TH_KEEP_MAX) found = TH_KEEP_MAX;
  memset(isText, 0, sizeof(isText));
  memset(drop, 0, sizeof(drop));
  for (a = 0; a < found; a++) {
    char ba[192], va[64], sa[16];
    int ci;
    thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
    if (!sa[0]) continue;
    ci = thHashGet(m->byName, out[a].license, strlen(out[a].license));
    /* Weight alone says "a lot of this license's wording is here", which a
       notice satisfies: the standard header's phrases are the license's too,
       and they are the heaviest rows it has. What separates the document from
       the notice is how much of the license is here at all -- a notice
       carries a tenth of its phrases, the document nearly all. */
    isText[a] = ci >= 0 && nt
        && thBodyEvidence(m, ci, nt, 1e9f, 0) > m->thBodyMin * TH_IS_THE_TEXT
        && thPhraseShare(m, ci, nt) >= TH_TEXT_SHARE;
  }
  nNoticed = thGrantedBeforeTheText(m, line, n, notice, nNotice, out, isText,
      found, noticed, TH_KEEP_MAX);
  for (a = 0; a < found; a++) {
    char ba[192], va[64], sa[16];
    thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
    asserted[a] = 0;
    if (!sa[0]) continue;
    if (isText[a]) {
      int k;
      if (strcmp(sa, "or-later") != 0) continue;
      for (k = 0; k < nNoticed; k++)
        if (strcmp(noticed[k], ba) == 0 && strcmp(noticed[k] + 128, va) == 0) {
          asserted[a] = 1;
          break;
        }
    } else {
      asserted[a] = out[a].grant > 0.0f;
    }
  }
  for (a = 0; a < found; a++) {
    char ba[192], va[64], sa[16];
    int conflict = 0, other = 0;
    if (!asserted[a]) continue;
    thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
    for (b = 0; b < found; b++) {
      char bb[192], vb[64], sb[16];
      if (b == a) continue;
      thSplit(out[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
      if (!sb[0] || strcmp(ba, bb) || strcmp(va, vb) || !strcmp(sa, sb)) continue;
      conflict = 1;
      if (asserted[b]) other = 1;
    }
    /* Only when this side is asserted and no other side is. */
    if (!conflict || other) continue;
    for (b = 0; b < found; b++) {
      char bb[192], vb[64], sb[16];
      if (b == a) continue;
      thSplit(out[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
      if (sb[0] && !strcmp(ba, bb) && !strcmp(va, vb) && strcmp(sa, sb))
        drop[b] = 1;
    }
  }
  /* The file is the licence document and nothing ahead of the text grants it
     "or later": the document is the licence as published, and its appendix
     is text, not a grant. Where both suffixes are found, both as the text,
     and neither is asserted, -only stays. */
  for (a = 0; a < found; a++) {
    char ba[192], va[64], sa[16];
    int conflict = 0, anyAsserted = 0, allText = isText[a];
    thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
    if (!sa[0] || strcmp(sa, "or-later") != 0 || drop[a]) continue;
    for (b = 0; b < found; b++) {
      char bb[192], vb[64], sb[16];
      thSplit(out[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
      if (!sb[0] || strcmp(ba, bb) || strcmp(va, vb)) continue;
      if (asserted[b]) anyAsserted = 1;
      if (!isText[b]) allText = 0;
      if (b != a && strcmp(sa, sb)) conflict = 1;
    }
    if (conflict && !anyAsserted && allText) drop[a] = 1;
  }
  for (b = 0, w = 0; b < found; b++)
    if (!drop[b]) out[w++] = out[b];
  return w;
}

/** \brief The conditions this text writes, by their letters. */
/* A possessive written where a plural was meant -- OpenCV's "Redistribution's
   of source code" -- normalises to "redistribution s of", one letter adrift
   of the marker: the text with such an "s" joined back to its word. */
static char *thJoinStrayS(const char *nt)
{
  size_t len = strlen(nt), i = 0, o = 0, run = 0;
  char *out = malloc(len + 1);
  if (!out) return NULL;
  for (i = 0; i < len; i++) {
    if (nt[i] == ' ' && run >= 4 && i + 2 <= len && nt[i + 1] == 's'
        && (i + 2 == len || nt[i + 2] == ' ')) {
      out[o++] = 's'; i++; run = 0; continue;
    }
    run = (nt[i] >= 'a' && nt[i] <= 'z') ? run + 1 : 0;
    out[o++] = nt[i];
  }
  out[o] = '\0';
  return out;
}

static int thClausesHere(thModel *m, const char *nt, char *here)
{
  int k, n = 0;
  /* in table order: a signature is compared as a string */
  char *joined = strstr(nt, " s ") ? thJoinStrayS(nt) : NULL;
  for (k = 0; k < m->nClause; k++)
    if (m->clausePhr[k] && (strstr(nt, m->clausePhr[k])
                            || (joined && strstr(joined, m->clausePhr[k]))))
      here[n++] = m->clauseKey[k][0];
  here[n] = 0;
  free(joined);
  return n;
}

/**
 * \brief Whether a condition written here is one the class's own text does
 *        not carry -- read within its family, waived for a file that names
 *        the class. A window sees part of a notice, so this is all it can
 *        rule out by.
 */
static int thBeyondSignature(thModel *m, int ci, const char *text, const char *nt,
    const char *here, int n)
{
  const char *want, *marks, *keys = NULL;
  int j;
  want = (ci >= 0 && m->clauseSig) ? m->clauseSig[ci] : NULL;
  if (!want) return 0;
  marks = strchr(want, ':');
  if (marks) {
    size_t len = (size_t) (marks - want);
    marks++;
    for (j = 0; j < m->nFamily; j++)
      if (strlen(m->famName[j]) == len && strncmp(m->famName[j], want, len) == 0) {
        keys = m->famKeys[j];
        break;
      }
  } else {
    marks = want;
  }
  for (j = 0; j < n; j++)
    if ((!keys || strchr(keys, here[j])) && !strchr(marks, here[j]))
      return thNameEvidence(m, ci, text, nt) < m->thNameMin;
  return 0;
}

/**
 * \brief Whether the conditions written here are the ones this class carries.
 *
 * Read within the license's own family, so a BSD condition says nothing about
 * an HPND claim, and waived for a file that names the license.
 */
static int thClauseAllows(thModel *m, int ci, const char *text, const char *nt,
    const char *here, int n)
{
  char mine[TH_CLAUSE_MAX + 1];
  const char *want, *marks, *keys = NULL;
  int j, o = 0;
  want = (ci >= 0 && m->clauseSig) ? m->clauseSig[ci] : NULL;
  if (!want) return 1;
  marks = strchr(want, ':');
  if (marks) {
    size_t len = (size_t) (marks - want);
    marks++;
    for (j = 0; j < m->nFamily; j++)
      if (strlen(m->famName[j]) == len
          && strncmp(m->famName[j], want, len) == 0) {
        keys = m->famKeys[j];
        break;
      }
  } else {
    marks = want;                 /* a table written before families existed */
  }
  for (j = 0; j < n; j++)
    if (!keys || strchr(keys, here[j])) mine[o++] = here[j];
  mine[o] = 0;
  return strcmp(marks, mine) == 0
      || thNameEvidence(m, ci, text, nt) >= m->thNameMin;
}

/**
 * \brief Drop a license whose conditions the file does not write.
 *
 * A containment family's licenses sit inside one another, so no wording is
 * exclusive to the shorter ones and every longer notice matches them in full.
 * What separates them is which conditions are written: BSD-Source-Code is
 * source-retain and no-endorsement with no binary clause, so a notice carrying
 * "Redistributions in binary form" is not under it, and HPND-sell-regexpr is
 * the opening sentence alone, so a notice going on to name an advertising
 * clause is not under it either.
 *
 * Conditions are read within a family and never across one, so a file carrying
 * a BSD condition says nothing about an HPND claim. A file that names its
 * license is taken at its word, and a file writing no conditions says nothing
 * either way. Nothing replaces what is dropped: several licenses share one
 * signature, so the conditions rule licenses out without ever naming one.
 */
/** \brief needle in hay on word boundaries, as normalised text has them */
static const char *thWordsAt(const char *hay, const char *needle)
{
  size_t len = strlen(needle);
  const char *p = hay;
  while ((p = strstr(p, needle)) != NULL) {
    if ((p == hay || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) return p;
    p++;
  }
  return NULL;
}

/**
 * \brief A numbered item that says it is gone -- "3. [Deleted]", "3. Rescinded
 *        22 July 1999" -- keeps its number and is no condition.
 */
static int thConditionGone(const char *item)
{
  static const char *gone[] = {"deleted", "removed", "rescinded", "omitted", "withdrawn",
                               "intentionally", "reserved", "blank", NULL};
  int k, word;
  const char *p = item;
  while (*p == ' ') p++;
  /* the word may follow the clause's name -- "BSD Advertising Clause
     omitted per the July 22, 1999 licensing change" -- so it is read within
     the item's first words */
  for (word = 0; word < 6 && *p; word++) {
    for (k = 0; gone[k]; k++) {
      size_t len = strlen(gone[k]);
      if (strncmp(p, gone[k], len) == 0 && (p[len] == ' ' || p[len] == '\0')) return 1;
    }
    /* but not past a "not": "3. This notice may not be removed" is zlib's
       condition */
    if (strncmp(p, "not", 3) == 0 && (p[3] == ' ' || p[3] == '\0')) return 0;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
  }
  return 0;
}

/**
 * \brief The fewest conditions any numbered conditions list here writes,
 *        and in *longest how long the longest list is.
 *
 * A list opens at "the following conditions" or "provided that" and closes
 * where the disclaimer begins -- one that never closes is not a licence's. The numbers are read as the longest run 1, 2, 3 ...
 * written in that order inside it, and each item counts unless it says it is
 * gone. A bulleted list counts as none: only a number the file wrote is
 * evidence of a condition.
 * \return the smallest count over the lists that number anything, or 0
 */
static int thFewestConditions(const char *nt, long *longest, int *any, int *gone)
{
  static const char *close[] = {"is provided", "provided by", "in no event", NULL};
  static const char *open[] = {"following conditions", "following restrictions", "provided that", NULL};
  const char *p;
  int fewest = 0, o;
  *longest = 0;
  *any = 0;
  if (gone) *gone = 0;
  for (o = 0; open[o]; o++)
  for (p = nt; (p = thWordsAt(p, open[o])) != NULL; ) {
    const char *from = p + strlen(open[o]), *end = NULL, *at = from;
    int k, n = 0, count = 0, known = 1;
    size_t left = strlen(from);
    for (k = 0; close[k]; k++) {
      const char *e = thWordsAt(from, close[k]);
      if (e && (!end || e < end)) end = e;
    }
    /* A list closes where the disclaimer begins, or where a notice cut
       short of one ends. One that closes neither way -- zlib writes its
       disclaimer first and its restrictions last, straight into the code --
       is read over a licence's reach and its length is unknown. */
    if (end && end - from > TH_CONDITIONS_SPAN) end = NULL;
    if (!end) {
      if (left <= TH_CONDITIONS_SPAN) end = from + left;
      else { end = from + TH_CONDITIONS_SPAN; known = 0; }
    }
    *any = 1;
    for (;;) {
      char num[16];
      const char *hit, *item;
      int len = snprintf(num, sizeof(num), " %d ", n + 1);
      hit = strstr(at, num);
      if (!hit || hit + len > end) break;
      /* a numbered list begins within a few words of its opener */
      if (n == 0 && hit - from > TH_CONDITIONS_LEAD) break;
      n++;
      at = hit + 1;
      /* the item's wording: past its number, to the next number or the end */
      item = hit + len;
      while (*item == ' ') item++;
      if (!thConditionGone(item)) count++;
      else if (gone) *gone = 1;
    }
    if (count > 0 && (fewest == 0 || count < fewest)) fewest = count;
    if (known && end - from > *longest) *longest = end - from;
    p = from;
  }
  return fewest;
}

/**
 * \brief Whether a conditions list here numbers an item it says is gone:
 *        "3. [Deleted]", "3. BSD Advertising Clause omitted". The notice
 *        then states the licence without that condition, rather than
 *        leaving one out.
 */
static int thConditionsGone(const char *nt)
{
  long longest;
  int any, gone = 0;
  thFewestConditions(nt, &longest, &any, &gone);
  return gone;
}

/**
 * \brief The family whose conditions a licence is judged by: the family of
 *        its clause signature, else the family whose member pattern names
 *        it.
 * \return the family's index, or -1
 */
static int thConditionsFamily(thModel *m, const char *name)
{
  int ci = thHashGet(m->byName, name, strlen(name)), fi;
  if (ci >= 0 && m->clauseSig && m->clauseSig[ci]) {
    const char *sig = m->clauseSig[ci], *colon = strchr(sig, ':');
    size_t fl = colon ? (size_t) (colon - sig) : 0;
    for (fi = 0; fi < m->nFamily; fi++)
      if (strlen(m->famName[fi]) == fl && strncmp(m->famName[fi], sig, fl) == 0) return fi;
    return -1;
  }
  for (fi = 0; fi < m->nFamily; fi++)
    if (m->famMemberOk[fi] && regexec(&m->famMember[fi], name, 0, NULL, 0) == 0) return fi;
  return -1;
}

/**
 * \brief A licence whose conditions the file numbers more of, or none of.
 *
 * Every marker of BSD-3-Clause is written, and then a fourth condition the
 * family has no marker for: "may only be used in connection with an Atmel
 * microcontroller product". The licence is not BSD-3-Clause, and no member
 * of the family has that clause; a licence is here that the model cannot
 * name, which is what UnclassifiedLicense says -- and what sends the file to
 * a reviewer instead of clearing it as BSD.
 *
 * The mirror: TekHVC's grant sentence, "provided that this copyright,
 * permission, and disclaimer notice is reproduced in all copies", with none
 * of the three numbered conditions that make TekHVC TekHVC; or zlib's grant
 * with no list at all. The head reads the sentence and the disclaimer and
 * names the licence; the licence's own conditions are absent, and the
 * licence with them.
 *
 * Only a numbered list is read, and only against what the licence's own
 * text writes, so a list the file bullets in full, or a licence that numbers
 * its own extra condition, is left alone -- as is a file that names the
 * licence it claims.
 */
static int thBeyondItsConditions(thModel *m, const char *text, const char *nt,
    thResult *out, int found)
{
  int a, w = 0, fewest, any;
  long longest;
  if (!(m->clauseCount || m->clauseSpan) || found <= 0) return found;
  fewest = thFewestConditions(nt, &longest, &any, NULL);
  for (a = 0; a < found; a++) {
    int ci = out[a].license ? thHashGet(m->byName, out[a].license, strlen(out[a].license)) : -1;
    int own = ci >= 0 && m->clauseCount ? m->clauseCount[ci] : 0, b, dup = 0;
    long span = ci >= 0 && m->clauseSpan ? m->clauseSpan[ci] : 0;
    const char *lead = ci >= 0 && m->clauseLead ? m->clauseLead[ci] : NULL;
    /* more conditions than the licence numbers, or -- the mirror -- a list
       numbering nothing and far shorter than the licence's, or no list at
       all where the file writes the words that run into the licence's:
       zlib's "Permission is granted to anyone to use this software for any
       purpose ... and redistribute it freely" without its three
       restrictions. A file that quotes the licence elsewhere -- its
       suggested notice -- is left alone. */
    int beyond = own > 0 && fewest > own;
    int shortOf = span > 0 && fewest == 0
        && ((!any && lead && strstr(nt, lead))
            || (longest > 0 && (double) longest < TH_CONDITIONS_SHARE * (double) span));
    if ((beyond || shortOf)
        && !(out[a].method && strcmp(out[a].method, "spdx-tag") == 0)
        && thNameEvidence(m, ci, text, nt) < m->thNameMin) {
      out[a].method = thMethodWith(out[a].method, "clause-count");
      out[a].license = "UnclassifiedLicense";
    }
    for (b = 0; b < w && !dup; b++)
      if (out[b].license && out[a].license && strcmp(out[b].license, out[a].license) == 0) dup = 1;
    if (!dup) out[w++] = out[a];
  }
  return w;
}

static int thClauseMismatch(thModel *m, const char *text, const char *nt,
    thResult *out, int found)
{
  char here[TH_CLAUSE_MAX + 1];
  int a, w = 0, n;
  if (!m->clauseSig || m->nClause <= 0 || found <= 0) return found;
  n = thClausesHere(m, nt, here);
  if (n == 0) return thBeyondItsConditions(m, text, nt, out, found); /* no marker written */
  for (a = 0; a < found; a++) {
    int ci = -1, b;
    if (out[a].license && !(out[a].method
                            && strcmp(out[a].method, "spdx-tag") == 0)) {
      for (b = 0; b < m->nClasses; b++)
        if (strcmp(m->classes[b], out[a].license) == 0) { ci = b; break; }
      if (!thClauseAllows(m, ci, text, nt, here, n)) continue;
    }
    out[w++] = out[a];
  }
  return thBeyondItsConditions(m, text, nt, out, w);
}

/**
 * \brief Which four-byte runs a text carries, so a phrase it lacks is refused
 *        without a search.
 *
 * A file is asked for tens of thousands of phrases and carries almost none
 * of them. Three runs of a phrase are looked up in a bit table of every run
 * the text has; a phrase with a run the text lacks cannot be a substring of
 * it, and only one that passes is searched for. The table can say yes to a
 * run the text lacks, never no to one it has, so the answer is the search's.
 */
typedef struct { unsigned char *bit; unsigned long mask; } thGrams;
#define TH_GRAM 8 /**< bytes per run: four is a syllable, which every file has */

static unsigned long thGramAt(const char *p)
{
  unsigned long h = 1469598103934665603UL;
  int i;
  for (i = 0; i < TH_GRAM; i++) h = (h ^ (unsigned char) p[i]) * 1099511628211UL;
  return h ^ (h >> 29);
}

static void thGramsBuild(thGrams *g, const char *t)
{
  size_t n = strlen(t), i, bits = 1u << 16;
  while (bits < n * 16 && bits < (1u << 26)) bits <<= 1;
  g->mask = bits - 1;
  g->bit = calloc(bits / 8, 1);
  if (!g->bit) { g->mask = 0; return; }
  for (i = 0; i + TH_GRAM <= n; i++) {
    unsigned long h = thGramAt(t + i) & g->mask;
    g->bit[h >> 3] |= (unsigned char) (1u << (h & 7));
  }
}

static void thGramsFree(thGrams *g) { free(g->bit); g->bit = NULL; }

/** \brief strstr, asked only where the phrase's runs are all in the table. */
static const char *thGramsFind(const thGrams *g, const char *t, const char *ph)
{
  size_t L = strlen(ph);
  if (g->bit && L >= TH_GRAM) {
    size_t span = L - TH_GRAM, k;
    for (k = 0; k < 5; k++) {
      unsigned long h = thGramAt(ph + span * k / 4) & g->mask;
      if (!(g->bit[h >> 3] & (1u << (h & 7)))) return NULL;
    }
  }
  return strstr(t, ph);
}

/**
 * \brief Whether this class may be read, given a family to stay inside.
 *
 * Without a family every class but an exception is read; with one, only its
 * members whose conditions the file writes.
 */
static int thVerbatimEligible(thModel *m, int i, const char *text,
    const char *nt, const char *family, size_t famLen, const char *here,
    int nHere)
{
  const char *want, *colon;
  if (m->isException && m->isException[i]) return 0;
  if (!family) return 1;
  want = m->clauseSig ? m->clauseSig[i] : NULL;
  colon = want ? strchr(want, ':') : NULL;
  if (!colon || (size_t) (colon - want) != famLen
      || strncmp(want, family, famLen) != 0)
    return 0;
  return thClauseAllows(m, i, text, nt, here, nHere);
}

/**
 * \brief Whether a license that extends another is the one written here.
 *
 * A variant is its base plus a clause of its own, so on any text of the family
 * it scores within a hair of the base and the higher share is arbitrary. What
 * settles it is the clause: a file that does not write the wording making the
 * variant a variant is under the license it extends. Asked only of a family
 * being read by its conditions, and only where they leave the base a license
 * this file could be under -- without them there is nothing to say a base is
 * nearer the file than the variant, and the pick has still to be judged.
 */
static int thIsTheVariant(thModel *m, int i, const char *text, const char *nt,
    const thGrams *g, const char *family, size_t famLen, const char *here,
    int nHere)
{
  int base, n = 0, miss = 0, allowed, k;
  if (!family || !m->variantBase || !m->varPhr || !m->varPhr[i]) return 1;
  base = m->variantBase[i];
  if (base < 0
      || !thVerbatimEligible(m, base, text, nt, family, famLen, here, nHere))
    return 1;
  while (m->varPhr[i][n]) n++;
  allowed = (int) ((float) n * (1.0f - TH_VERBATIM));
  for (k = 0; k < n; k++)
    if (!thGramsFind(g, nt, m->varPhr[i][k]) && ++miss > allowed) return 0;
  return 1;
}

/** \brief The license whose own text this file carries most fully, or -1. */
static int thVerbatimPick(thModel *m, const char *text, const char *nt,
    const thGrams *g, const char *family, size_t famLen, const char *here,
    int nHere)
{
  int i, best = -1;
  float most = 0.0f;
  for (i = 0; i < m->nClasses; i++) {
    int n = 0, miss = 0, allowed, k;
    if (!m->varPhr[i]) continue;
    if (!thVerbatimEligible(m, i, text, nt, family, famLen, here, nHere))
      continue;
    while (m->varPhr[i][n]) n++;
    if (n < TH_VERBATIM_MIN) continue;
    allowed = (int) ((float) n * (1.0f - TH_VERBATIM));
    for (k = 0; k < n; k++)
      if (!thGramsFind(g, nt, m->varPhr[i][k]) && ++miss > allowed) break;
    if (miss > allowed) continue;
    /* Among the licenses whose distinguishing text is here, the one whose text
       is here most fully: a disclaimer every BSD-like license shares can be
       most of a short list of phrases. */
    { float weight = thBodyEvidence(m, i, nt, 1e9f, 0);
      if (weight > most) { most = weight; best = i; } }
  }
  if (best >= 0) return best;
  /* Variant phrases cover only the licenses SPDX derives from another, so a
     license that is nobody's variant -- MIT above all -- has no entry and
     could never be found here. Its own text can still be the whole file:
     MIT's canonical wording names no license anywhere in it, so a real MIT
     header offers the classifier nothing to be confident about and every
     window is a fragment of a notice it cannot place. What the file does
     carry is the license, near enough word for word. */
  /* Length is not read here. Scoring a whole file dilutes a notice until the
     verifier cannot clear it, which is what TH_WHOLE_MAX is for; this asks
     whether the license's phrases are in the text at all, and a long file
     neither weakens nor strengthens that. Capping it by size lost every source
     file whose header is the license and whose body is large. */
  if (!m->bodyPhr) return -1;
  { float shareBest = 0.0f;
    int bestHits = 0, a, b;
    for (i = 0; i < m->nClasses; i++) {
      int n = 0, k, allowed, miss = 0;
      if (!m->bodyPhr[i]) continue;
      if (!thVerbatimEligible(m, i, text, nt, family, famLen, here, nHere))
        continue;
      while (m->bodyPhr[i][n]) n++;
      if (n < TH_VERBATIM_MIN) continue;
      allowed = (int) ((float) n * (1.0f - TH_VERBATIM));
      for (k = 0; k < n; k++)
        if (!thGramsFind(g, nt, m->bodyPhr[i][k]) && ++miss > allowed) break;
      if (miss > allowed) continue;
      /* How completely this license's own text is here, not how much of it
         there is: a longer license carries more weight for the same notice,
         and weighing them picked Xnet over the MIT the file was. */
      { float share = 1.0f - (float) miss / (float) n;
        if (share > shareBest
            && thIsTheVariant(m, i, text, nt, g, family, famLen, here, nHere)) {
          shareBest = share; best = i; bestHits = n - miss;
        } }
    }

    /* How much of the nearest way each license has been written is here. A
       rendering answers only where the file carries it, so all but a phrase
       or two of it has to be present and a license that is not the one falls
       out within the first few -- which is what makes asking it of every
       rendering cost almost nothing. */
    if (m->rendPhr) {
      for (a = 0; a < m->nClasses; a++) {
        int r;
        if (!m->rendPhr[a]) continue;
        if (!thVerbatimEligible(m, a, text, nt, family, famLen, here, nHere))
          continue;
        for (r = 0; m->rendPhr[a][r]; r++) {
          int n = 0, allowed, miss = 0;
          double share;
          while (m->rendPhr[a][r][n]) n++;
          if (n < TH_VERBATIM_MIN) continue;
          allowed = n - (int) ceil(TH_RENDER_MIN * (double) n);
          for (b = 0; b < n; b++)
            if (!thGramsFind(g, nt, m->rendPhr[a][r][b]) && ++miss > allowed) break;
          if (miss > allowed) continue;
          share = 1.0 - (double) miss / (double) n;
          /* And explaining at least as much of the file as the license it
             displaces: a short license written into a longer notice is wholly
             present in it, which is a perfect share of a smaller thing and not
             a better account of what the file says. */
          if (share > (double) shareBest && n - miss >= bestHits
              && thIsTheVariant(m, a, text, nt, g, family, famLen, here, nHere)) {
            shareBest = (float) share; best = a; bestHits = n - miss;
          }
        }
      }
    }
  }
  return best;
}

/**
 * \brief The license whose own distinguishing text this file carries.
 *
 * A last resort for a notice longer than one window that names no license:
 * every window sees a fragment and the verifier is right to be unsure of each,
 * while over the whole file the phrases that tell a license from its siblings
 * are decisive. It carries no match percentage, having no calibration behind
 * it.
 *
 * The text a file carries most fully is often the shortest license of a
 * containment family, which the file's own conditions then rule out. What may
 * take its place is read from that family and nowhere else: outside it the
 * conditions say nothing, so the license scoring next is merely the next
 * license, not the one the file was narrowed to.
 */
static int thVerbatim(thModel *m, const char *text, thResult *out, int maxOut,
    char *buf, size_t bufLen, size_t *used)
{
  char *nt;
  char here[TH_CLAUSE_MAX + 1];
  thGrams g;
  int best, nHere;
  if (!m->varPhr || maxOut <= 0) return 0;
  nt = thNormalise(text);
  if (!nt) return 0;
  thGramsBuild(&g, nt);
  best = thVerbatimPick(m, text, nt, &g, NULL, 0, NULL, 0);
  if (best >= 0 && m->clauseSig && m->nClause > 0) {
    nHere = thClausesHere(m, nt, here);
    if (nHere > 0 && !thClauseAllows(m, best, text, nt, here, nHere)) {
      const char *want = m->clauseSig[best];
      const char *colon = want ? strchr(want, ':') : NULL;
      best = colon ? thVerbatimPick(m, text, nt, &g, want,
                                    (size_t) (colon - want), here, nHere) : -1;
    }
  }
  thGramsFree(&g);
  free(nt);
  if (best < 0) return 0;
  { size_t need = strlen(m->classes[best]) + 1;
    if (!buf || *used + need > bufLen) return 0;
    memcpy(buf + *used, m->classes[best], need);
    memset(out, 0, sizeof(*out));
    out->license = buf + *used;
    out->status = "HIGH";
    out->method = "verbatim";
    *used += need;
  }
  return 1;
}

/**
 * \brief A dedication to the public domain is an identification, not a pointer.
 *
 * There is no text to match: "this file has been put into the public domain"
 * is the whole of what the licensor wrote, and FOSSology records it under the
 * name nomos has used for years. It is read from two shapes only -- the work
 * naming itself as the subject, and a header line that says nothing but
 * "public domain" beside an author or a year -- because "public domain" in
 * prose is about other things, and a denial ("not in the public domain")
 * withdraws it.
 * \return 1 where the file dedicates itself
 */
static int thDedication(thModel *m, const char *text, const char *nt)
{
  int i, stated = 0;
  for (i = 0; i < m->nRef; i++) {
    if (!m->refWhere) return 0;
    if (m->refWhere[i] == 0 && strcmp(m->refName[i], "NOT-public-domain") == 0
        && thRegexec(((regex_t *) m->refRe) + i, m->refLit[i], 0, nt, 0, NULL) == 0)
      return 0;
  }
  for (i = 0; i < m->nRef && !stated; i++) {
    if (strcmp(m->refName[i], "Public-domain") != 0) continue;
    if (m->refWhere[i] == 4 && thRegexec(((regex_t *) m->refRe) + i, m->refLit[i], 0, nt, 0, NULL) == 0)
      stated = 1;
    else if (m->refWhere[i] == 5 && thRegexec(((regex_t *) m->refRe) + i, m->refLit[i], 0, text, 0, NULL) == 0)
      stated = 1;
  }
  return stated;
}

/**
 * \brief The one holder of finding a whose own wording is on the page, or -1.
 *
 * The mirror rule's test. Holders hold each other: the notice that carries
 * HPND carries every word of the licenses HPND is written inside, so they fit
 * as well. The smallest fitting holder is the one written here; whether a
 * larger one is too is the same question asked again, which the pass after
 * the fallbacks asks. Two fitting holders neither of which holds the other
 * settle nothing.
 */
/** \brief whether a phrase, as space-separated words, carries this word */
static int thPhraseHasWord(const char *phrase, const char *word)
{
  size_t len = strlen(word);
  const char *p = phrase;
  while ((p = strstr(p, word)) != NULL) {
    if ((p == phrase || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) return 1;
    p++;
  }
  return 0;
}

static int thHolderWrittenHere(thModel *m, const thResult *out, int found, int a,
    const char *text, const char *nt)
{
  int ci, h, k, nFit = 0, i, j, left = -1, nLeft = 0;
  int fit[TH_HOLDERS_MAX];
  const char *fitName[TH_HOLDERS_MAX];
  for (ci = 0; ci < m->nClasses; ci++)
    if (strcmp(m->classes[ci], out[a].license) == 0) break;
  if (ci >= m->nClasses || !m->conNear[ci] || !m->conDist[ci]) return -1;
  if (thNameEvidence(m, ci, text, nt) >= m->thNameMin) return -1;
  for (h = 0; m->conNear[ci][h] && nFit < TH_HOLDERS_MAX; h++) {
    int b, oc, k, n = 0, here = 0, claimed = 0;
    const char *outer = m->conNear[ci][h];
    if (!m->conDist[ci][h]) continue;
    for (b = 0; b < found; b++)
      if (b != a && out[b].license && strcmp(out[b].license, outer) == 0) { claimed = 1; break; }
    for (oc = 0; oc < m->nClasses; oc++)
      if (strcmp(m->classes[oc], outer) == 0) break;
    if (oc >= m->nClasses || (m->isException && m->isException[oc])) continue;
    /* Less the phrases that carry the holder's own name: "nor CyberSAFE
       Corporation make any representations" is the holder's, and a file
       under another holder writes the same sentence with another name in
       it -- which is also why the name itself must then be here: what is
       left of X11 without "X Consortium" is the wording of MIT-open-group. */
    { char **theirs = m->conHolder ? m->conHolder[oc] : NULL;
      int total = 0, w, namedHere = 0;
      for (k = 0; m->conDist[ci][h][k]; k++) {
        const char *ph = m->conDist[ci][h][k];
        int carries = 0;
        total++;
        for (w = 0; theirs && theirs[w] && !carries; w++)
          if (thPhraseHasWord(ph, theirs[w])) carries = 1;
        if (carries) continue;
        n++;
        if (strstr(nt, ph)) here++;
      }
      /* The name is required only where the holder is named inside the
         distinguishing wording: a name there takes a shingle's width of
         phrases with it, while one that only grazes a phrase's edge is the
         copyright line. And required as a holder -- in a copyright
         statement: a document's subtitle naming the X Consortium says
         nothing about whose notice it carries lower down. */
      if (total - n >= TH_HOLDER_NAMED) {
        const char *cp = nt;
        while (!namedHere && (cp = strstr(cp, "copyright")) != NULL) {
          char stmt[TH_HOLDER_NEAR + 1];
          size_t len = strlen(cp);
          if (len > TH_HOLDER_NEAR) len = TH_HOLDER_NEAR;
          memcpy(stmt, cp, len); stmt[len] = '\0';
          for (w = 0; theirs && theirs[w] && !namedHere; w++)
            if (thWordsAt(stmt, theirs[w])) namedHere = 1;
          cp += 9;
        }
        if (!namedHere) continue;
      }
      if (n * 2 < total) continue; }
    if (n && (float) here >= TH_VERBATIM * (float) n) {
      /* A fitting holder another window already named is the one written
         here, and the smaller licence is its part: the two findings are
         one. Not a second holder beside it. */
      if (claimed) return oc;
      fit[nFit] = oc;
      fitName[nFit] = outer;
      nFit++;
    }
  }
  for (i = 0; i < nFit; i++) {
    int held = 0;
    for (j = 0; j < nFit && !held; j++) {
      const char **near;
      if (j == i || !m->conNear[fit[j]]) continue;
      near = (const char **) m->conNear[fit[j]];
      for (k = 0; near[k]; k++)
        if (strcmp(near[k], fitName[i]) == 0) { held = 1; break; }
    }
    if (!held) { left = fit[i]; nLeft++; }
  }
  return nLeft == 1 ? left : -1;
}

/**
 * \brief Mark as covered what the other suffix of this licence explains.
 *
 * -only and -or-later are one document rendered twice over, so what one
 * explains the other explains: a licence quoting the GPL is subsumed by
 * GPL-1.0-only as it was by GPL-1.0-or-later.
 */
static void thCoverSuffixSibling(thModel *m, const char *name, const char *nt,
    unsigned char *covered)
{
  char base[192], ver[64], suf[16], other[272];
  int ci, k;
  thSplit(name, base, sizeof(base), ver, sizeof(ver), suf, sizeof(suf));
  if (!suf[0]) return;
  snprintf(other, sizeof(other), "%.191s-%.63s-%s", base, ver,
           strcmp(suf, "or-later") == 0 ? "only" : "or-later");
  ci = thHashGet(m->byName, other, strlen(other));
  if (ci < 0 || !m->bodyPhr || !m->bodyPhr[ci]) return;
  for (k = 0; m->bodyPhr[ci][k]; k++) {
    const char *ph = m->bodyPhr[ci][k];
    size_t pl = strlen(ph);
    const char *at = strstr(nt, ph);
    while (at) {
      memset(covered + (size_t) (at - nt), 1, pl);
      at = strstr(at + 1, ph);
    }
  }
}


/* A license's text written in full beside another finding: an MIT notice
   that never says "MIT" scores little in any window, and under an SPDX tag
   for another family the whole-file pass never ran. What the file carries
   most fully stands where it is another family than any finding, neither
   holds the other, and its run is not a found license's run read under
   another name. */
static int thVerbatimBeside(thModel *m, const char *text, const char *nt,
    thResult *out, int found, int maxOut, char *buf, size_t bufLen, size_t *used)
{
  thResult whole;
  size_t mark = *used, len;
  int a, ci, allRef = 1, h;
  char stem[192], v[64], sf[16], fstem[192];
  const char *d;
  unsigned char *map;
  long mine[8][2], theirs[8][2];
  int nMine, nTheirs, r, covered = 1;

  if (found <= 0 || found >= maxOut || !m->varPhr || !nt) return found;
  for (a = 0; a < found; a++)
    if (!out[a].method || strcmp(out[a].method, "referential") != 0) allRef = 0;
  if (allRef) return found;
  if (thVerbatim(m, text, &whole, 1, buf, bufLen, used) != 1) { *used = mark; return found; }
  ci = thHashGet(m->byName, whole.license, strlen(whole.license));
  if (ci < 0) { *used = mark; return found; }
  thSplit(whole.license, stem, sizeof(stem), v, sizeof(v), sf, sizeof(sf));
  /* a versioned or suffixed license -- the GNU documents -- is settled by
     which version and suffix the file states, which the windows read and
     the whole-file pass does not */
  if (v[0] || sf[0]) { *used = mark; return found; }
  if ((d = strchr(stem, '-')) != NULL) *((char *) d) = '\0';
  for (a = 0; a < found; a++) {
    int cf;
    if (!out[a].license) continue;
    thSplit(out[a].license, fstem, sizeof(fstem), v, sizeof(v), sf, sizeof(sf));
    if ((d = strchr(fstem, '-')) != NULL) *((char *) d) = '\0';
    if (strcmp(fstem, stem) == 0) { *used = mark; return found; }
    cf = thHashGet(m->byName, out[a].license, strlen(out[a].license));
    if (cf < 0) continue;
    if (m->conIn) {
      if (m->conIn[ci]) for (h = 0; m->conIn[ci][h]; h++)
        if (strcmp(m->conIn[ci][h], out[a].license) == 0) { *used = mark; return found; }
      if (m->conIn[cf]) for (h = 0; m->conIn[cf][h]; h++)
        if (strcmp(m->conIn[cf][h], whole.license) == 0) { *used = mark; return found; }
    }
    if (thNearPair(m, ci, cf)) { *used = mark; return found; }
  }
  len = strlen(nt);
  map = malloc(len + 1);
  if (!map) { *used = mark; return found; }
  nMine = m->bodyPhr[ci] ? thTextRuns(m, ci, nt, map, len, mine, 8) : 0;
  if (nMine == 0) { free(map); *used = mark; return found; }
  nTheirs = 0;
  for (a = 0; a < found && nTheirs < 8; a++) {
    int cf = out[a].license ? thHashGet(m->byName, out[a].license, strlen(out[a].license)) : -1;
    long runs[8][2];
    int n, k;
    if (cf < 0 || !m->bodyPhr[cf]) continue;
    n = thTextRuns(m, cf, nt, map, len, runs, 8);
    for (k = 0; k < n && nTheirs < 8; k++) { theirs[nTheirs][0] = runs[k][0]; theirs[nTheirs][1] = runs[k][1]; nTheirs++; }
  }
  free(map);
  if (nTheirs == 0) covered = 0;
  for (r = 0; r < nMine && covered; r++) {
    int k, any = 0;
    for (k = 0; k < nTheirs; k++) {
      long lo = mine[r][0] > theirs[k][0] ? mine[r][0] : theirs[k][0];
      long hi = mine[r][1] < theirs[k][1] ? mine[r][1] : theirs[k][1];
      if (hi - lo >= (mine[r][1] - mine[r][0]) / 2) { any = 1; break; }
    }
    if (!any) covered = 0;
  }
  if (covered) { *used = mark; return found; }
  out[found] = whole;
  return found + 1;
}

/* A second license granted by name beside another license's text.
   "Alternatively, this product may be distributed under the terms of the GNU
   General Public License" sits between a BSD notice's conditions and its
   disclaimer and grants the GPL as the other choice. The grant rows are
   otherwise a fallback, read where nothing was identified; beside a finding
   one stands where the sentence is the file's own and not wording of a found
   license's text -- the LGPL says "under the terms of the ordinary GNU General
   Public License" of itself. A version stated after the name is the head's to
   resolve, as for any referential row. */
static int thGrantedBeside(thModel *m, const char *nt, const thGrams *fg,
    thResult *out, int found, int maxOut, char *buf, size_t bufLen, size_t *used)
{
  static const char *grant[] = {"GPL-1.0-or-later", "LGPL-2.1-or-later",
                                "LGPL-2.0-or-later", "LGPL", NULL};
  unsigned char *covered;
  size_t len;
  int a, i, allRef = 1;

  if (found <= 0 || found >= maxOut || !m->nRef || !nt) return found;
  for (a = 0; a < found; a++)
    if (!out[a].method || strcmp(out[a].method, "referential") != 0) allRef = 0;
  if (allRef) return found;
  len = strlen(nt);
  covered = calloc(len + 1, 1);
  if (!covered) return found;
  for (a = 0; a < found; a++) {
    int ci, k;
    if (!out[a].license) continue;
    ci = thHashGet(m->byName, out[a].license, strlen(out[a].license));
    if (ci < 0 || !m->bodyPhr[ci]) continue;
    for (k = 0; m->bodyPhr[ci][k]; k++) {
      const char *ph = m->bodyPhr[ci][k];
      size_t pl = strlen(ph);
      const char *at = thGramsFind(fg, nt, ph);
      while (at) { memset(covered + (size_t) (at - nt), 1, pl); at = strstr(at + 1, ph); }
    }
  }
  for (i = 0; i < m->nRef && found < maxOut; i++) {
    const char *name = m->refName[i], *d;
    char stem[192], v[64], sf[16], fstem[192];
    size_t off = 0;
    int g, claimed = 0;
    for (g = 0; grant[g]; g++) if (strcmp(name, grant[g]) == 0) break;
    if (!grant[g] || (m->refWhere && m->refWhere[i] != 0)) continue;
    thSplit(name, stem, sizeof(stem), v, sizeof(v), sf, sizeof(sf));
    if ((d = strchr(stem, '-')) != NULL) *((char *) d) = '\0';
    for (a = 0; a < found && !claimed; a++) {
      if (!out[a].license) continue;
      thSplit(out[a].license, fstem, sizeof(fstem), v, sizeof(v), sf, sizeof(sf));
      if ((d = strchr(fstem, '-')) != NULL) *((char *) d) = '\0';
      if (strcmp(fstem, stem) == 0) claimed = 1;
    }
    if (claimed) continue;
    while (off < len) {
      regmatch_t hit;
      size_t so, eo, p, own = 0;
      if (thRegexec(((regex_t *) m->refRe) + i, m->refLit[i], 0, nt + off, 1, &hit) != 0) break;
      so = off + (size_t) hit.rm_so; eo = off + (size_t) hit.rm_eo;
      off = eo > off ? eo : off + 1;
      if (thVersionStated(nt + eo)) continue;
      for (p = so; p < eo; p++) if (covered[p]) own++;
      if (own * 2 > eo - so) continue;
      {
        size_t need = strlen(name) + 1;
        if (!buf || *used + need > bufLen) break;
        memcpy(buf + *used, name, need);
        memset(out + found, 0, sizeof(out[0]));
        out[found].license = buf + *used;
        out[found].status = "LOW";
        out[found].score = 0;
        out[found].method = "referential";
        *used += need;
        found++;
      }
      break;
    }
  }
  free(covered);
  return found;
}

/**
 * \brief Whether the declaration stands ahead of the license's own text.
 *
 * The GNU Free Documentation License prints, in its appendix, the very
 * sentence it asks a document to carry, so every copy of the license contains
 * the declaration and the wording cannot tell the two apart. Where it stands
 * can: the sentence a document writes about itself has the license after it,
 * and the sentence the license prints as an example has the license before it.
 * A file that carries the notice and not the license has nothing to weigh.
 */
static int thDeclaredAhead(thModel *m, const char *name, size_t where,
    const char *nt)
{
  int ci = thHashGet(m->byName, name, strlen(name)), k, total = 0, here = 0;
  size_t *pos = NULL, npos = 0, cap = 0, before = 0, i;
  if (ci < 0 || !m->bodyPhr || !m->bodyPhr[ci]) return 1;
  for (k = 0; m->bodyPhr[ci][k]; k++) {
    total++;
    if (strstr(nt, m->bodyPhr[ci][k])) here++;
  }
  if (!total || (float) here < TH_TEXT_SHARE * (float) total) return 1;
  /* where the license's own wording sits */
  for (k = 0; m->bodyPhr[ci][k]; k++) {
    const char *p2 = nt;
    while ((p2 = strstr(p2, m->bodyPhr[ci][k])) != NULL) {
      if (npos == cap) {
        size_t want = cap ? cap * 2 : 256;
        size_t *grown = realloc(pos, want * sizeof(size_t));
        if (!grown) { free(pos); return 1; }
        pos = grown; cap = want;
      }
      pos[npos++] = (size_t) (p2 - nt);
      p2++;
    }
  }
  for (i = 0; i < npos; i++) if (pos[i] < where) before++;
  free(pos);
  return npos - before >= before;
}

/**
 * \brief Whether the notice declares this variant where it stands.
 *
 * A declaration is written out in full even when it withdraws what the
 * variant is named for: a document with no invariant sections may still be
 * published with cover texts, and the sentence says so in the same breath.
 * The withdrawing word within reach of the phrase means the declaration was
 * not made.
 */
static int thDeclaredHere(thModel *m, const char *name, const char *phrase,
    const char *unless, const char *nt, int before)
{
  const char *at = nt;
  size_t plen = strlen(phrase);
  while ((at = strstr(at, phrase)) != NULL) {
    const char *end = at + plen;
    int withdrawn = 0;
    if (unless && *unless) {
      char reach[TH_DECLARED_REACH + 1];
      size_t left = strlen(end), take = left < TH_DECLARED_REACH ? left : TH_DECLARED_REACH;
      memcpy(reach, end, take);
      reach[take] = '\0';
      withdrawn = strstr(reach, unless) != NULL;
    }
    if (!withdrawn
        && (!before || thDeclaredAhead(m, name, (size_t) (at - nt), nt)))
      return 1;
    at = end;
  }
  return 0;
}



static int thCollect(thModel *m, const char *text, thResult *out, int maxOut,
    char *buf, size_t bufLen)
{
  const char **line;
  size_t n = 0, i;
  int found = 0, pass;
  size_t used = 0;
  /* The window carrying an exception clause usually names no license and is
     dropped below, so keep the strongest exception and attach it at the end. */
  const char *bestExc = NULL;
  const char *bestExcWin = NULL;      /* the clause names the license it modifies */
  size_t bestExcWinLen = 0;
  float bestExcScore = 0.0f;
  thNotice notice[TH_NOTICE_MAX];
  int nNotice = 0;
  /* The file normalised once, for every rule that reads it whole, and its
     run table once, for every rule that asks it for thousands of phrases. */
  char *fileNorm = NULL;
  thGrams fileGrams = {NULL, 0};
  int fileGramsBuilt = 0;
#define TH_FILE_NORM() (fileNorm ? fileNorm : (fileNorm = thNormalise(text)))
#define TH_FILE_GRAMS() (fileGramsBuilt ? &fileGrams \
    : (fileGramsBuilt = 1, thGramsBuild(&fileGrams, TH_FILE_NORM()), &fileGrams))

  if (buf && bufLen) buf[0] = '\0';

  line = thLineStarts(text, &n);
  if (!line) return 0;

  /* The wide pass reads a notice in context; the narrow one separates the
     licenses a multi-licensed header would lose to the argmax. */
  for (pass = 0; pass < 2; pass++) {
    size_t win = pass ? TH_FINE_LINES : TH_WIN_LINES;
    size_t step = pass ? TH_FINE_STEP : TH_WIN_STEP;
    /* a wide window holding less than this is a fragment, not a header */
    size_t least = pass ? 25u : 40u;
    size_t seen = 0;
    int read = 0; /* windows this pass actually scored */
    if (n <= win && pass) break; /* the wide pass already read it all */
    for (i = 0; i < n; i += step) {
      size_t end = i + win < n ? i + win : n;
      size_t len = (end < n ? (size_t) (line[end] - line[i]) : strlen(line[i]));
      char *w;
      thResult r;
      /* The floor rejects a window cut from a longer file. A file that is one
         window is no fragment, so it is read whole however short. */
      if (thStrippedLen(line[i], len) < least && n > win) continue;
      read++;
      if (pass && ++seen > TH_FINE_MAX) break; /* bound pathological files */
      char *wn = NULL; /* the window normalised, for the rules that read it */
      w = malloc(len + 1);
      if (!w) break;
      memcpy(w, line[i], len); w[len] = '\0';
      if (thClassifyN(m, w, &r, &wn) == 0 && r.exception &&
          (!bestExc || r.excScore > bestExcScore)) {
        bestExc = r.exception; bestExcScore = r.excScore;
        bestExcWin = line[i]; bestExcWinLen = len;
      }
      if (!r.license) {
        thResult extra[TH_EXPRESSION_MAX];
        int ne, e;
        ne = thChoiceWithoutHead(m, w, wn, extra, TH_EXPRESSION_MAX);
        for (e = 0; e < ne; e++) {
          extra[e].start = (long) (line[i] - text);
          extra[e].len = (long) len;
          thOffer(out, &found, maxOut, buf, bufLen, &used, extra + e);
        }
      }
      if (r.license) {
        thResult extra[TH_EXPRESSION_MAX];
        int ne, e;
        r.start = (long) (line[i] - text);
        r.len = (long) len;
        thOffer(out, &found, maxOut, buf, bufLen, &used, &r);
        if (!pass && r.grant > 0.0f && nNotice < TH_NOTICE_MAX
            && thHasSuffix(r.license, "-or-later")) {
          notice[nNotice].line = i;
          snprintf(notice[nNotice++].license, sizeof(notice[0].license), "%s", r.license);
        }
        /* A window that says it offers a choice grants every license it names,
           not only the one that won the argmax. */
        ne = thCoordinated(m, w, wn, r.license, extra, TH_EXPRESSION_MAX - 1);
        for (e = 0; e < ne; e++) {
          extra[e].start = r.start;
          extra[e].len = r.len;
          thOffer(out, &found, maxOut, buf, bufLen, &used, extra + e);
          /* granted by the window that named it beside its pick */
          if (!pass && r.grant > 0.0f && nNotice < TH_NOTICE_MAX
              && thHasSuffix(extra[e].license, "-or-later")) {
            notice[nNotice].line = i;
            snprintf(notice[nNotice++].license, sizeof(notice[0].license), "%s", extra[e].license);
          }
        }
      }
      free(w); free(wn);
      if (n <= win) break;
    }
    /* Every window fell under the floor -- many very short lines. Read the file
       whole rather than not at all. */
    if (!read && !pass) {
      thResult r;
      if (thClassify(m, text, &r) == 0 && r.license && found < maxOut) {
        size_t need = strlen(r.license) + 1;
        if (buf && used + need <= bufLen) {
          memcpy(buf + used, r.license, need);
          out[found] = r;
          out[found].license = buf + used;
          out[found].start = 0;
          out[found].len = 0;
          used += need;
          found++;
        }
      }
    }
  }
  /* Sort before resolving and truncating, so the limit keeps the strongest
     findings rather than whichever windows came first. */
  qsort(out, (size_t) found, sizeof(out[0]), thCmpFinding);
  /* Before the near-duplicate rules, which compare suffix siblings on
     overlapping text and cannot see which of them the file granted. */
  { const char *nt = TH_FILE_NORM();
    found = thGrantDecidesSuffix(m, nt, out, found, line, n, notice, nNotice); }
  free(line);
  /* A window is too short to separate a license from a variant of it, so the
     generic wins on score. Over the whole file the phrases decide. */
  if (found > 1 && m->varPhr && m->variant) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a, b, k, w = 0, at[TH_KEEP_MAX];
      unsigned char drop[TH_KEEP_MAX];
      memset(drop, 0, sizeof(drop));
      for (a = 0; a < found; a++) {
        at[a] = -1;
        for (b = 0; b < m->nClasses; b++)
          if (strcmp(m->classes[b], out[a].license) == 0) { at[a] = b; break; }
      }
      for (a = 0; a < found; a++) {
        float mine;
        if (at[a] < 0 || !m->variant[at[a]]) continue;
        mine = thOwnShare(m, at[a], nt);
        for (k = 0; m->variant[at[a]][k] >= 0 && !drop[a]; k++)
          for (b = 0; b < found; b++)
            if (at[b] == m->variant[at[a]][k]) {
              float theirs = thOwnShare(m, at[b], nt);
              if (theirs >= TH_VARIANT_SHARE && theirs > mine) drop[a] = 1;
              break;
            }
      }
      for (a = 0; a < found; a++) if (!drop[a]) w++;
      if (w > 0 && w < found) {
        w = 0;
        for (a = 0; a < found; a++) if (!drop[a]) out[w++] = out[a];
        found = w;
      }
    }
  }
  /* Drop a license whose whole case is another's text. Compared on where the
     phrases matched, since two licenses quoting one paragraph differ there.
     Shared phrases say the same words are here, not whose document they
     are: a PHP header names PHP-3.01, whose text carries the BSD conditions,
     and a BSD notice further down is not PHP's for that. It is where the
     covering license's own wording -- phrases few families share and this
     license lacks -- runs through the same text. */
  if (found > 1 && m->bodyPhr) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      size_t len = strlen(nt);
      unsigned char *covered = calloc(len + 1, 1);
      unsigned char *mine = calloc(len + 1, 1);
      int keptAt[TH_KEEP_MAX], nKept = 0;
      if (covered && mine) {
        int a, w = 0;
        for (a = 0; a < found; a++) {
          int ci = thHashGet(m->byName, out[a].license, strlen(out[a].license));
          int drop = 0, twin = 0, q2;
          /* A license's own suffix sibling is not another license whose text
             explains it: they are one document rendered twice, and which
             suffix the file granted is the suffix rules' question. */
          { char ba[192], va[64], sa[16];
            thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
            for (q2 = 0; q2 < nKept && !twin; q2++) {
              char bb[192], vb[64], sb[16];
              if (keptAt[q2] < 0) continue;
              thSplit(m->classes[keptAt[q2]], bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
              if (strcmp(ba, bb) == 0 && strcmp(va, vb) == 0) twin = 1;
            } }
          if (ci >= 0 && !twin && m->bodyPhr[ci] && m->nameVar
              && thNameEvidence(m, ci, text, nt) < m->thNameMin) {
            size_t total = 0, shared = 0, i2, lo = len, hi = 0;
            int k;
            memset(mine, 0, len + 1);
            for (k = 0; m->bodyPhr[ci][k]; k++) {
              const char *ph = m->bodyPhr[ci][k];
              size_t pl = strlen(ph);
              const char *at = strstr(nt, ph);
              while (at) {
                memset(mine + (size_t) (at - nt), 1, pl);
                at = strstr(at + 1, ph);
              }
            }
            for (i2 = 0; i2 < len; i2++) {
              if (!mine[i2]) continue;
              total++;
              if (covered[i2]) shared++;
              if (i2 < lo) lo = i2;
              if (i2 > hi) hi = i2;
            }
            if (total && (float) shared / (float) total >= m->thSubsumed) {
              /* the covering license's own wording within reach of this span */
              int q, near = 0;
              size_t from = lo > TH_SUBSUMED_REACH ? lo - TH_SUBSUMED_REACH : 0;
              size_t to = hi + TH_SUBSUMED_REACH;
              for (q = 0; q < nKept && !near; q++) {
                int kc = keptAt[q], j, own = 0;
                if (kc < 0 || !m->bodyPhr[kc]) continue;
                for (j = 0; m->bodyPhr[kc][j] && !near; j++) {
                  const char *ph = m->bodyPhr[kc][j], *at;
                  int r, theirs = 0;
                  if (m->bodyFam && m->bodyFam[kc] && m->bodyFam[kc][j] > TH_DISTINCT) continue;
                  for (r = 0; m->bodyPhr[ci][r]; r++)
                    if (!strcmp(m->bodyPhr[ci][r], ph)) { theirs = 1; break; }
                  if (theirs) continue;
                  own = 1;
                  at = strstr(nt + from, ph);
                  if (at && (size_t) (at - nt) <= to) near = 1;
                }
                /* one text under two names: nothing could tell them apart */
                if (!own) near = 1;
              }
              if (near) drop = 1;
            }
            if (!drop) {
              if (nKept < TH_KEEP_MAX) keptAt[nKept++] = ci;
              for (i2 = 0; i2 < len; i2++) if (mine[i2]) covered[i2] = 1;
              thCoverSuffixSibling(m, out[a].license, nt, covered);
            }
          } else if (ci >= 0 && m->bodyPhr[ci]) {
            int k;
            if (nKept < TH_KEEP_MAX) keptAt[nKept++] = ci;
            for (k = 0; m->bodyPhr[ci][k]; k++) {
              const char *ph = m->bodyPhr[ci][k];
              size_t pl = strlen(ph);
              const char *at = strstr(nt, ph);
              while (at) {
                memset(covered + (size_t) (at - nt), 1, pl);
                at = strstr(at + 1, ph);
              }
            }
            thCoverSuffixSibling(m, out[a].license, nt, covered);
          }
          if (!drop) out[w++] = out[a];
        }
        found = w;
      }
      free(covered); free(mine);
    }
  }
  /* A license named inside another license's text is mentioned there: EUPL's
     appendix lists the licenses it is compatible with, MPL-2.0 defines its
     Secondary Licenses by naming the GNU ones. A license whose own text is
     not here, and whose every name in the file falls inside a run of another
     license's text, is dropped; named anywhere outside such a run it stands. */
  if (found > 1 && m->bodyPhr && m->nameVar) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      size_t len = strlen(nt);
      long runs[TH_KEEP_MAX][TH_TEXT_RUNS_MAX][2];
      int nRuns[TH_KEEP_MAX], own[TH_KEEP_MAX];
      unsigned char *mine = calloc(len + 1, 1);
      int a, any = 0;
      if (mine) {
        for (a = 0; a < found; a++) {
          int ci = thHashGet(m->byName, out[a].license, strlen(out[a].license));
          nRuns[a] = 0; own[a] = 0;
          if (ci < 0 || !m->bodyPhr[ci]) continue;
          nRuns[a] = thTextRuns(m, ci, nt, mine, len, runs[a], TH_TEXT_RUNS_MAX);
          own[a] = nRuns[a] > 0;
          if (nRuns[a] > 0 && thBodyEvidence(m, ci, nt, 1e9f, 1) < m->thBodyMin * TH_IS_THE_TEXT)
            nRuns[a] = 0;   /* runs of its wording, but not its text */
          if (nRuns[a] > 0) any = 1;
        }
        if (any) {
          int w = 0;
          for (a = 0; a < found; a++) {
            int ci = thHashGet(m->byName, out[a].license, strlen(out[a].license));
            char fa[192], v[64], sf[16], fb[192];
            int k, inside = 1, named = 0, b, holders = 0;
            unsigned char holder[TH_KEEP_MAX];
            memset(holder, 0, sizeof(holder));
            if (ci < 0 || !m->nameVar[ci]) { out[w++] = out[a]; continue; }
            /* A licence's own text is here -- unless it is written inside a
               licence that holds it whole (the GPL at the end of the
               LGPL-3.0 document): then it is that document's text, and only
               a name outside it says the file grants it apart. */
            if (own[a]) {
              int r, all = 1;
              if (m->conIn && m->conIn[ci]) {
                int h;
                for (h = 0; m->conIn[ci][h]; h++)
                  for (b = 0; b < found; b++)
                    if (b != a && nRuns[b] > 0 && !strcmp(out[b].license, m->conIn[ci][h]))
                      { holder[b] = 1; holders = 1; }
              }
              if (!holders) { out[w++] = out[a]; continue; }
              for (r = 0; r < nRuns[a] && all; r++) {
                int fits = 0, q;
                for (b = 0; b < found && !fits; b++) {
                  if (!holder[b]) continue;
                  for (q = 0; q < nRuns[b]; q++)
                    if (runs[b][q][0] - TH_TEXT_GAP <= runs[a][r][0]
                        && runs[a][r][1] <= runs[b][q][1] + TH_TEXT_GAP) { fits = 1; break; }
                }
                if (!fits) all = 0;
              }
              if (!all) { out[w++] = out[a]; continue; }
            }
            thSplit(out[a].license, fa, sizeof(fa), v, sizeof(v), sf, sizeof(sf));
            for (k = 0; m->nameVar[ci][k] && inside; k++) {
              const char *ph = m->nameVar[ci][k], *at;
              size_t pl = strlen(ph);
              for (at = nt; (at = strstr(at, ph)) != NULL; at += pl) {
                long pos = (long) (at - nt);
                int covered = 0, r;
                if ((at != nt && at[-1] != ' ') || (at[pl] && at[pl] != ' ')) continue;
                named = 1;
                for (b = 0; b < found && !covered; b++) {
                  if (b == a || nRuns[b] == 0) continue;
                  if (holders && !holder[b]) continue;
                  thSplit(out[b].license, fb, sizeof(fb), v, sizeof(v), sf, sizeof(sf));
                  if (!holders && !strcmp(fa, fb)) continue;
                  for (r = 0; r < nRuns[b]; r++)
                    if (runs[b][r][0] <= pos && pos <= runs[b][r][1]) { covered = 1; break; }
                }
                if (!covered) { inside = 0; break; }
              }
            }
            if (!(named && inside)) out[w++] = out[a];
          }
          found = w;
        }
      }
      free(mine);
    }
  }
  /* A license claimed on wording that another license's text explains: a
     three-line window sees one sentence the head names a license for, while
     the file carries another license's text whole. The file decides, span
     against span: a license whose wording covers what the claim rests on and
     runs well beyond it, with its own phrases there, is the license that
     sentence belongs to. License-side shares cannot decide this -- a subset
     license is always wholly present -- so none is read; a sibling, or a
     license the claim is a near-duplicate of, is another rule's question;
     a claim the file names stands. */
  if (found > 0 && m->bodyPhr) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      size_t len = strlen(nt);
      unsigned char *mine = calloc(len + 1, 1), *theirs = calloc(len + 1, 1);
      unsigned char *others = calloc(len + 1, 1);
      const thGrams *fg = TH_FILE_GRAMS();
      if (mine && theirs && others) {
        int a, w = 0;
        for (a = 0; a < found; a++) {
          int ci = thHashGet(m->byName, out[a].license, strlen(out[a].license));
          int g, best = -1, k, dup = 0, b;
          double bestBeyond = 0.0;
          size_t total = 0, i2, lo = len, hi = 0, from, to;
          char stem[192], sb[192], v[64], sf[16], *d;
          if (ci < 0 || (m->isException && m->isException[ci]) || !m->bodyPhr[ci]
              || thNameEvidence(m, ci, text, nt) >= m->thNameMin) goto keep;
          memset(mine, 0, len + 1);
          for (k = 0; m->bodyPhr[ci][k]; k++) {
            const char *ph = m->bodyPhr[ci][k];
            size_t pl = strlen(ph);
            /* the run table says no for nearly every phrase of nearly every
               license without a search, and never says no wrongly */
            const char *at = thGramsFind(fg, nt, ph);
            while (at) { memset(mine + (size_t) (at - nt), 1, pl); at = strstr(at + 1, ph); }
          }
          for (i2 = 0; i2 < len; i2++) if (mine[i2]) { total++; if (i2 < lo) lo = i2; if (i2 > hi) hi = i2; }
          if (!total) goto keep;
          /* read within reach of the claim: in a file of many notices a
             license's phrases match text far from this one */
          from = lo > TH_SUBSUMED_REACH ? lo - TH_SUBSUMED_REACH : 0;
          to = hi + TH_SUBSUMED_REACH < len ? hi + TH_SUBSUMED_REACH : len - 1;
          thSplit(out[a].license, stem, sizeof(stem), v, sizeof(v), sf, sizeof(sf));
          if ((d = strchr(stem, '-')) != NULL) *d = '\0';
          for (g = 0; g < m->nClasses; g++) {
            size_t shared = 0, theirsN = 0, beyondN = 0;
            double beyond;
            if (g == ci || (m->isException && m->isException[g]) || !m->bodyPhr[g]) continue;
            thSplit(m->classes[g], sb, sizeof(sb), v, sizeof(v), sf, sizeof(sf));
            if ((d = strchr(sb, '-')) != NULL) *d = '\0';
            if (!strcmp(sb, stem)) continue;
            if (thNearPair(m, ci, g)) continue;
            /* Wording the file's other claims already account for is not
               what a third license runs "beyond" into: a license whose text
               bundles a BSD block and an X11 block covers a file carrying
               both, but the file said which each is. The candidate's own
               claim is not "another", nor is a sibling or near-duplicate of
               it, which carries the same text. */
            memset(others + from, 0, to - from + 1);
            for (b = 0; b < found; b++) {
              int co;
              char cb[192];
              if (b == a || !out[b].license || !strcmp(out[b].license, out[a].license)) continue;
              co = thHashGet(m->byName, out[b].license, strlen(out[b].license));
              if (co < 0 || co == g || !m->bodyPhr[co]) continue;
              thSplit(m->classes[co], cb, sizeof(cb), v, sizeof(v), sf, sizeof(sf));
              if ((d = strchr(cb, '-')) != NULL) *d = '\0';
              if (!strcmp(cb, sb) || thNearPair(m, co, g)) continue;
              for (k = 0; m->bodyPhr[co][k]; k++) {
                const char *ph = m->bodyPhr[co][k];
                size_t pl = strlen(ph);
                const char *at = thGramsFind(fg, nt, ph);
                while (at) { memset(others + (size_t) (at - nt), 1, pl); at = strstr(at + 1, ph); }
              }
            }
            /* only the reach is read, so only the reach is cleared; marks a
               phrase leaves outside it are never looked at */
            memset(theirs + from, 0, to - from + 1);
            for (k = 0; m->bodyPhr[g][k]; k++) {
              const char *ph = m->bodyPhr[g][k];
              size_t pl = strlen(ph);
              const char *at = thGramsFind(fg, nt, ph);
              while (at) { memset(theirs + (size_t) (at - nt), 1, pl); at = strstr(at + 1, ph); }
            }
            for (i2 = from; i2 <= to; i2++) {
              if (!theirs[i2]) continue;
              theirsN++;
              if (mine[i2]) shared++; else if (!others[i2]) beyondN++;
            }
            if (!theirsN || (double) shared / (double) total < TH_EXPLAINS_COVER) continue;
            beyond = (double) beyondN / (double) theirsN;
            if (beyond < TH_EXPLAINS_BEYOND || beyond <= bestBeyond) continue;
            if (!thOwnWordingNear(m, g, ci, nt, lo, hi)) continue;
            best = g; bestBeyond = beyond;
          }
          if (best >= 0) {
            out[a].license = m->classes[best];
            out[a].method = thMethodWith(out[a].method, "explained");
          }
keep:
          for (b = 0; b < w && !dup; b++)
            if (out[b].license && !strcmp(out[b].license, out[a].license)) dup = 1;
          if (!dup) out[w++] = out[a];
        }
        found = w;
      }
      free(mine); free(theirs); free(others);
    }
  }
  /* Between two members of one family, keep the one whose own distinctive text
     is present. A license reported alone is never removed. */
  if (m->reqPhr) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a, b, r;
      size_t len = strlen(nt);
      unsigned char *map = malloc(len + 1);
      for (a = 0; a < found && map; a++) {
        int ia = -1, beaten = 0, mine = 0, hasOwn;
        for (b = 0; b < m->nClasses; b++)
          if (strcmp(m->classes[b], out[a].license) == 0) { ia = b; break; }
        if (ia < 0 || !m->reqSib || !m->reqSib[ia]) continue;
        hasOwn = m->reqPhr[ia] != NULL;
        for (r = 0; hasOwn && m->reqPhr[ia][r]; r++)
          if (strstr(nt, m->reqPhr[ia][r])) { mine = 1; break; }
        if (mine) continue;
        { char bm[192], vm[64], sm[16];
          thSplit(out[a].license, bm, sizeof(bm), vm, sizeof(vm), sm, sizeof(sm));
        for (r = 0; m->reqSib[ia][r] && !beaten; r++) {
          int c, is = -1, sr, sibOwn = 0;
          char bs[192], vs[64], ss[16];
          /* -only and -or-later share one body, so this test cannot separate
             them; the grant and the name settle which was granted. */
          thSplit(m->reqSib[ia][r], bs, sizeof(bs), vs, sizeof(vs), ss, sizeof(ss));
          if (strcmp(bm, bs) == 0 && strcmp(vm, vs) == 0) continue;
          for (c = 0; c < found; c++)
            if (strcmp(out[c].license, m->reqSib[ia][r]) == 0) break;
          if (c == found) continue; /* sibling not reported */
          for (c = 0; c < m->nClasses; c++)
            if (strcmp(m->classes[c], m->reqSib[ia][r]) == 0) { is = c; break; }
          if (is < 0) continue;
          if (!m->reqPhr[is]) {
            /* a sibling with no distinctive phrases has none because this
               license contains all of it, so being reported is the evidence */
            if (hasOwn) { beaten = 1; break; }
            continue;
          }
          for (sr = 0; m->reqPhr[is][sr]; sr++)
            if (strstr(nt, m->reqPhr[is][sr])) { sibOwn = 1; break; }
          if (!sibOwn) continue;
          if (hasOwn) { beaten = 1; break; }
          /* A license with no wording of its own -- MIT beside the X11
             variant that adds "distribute with modifications" -- is the
             sibling's text read under the base name where the sibling's own
             wording is here and the two stand on one notice. */
          { long mr[TH_TEXT_RUNS_MAX][2], sr2[TH_TEXT_RUNS_MAX][2];
            int nm = m->bodyPhr[ia] ? thTextRuns(m, ia, nt, map, len, mr, TH_TEXT_RUNS_MAX) : 0;
            int ns = m->bodyPhr[is] ? thTextRuns(m, is, nt, map, len, sr2, TH_TEXT_RUNS_MAX) : 0;
            int i2, all = nm > 0 && ns > 0;
            for (i2 = 0; i2 < nm && all; i2++) {
              int j2, any = 0;
              for (j2 = 0; j2 < ns; j2++) {
                long lo = mr[i2][0] > sr2[j2][0] ? mr[i2][0] : sr2[j2][0];
                long hi = mr[i2][1] < sr2[j2][1] ? mr[i2][1] : sr2[j2][1];
                if (hi - lo >= (mr[i2][1] - mr[i2][0]) / 2) { any = 1; break; }
              }
              if (!any) all = 0;
            }
            /* named outside the sibling's text, it is granted apart */
            if (all && m->nameVar && m->nameVar[ia]) {
              int k;
              for (k = 0; m->nameVar[ia][k] && all; k++) {
                const char *ph = m->nameVar[ia][k];
                size_t pl = strlen(ph);
                const char *at = strstr(nt, ph);
                while (at && all) {
                  long pos = (long) (at - nt);
                  int inside = 0, j2;
                  if ((at == nt || at[-1] == ' ') && (at[pl] == '\0' || at[pl] == ' ')) {
                    for (j2 = 0; j2 < ns; j2++)
                      if (sr2[j2][0] <= pos && pos <= sr2[j2][1]) { inside = 1; break; }
                    if (!inside) all = 0;
                  }
                  at = strstr(at + 1, ph);
                }
              }
            }
            if (all) { beaten = 1; break; }
          }
        } }
        if (beaten) {
          int k;
          for (k = a; k < found - 1; k++) out[k] = out[k + 1];
          found--; a--;
        }
      }
      free(map);
    }
  }
  /* A member whose version the file states beats one whose version it does
     not, whatever the windows scored: "either version 3 of the License" with
     "See the GNU Library General Public License for more details" beneath
     it is LGPL-3.0, not the Library GPL the version-less name reaches. */
  { int a, b;
    for (a = 0; a < found; a++) {
      char ba[192], va[64], sa[16];
      int hasStated = 0, hasUnstated = 0, mineStated;
      thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
      if (!va[0]) continue;
      mineStated = thTextHasVersion(text, va);
      for (b = 0; b < found; b++) {
        char bb[192], vb[64], sb[16];
        thSplit(out[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
        if (strcmp(ba, bb) || !vb[0]) continue;
        if (thTextHasVersion(text, vb)) hasStated = 1; else hasUnstated = 1;
      }
      if (hasStated && hasUnstated && !mineStated) {
        int k;
        for (k = a; k < found - 1; k++) out[k] = out[k + 1];
        found--; a--;
      }
    } }
  /* A variant SPDX marks in the identifier -- GFDL-1.3-invariants-only beside
     GFDL-1.3-only -- shares the text with the plain member and differs by what
     the notice declares, read afterwards from the plain claim: the plain
     member stands for both. */
  { int a;
    for (a = 0; a < found; a++) {
      const char *mark, *lic = out[a].license;
      char plain[256];
      int b, k;
      if (!lic) continue;
      mark = strstr(lic, "-no-invariants-");
      if (!mark) mark = strstr(lic, "-invariants-");
      if (!mark) continue;
      snprintf(plain, sizeof(plain), "%.*s-%s", (int) (mark - lic), lic,
               mark + (strncmp(mark, "-no-", 4) == 0 ? 15 : 12));
      { char pb[192], pv[64], ps[16];
        thSplit(plain, pb, sizeof(pb), pv, sizeof(pv), ps, sizeof(ps));
        for (b = 0; b < found; b++) {
          char ob[192], ov[64], os[16];
          if (b == a || !out[b].license || strstr(out[b].license, "-invariants-")) continue;
          thSplit(out[b].license, ob, sizeof(ob), ov, sizeof(ov), os, sizeof(os));
          if (strcmp(ob, pb) == 0 && strcmp(ov, pv) == 0) break;
        } }
      if (b == found) continue;
      for (k = a; k < found - 1; k++) out[k] = out[k + 1];
      found--; a--;
    } }
  /* One claim per family: the members contradict each other, and independently
     scored windows produce both. Ties survive. Two suffixes of one version,
     each named on its own -- "Licensed under GPLv2" over an FSF notice
     granting "version 2 or later" -- are two grants the file makes, not one
     claim scored twice, and which scored higher is no reason to lose the
     other. */
  /* Decided over what entered, then removed in one pass: a member dropped
     while the others are still being judged changes what they are judged
     against, and the same file then answers differently for the order the
     windows happened to be scored in. */
  { int a, b, k;
    const char *nt = found > 1 ? TH_FILE_NORM() : NULL;
    unsigned char weaker[TH_KEEP_MAX];
    memset(weaker, 0, sizeof(weaker));
    for (a = 0; a < found && a < TH_KEEP_MAX; a++) {
      char ba[192], va[64], sa[16];
      int top = out[a].score, both = 0;
      thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
      for (b = 0; b < found; b++) {
        char bb[192], vb[64], sb[16];
        if (b == a) continue;
        thSplit(out[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
        if (strcmp(ba, bb) == 0 && out[b].score > top) top = out[b].score;
        if (nt && !both && strcmp(ba, bb) == 0 && strcmp(va, vb) == 0 && strcmp(sa, sb) != 0) {
          int ca = thHashGet(m->byName, out[a].license, strlen(out[a].license));
          int cb = thHashGet(m->byName, out[b].license, strlen(out[b].license));
          if (ca >= 0 && cb >= 0 && thNamedApart(m, ca, cb, text, nt)
              && thNamedApart(m, cb, ca, text, nt))
            both = 1;
        }
      }
      weaker[a] = (unsigned char) (out[a].score < top && !both);
    }
    for (a = 0, k = 0; a < found; a++)
      if (a >= TH_KEEP_MAX || !weaker[a]) out[k++] = out[a];
    found = k; }
  /* Ties between versions of a family break on the one the file names: a
     notice granting "Version 2 or later" also yields the versionless reading
     at the same confidence, and the version it states is the one it means.
     Only versions: a name says which version, never which suffix. */
  if (found > 1) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a, k;
      unsigned char outshone[TH_KEEP_MAX];
      memset(outshone, 0, sizeof(outshone));
      for (a = 0; a < found && a < TH_KEEP_MAX; a++) {
        char ba[192], va[64], sa[16];
        int b, ci = -1, versions = 0;
        float mine, best;
        thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
        for (b = 0; b < found; b++) {
          char bb[192], vb[64], sb[16];
          thSplit(out[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
          if (strcmp(ba, bb) == 0 && strcmp(va, vb) != 0) versions = 1;
        }
        if (!versions) continue;
        for (ci = 0; ci < m->nClasses; ci++)
          if (strcmp(m->classes[ci], out[a].license) == 0) break;
        if (ci >= m->nClasses) continue;
        mine = best = thNameEvidence(m, ci, text, nt);
        for (b = 0; b < found; b++) {
          char bb[192], vb[64], sb[16];
          int cj;
          if (b == a) continue;
          thSplit(out[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
          if (strcmp(ba, bb) != 0) continue;
          for (cj = 0; cj < m->nClasses; cj++)
            if (strcmp(m->classes[cj], out[b].license) == 0) break;
          if (cj < m->nClasses) {
            float ev = thNameEvidence(m, cj, text, nt);
            if (ev > best) best = ev;
          }
        }
        outshone[a] = (unsigned char) (mine < best);
      }
      for (a = 0, k = 0; a < found; a++)
        if (a >= TH_KEEP_MAX || !outshone[a]) out[k++] = out[a];
      found = k;
    }
  }
  /* No wording separates a licence from one that contains it, so nothing in
     the file can evidence the inner over the outer and both stand on one
     notice. What the file does say is which it grants, and a licence only
     named loses to one that is granted. Granting both or neither decides
     nothing, and the pair stands. */
  if (m->conIn && found > 1) {
    int a, keep = 0;
    char *dropped = calloc((size_t) found, 1);
    if (dropped) {
      for (a = 0; a < found; a++) {
        int ia = -1, b, r;
        for (b = 0; b < m->nClasses; b++)
          if (strcmp(m->classes[b], out[a].license) == 0) { ia = b; break; }
        if (ia < 0 || !m->conIn[ia]) continue;
        for (r = 0; m->conIn[ia][r]; r++)
          for (b = 0; b < found; b++) {
            if (b == a || strcmp(out[b].license, m->conIn[ia][r]) != 0) continue;
            if (out[b].grant > 0.0f && !(out[a].grant > 0.0f)) dropped[a] = 1;
            else if (out[a].grant > 0.0f && !(out[b].grant > 0.0f)) dropped[b] = 1;
          }
      }
      for (a = 0; a < found; a++) if (!dropped[a]) keep++;
      if (keep && keep < found) {
        int w = 0;
        for (a = 0; a < found; a++) if (!dropped[a]) out[w++] = out[a];
        found = w;
      }
      free(dropped);
    }
  }
  /* A licence written inside another shares every word with it, so the larger
     identifier wins the ranking on a notice that only ever stated the smaller
     one. What the holder says beyond it decides: none of that wording here
     means the holder was never written. The inner licence's own body must
     still be present, or a bare mention of the holder would be renamed, and
     where two licences it holds would both fit nothing separates them. A file
     that names the holder states it outright, and the phrases here are its
     body rather than its name, so the name is tested separately. */
  if (m->conDist && m->conNear && m->conIn && found > 0) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a;
      for (a = 0; a < found; a++) {
        int inner, fits = -1, many = 0, self = -1;
        for (inner = 0; inner < m->nClasses; inner++)
          if (strcmp(m->classes[inner], out[a].license) == 0) { self = inner; break; }
        if (self >= 0 && thNameEvidence(m, self, text, nt) >= m->thNameMin)
          continue;
        for (inner = 0; inner < m->nClasses && !many; inner++) {
          int h, b, seen = 0;
          if (!m->conNear[inner] || !m->conDist[inner]) continue;
          for (b = 0; b < found; b++)
            if (strcmp(out[b].license, m->classes[inner]) == 0) { seen = 1; break; }
          if (seen) continue; /* already claimed on its own */
          /* On the tight relation only: this rule reads the absence of the
             holder's own wording, and at a looser relation that wording is
             mostly what renderings vary, so absence proves nothing. The
             phrases align with the near relation, so find the holder there. */
          if (!m->conIn[inner]) continue;
          for (h = 0; m->conNear[inner][h]; h++) {
            int k, held = 0, tight = 0;
            if (strcmp(m->conNear[inner][h], out[a].license) != 0) continue;
            for (k = 0; m->conIn[inner][k]; k++)
              if (strcmp(m->conIn[inner][k], out[a].license) == 0) { tight = 1; break; }
            if (!tight || !m->conDist[inner][h]) continue;
            for (k = 0; m->conDist[inner][h][k]; k++)
              if (strstr(nt, m->conDist[inner][h][k])) { held = 1; break; }
            if (held) continue; /* the holder's own text is here */
            if (thBodyEvidence(m, inner, nt, m->thBodyMin, 0) < m->thBodyMin)
              continue;
            if (fits >= 0) { many = 1; break; }
            fits = inner;
          }
        }
        if (fits >= 0 && !many) {
          out[a].license = m->classes[fits];
          out[a].method = thMethodWith(out[a].method, "contained");
        }
      }
    }
  }
  /* The mirror: a licence held by another shares all its words with it, so
     on a file carrying the larger text the smaller one is wholly present too
     and, being shorter, reads as the better match. What settles it is
     whether the larger licence's own wording is on the page. A file that
     names the smaller licence has said which it means, and where two hold it
     nothing chooses between them. */
  if (m->conDist && m->conNear && found > 0) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a;
      for (a = 0; a < found; a++) {
        int lifted = thHolderWrittenHere(m, out, found, a, text, nt);
        if (lifted >= 0) {
          out[a].license = m->classes[lifted];
          out[a].method = thMethodWith(out[a].method, "container");
        }
      }
    }
  }
  /* A licence named but not quoted cannot settle which version it is. */
  if (found > 0) {
    const char *nt = TH_FILE_NORM();
    if (nt) found = thLaterIsWritten(m, nt, out, found);
  }
  if (m->unver && found > 0) {
    const char *nt = TH_FILE_NORM();
    if (nt) found = thNoVersionFromAName(m, text, nt, out, found);
  }
  /* Text stating an exception and granting almost nothing is describing the
     exception, not granting the license it names in order to modify. */
  if (bestExc && found > 0 && m->thExcGrant >= 0.0f) {
    const char *nt2 = TH_FILE_NORM();
    if (nt2) {
      float d[TH_DENSE];
      int a, kept = 0;
      thDense(m, nt2, d);
      if (d[6] <= m->thExcGrant) {
        for (a = 0; a < found; a++) {
          int ci, isExc = 0;
          for (ci = 0; ci < m->nClasses; ci++)
            if (strcmp(m->classes[ci], out[a].license) == 0) {
              isExc = m->isException && m->isException[ci];
              break;
            }
          if (isExc) out[kept++] = out[a];
        }
        found = kept;
      }
    }
  }
  /* Nothing could be identified. The file may still say that a license applies
     and where to find it, which is what a reviewer needs. */
  if (found == 0 && bestExc) {
    /* An exception quoted without the license it modifies is a real
       identification, but uncalibrated, so it claims no match percentage. */
    size_t need = strlen(bestExc) + 1;
    if (buf && used + need <= bufLen) {
      memcpy(buf + used, bestExc, need);
      memset(out, 0, sizeof(out[0]));
      out[0].license = buf + used;
      out[0].status = "LOW";
      out[0].score = 0;
      out[0].method = "exception-only";
      out[0].exception = bestExc;
      out[0].excScore = bestExcScore;
      used += need;
      found = 1;
    }
  }
  /* Naming both sides of a choice says all the marker would, and beside them
     it reads as a third license. It survives only where nothing was named. */
  if (found == 0) {
    /* A license text longer than one window grants nothing in any of them,
       while the file read whole is the license and says so plainly. It is
       scored on the same terms as a window, so it is reported only if it
       would have been reported had it fitted in one. */
    thResult whole;
    /* Only where reading it whole can still carry: past this the notice is a
       fraction of the file and the verifier will not clear it, so the work is
       spent for nothing. */
    if (strlen(text) <= TH_WHOLE_MAX
        && thClassify(m, text, &whole) == 0 && whole.license) {
      whole.start = 0;
      whole.len = (long) strlen(text);
      thOffer(out, &found, maxOut, buf, bufLen, &used, &whole);
    }
  }
  if (found == 0) {
    /* The file may still carry a license's text in full, and failing that may
       say where to look. */
    found = thVerbatim(m, text, out, maxOut, buf, bufLen, &used);
      if (found == 0)
      found = thReferential(m, text, out, maxOut, buf, bufLen, &used);
  }
  if (found > 0) {
    const char *nt = TH_FILE_NORM();
    int a, allRef = 1, onlyAt = -1, restAt = -1;
    found = thGrantedBeside(m, nt, TH_FILE_GRAMS(), out, found, maxOut, buf, bufLen, &used);
    found = thVerbatimBeside(m, text, nt, out, found, maxOut, buf, bufLen, &used);
    /* A grant found beside an exception quoted on its own is the license the
       exception modifies: "distributed under the terms of the GNU General
       Public License. As a special exception ..." */
    for (a = 0; a < found; a++) {
      if (!out[a].method || strcmp(out[a].method, "referential") != 0) allRef = 0;
      if (out[a].method && strcmp(out[a].method, "exception-only") == 0) {
        if (onlyAt < 0) onlyAt = a;
      } else if (restAt < 0) {
        restAt = a;
      }
    }
    if (!allRef && onlyAt >= 0 && restAt >= 0) {
      int withExc = 0;
      for (a = 0; a < found; a++)
        if (a != onlyAt && out[a].exception) withExc = 1;
      if (!withExc) {
        out[restAt].exception = out[onlyAt].exception;
        out[restAt].excScore = out[onlyAt].excScore;
        for (a = onlyAt; a + 1 < found; a++) out[a] = out[a + 1];
        found--;
      }
    }
  }
  /* After the fallbacks: the conditions rule a license out without naming
     another, so a replacement would be a guess. */
  if (found > 0 && m->clauseSig && m->nClause > 0) {
    /* Normalising the whole file is the expensive part, so only where a
       finding could be judged by its conditions at all: most files carry no
       licence from a family that is read that way. */
    int a, judgeable = 0;
    for (a = 0; a < found && !judgeable; a++) {
      int b;
      if (!out[a].license) continue;
      for (b = 0; b < m->nClasses; b++)
        if (strcmp(m->classes[b], out[a].license) == 0) {
          judgeable = m->clauseSig[b] != NULL
              || (m->clauseCount && m->clauseCount[b] > 0)
              || (m->clauseSpan && m->clauseSpan[b] > 0);
          break;
        }
    }
    if (judgeable) {
      const char *nt = TH_FILE_NORM();
      char vetoed[TH_KEEP_MAX][192];
      int nVetoed = found < TH_KEEP_MAX ? found : TH_KEEP_MAX;
      for (a = 0; a < nVetoed; a++)
        snprintf(vetoed[a], sizeof(vetoed[a]), "%s", out[a].license ? out[a].license : "");
      if (nt) found = thClauseMismatch(m, text, nt, out, found);
      /* -- except the one the file's own text makes. A head finding the
         conditions rule out kept the fallbacks from running, and what the
         file carries in full, read within those conditions, is what they
         exist to find. Not a license the ruled-out one holds: a notice
         missing one marker reads as lacking that condition, and the smaller
         license the text then fits is only the part the two share. */
      /* Unless the notice says the condition is gone -- "3. [Deleted]" in
         a Berkeley notice after the 1999 change -- when the smaller license
         is what it states. */
      if (found == 0 && nVetoed > 0) {
        found = thVerbatim(m, text, out, maxOut, buf, bufLen, &used);
        if (found > 0) {
          int ci = thHashGet(m->byName, out[0].license, strlen(out[0].license));
          int held = 0, h, v;
          if (ci >= 0 && m->conNear && m->conNear[ci])
            for (h = 0; m->conNear[ci][h] && !held; h++)
              for (v = 0; v < nVetoed; v++)
                if (strcmp(m->conNear[ci][h], vetoed[v]) == 0) { held = 1; break; }
          if (held && !thConditionsGone(nt)) found = 0;
        }
        if (found == 0)
          found = thReferential(m, text, out, maxOut, buf, bufLen, &used);
      } else if (found > 0 && nVetoed > 0 && found < maxOut) {
        /* A license ruled out beside another that stands -- the Berkeley
           notice with its clause deleted, under a GPL grant -- is replaced
           the same way, by the text within its family only: the other
           finding is not the file's answer to it. */
        int gone[TH_FAMILY_MAX], nGone = 0, v, fi;
        for (v = 0; v < nVetoed; v++) {
          int still = 0, g;
          fi = vetoed[v][0] ? thConditionsFamily(m, vetoed[v]) : -1;
          if (fi < 0) continue;
          for (a = 0; a < found && !still; a++)
            if (out[a].license && thConditionsFamily(m, out[a].license) == fi) still = 1;
          for (g = 0; g < nGone && !still; g++)
            if (gone[g] == fi) still = 1;
          if (!still && nGone < TH_FAMILY_MAX) gone[nGone++] = fi;
        }
        if (nGone > 0) {
          thResult more[1];
          int n = thVerbatim(m, text, more, 1, buf, bufLen, &used), g, held = 0, h;
          if (n > 0) {
            int ci = thHashGet(m->byName, more[0].license, strlen(more[0].license));
            if (ci >= 0 && m->conNear && m->conNear[ci])
              for (h = 0; m->conNear[ci][h] && !held; h++)
                for (v = 0; v < nVetoed; v++)
                  if (strcmp(m->conNear[ci][h], vetoed[v]) == 0) { held = 1; break; }
            if (held && !thConditionsGone(nt)) n = 0;
          }
          for (v = 0; n > 0 && v < nVetoed; v++)
            if (strcmp(vetoed[v], more[0].license) == 0) n = 0; /* the name ruled out itself */
          if (n > 0 && more[0].method && strcmp(more[0].method, "verbatim") == 0) {
            fi = thConditionsFamily(m, more[0].license);
            for (g = 0; g < nGone; g++)
              if (gone[g] == fi) { out[found++] = more[0]; break; }
          }
        }
      }
    }
  }
  /* The mirror again, after the fallbacks: a licence held by another shares all its words with it, so
     on a file carrying the larger text the smaller one is wholly present too
     and, being shorter, reads as the better match. What settles it is
     whether the larger licence's own wording is on the page. A file that
     names the smaller licence has said which it means, and where two hold it
     nothing chooses between them. */
  if (m->conDist && m->conNear && found > 0) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a;
      for (a = 0; a < found; a++) {
        int lifted = thHolderWrittenHere(m, out, found, a, text, nt);
        if (lifted >= 0) {
          out[a].license = m->classes[lifted];
          out[a].method = thMethodWith(out[a].method, "container");
        }
      }
    }
  }
  /* Again after the fallbacks, which reach the same name a second time. */
  if (m->unver && found > 0) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a, w = thNoVersionFromAName(m, text, nt, out, found);
      /* A name dropped here for stating no version leaves the file with
         nothing, though it may still say where its licence is. The pointer
         is what remains -- the name itself is not re-derived. */
      if (found > 0 && w == 0) {
        static const char *see[] = {"Same-license-as", "See-file.LICENSE", "See-file.COPYING",
            "See-file.README", "See-URL", "See-file", "See-doc.OTHER", NULL};
        int n = thReferential(m, text, out, maxOut, buf, bufLen, &used), k;
        for (a = 0; a < n; a++) {
          int isSee = 0;
          for (k = 0; see[k]; k++)
            if (out[a].license && strcmp(out[a].license, see[k]) == 0) { isSee = 1; break; }
          if (isSee) out[w++] = out[a];
        }
      }
      found = w;
    }
  }
  /* Name the identifier the notice declares, where the text cannot. SPDX
     publishes one set of words under several identifiers when what separates
     them is declared beside the notice rather than written in it: OFL-1.1 and
     OFL-1.1-RFN are the same licence word for word, and only the font's
     copyright line says which one applies. A tag is already the author saying
     which identifier applies, and is left as written. */
  if ((m->nDecl > 0 || m->nLicensor > 0) && found > 0) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a, w = 0;
      for (a = 0; a < found; a++) {
        int j, b, dup = 0;
        if (out[a].license && !(out[a].method
                                && strcmp(out[a].method, "spdx-tag") == 0)) {
          for (j = 0; j < m->nDecl; j++) {
            int ci;
            if (strcmp(out[a].license, m->declBase[j]) != 0) continue;
            ci = thHashGet(m->byName, m->declTo[j], strlen(m->declTo[j]));
            if (ci >= 0
                && thDeclaredHere(m, out[a].license, m->declPhr[j],
                                  m->declUnless[j], nt, m->declAhead[j])) {
              out[a].license = m->classes[ci];
              out[a].method = thMethodWith(out[a].method, "declared");
              break;
            }
          }
          /* A licensor's name inside the terms is the whole difference, so
             the name decides in both directions: the X11 words naming the
             XFree86 Project are XFree86-1.0, and naming anyone else, X11. */
          for (j = 0; j < m->nLicensor; j++) {
            int ci;
            const char *from = out[a].license, *into = NULL;
            if (strcmp(from, m->licBase[j]) == 0 && strstr(nt, m->licPhr[j]))
              into = m->licTo[j];
            else if (strcmp(from, m->licTo[j]) == 0 && !strstr(nt, m->licPhr[j]))
              into = m->licBase[j];
            if (!into) continue;
            ci = thHashGet(m->byName, into, strlen(into));
            if (ci >= 0) {
              out[a].license = m->classes[ci];
              out[a].method = thMethodWith(out[a].method, "licensor");
            }
            break;
          }
        }
        /* A window that read the base and another that read the declared
           identifier are the same finding once the notice has been read. */
        for (b = 0; b < w; b++) {
          const char *la = out[a].license, *lb = out[b].license;
          const char *ea = out[a].exception, *eb = out[b].exception;
          if (!la != !lb || (la && strcmp(la, lb))) continue;
          if (!ea != !eb || (ea && strcmp(ea, eb))) continue;
          dup = 1;
          break;
        }
        if (!dup) out[w++] = out[a];
      }
      found = w;
    }
  }
  /* An exception modifies the license it was granted alongside, so it goes on
     the strongest finding -- unless a window already reported one there. */
  if (bestExc && found > 0) {
    int a, any = 0, at = 0;
    for (a = 0; a < found; a++) if (out[a].exception) { any = 1; break; }
    /* On the license the clause names, not on the strongest finding: "As a
       special exception to the GNU General Public License" says which license
       it modifies, and ltmain.sh grants MIT as well. */
    if (!any && bestExcWin && bestExcWinLen) {
      char *w = malloc(bestExcWinLen + 1);
      if (w) {
        char *wn;
        memcpy(w, bestExcWin, bestExcWinLen);
        w[bestExcWinLen] = '\0';
        wn = thNormalise(w);
        if (wn) {
          for (a = 0; a < found; a++) {
            int ci = thHashGet(m->byName, out[a].license, strlen(out[a].license));
            if (ci >= 0 && thNameSpan(m, ci, w, wn) > 0) { at = a; break; }
          }
          free(wn);
        }
        free(w);
      }
    }
    if (!any) { out[at].exception = bestExc; out[at].excScore = bestExcScore; }
  }
  /* An exception is told from a variant of it by the same test as a license.
     Over the whole file: the sentence that separates them may be elsewhere. */
  if (found > 0 && m->varPhr) {
    const char *nt = TH_FILE_NORM();
    if (nt) {
      int a;
      for (a = 0; a < found; a++) {
        int at, swapped = 0, into;
        if (!out[a].exception) continue;
        at = thHashGet(m->byName, out[a].exception, strlen(out[a].exception));
        if (at < 0) continue;
        into = thPreferVariant(m, at, nt, &swapped);
        if (!swapped) into = at;
        /* The mirror rule, on the exception axis: the exception head prefers
           the shorter of two that read alike, and the shorter is often
           written inside the longer. The holder's own wording on the page
           settles it, as for a license. */
        if (m->conDist && m->conNear && m->conNear[into] && m->conDist[into]
            && thNameEvidence(m, into, text, nt) < m->thNameMin) {
          int h, fits = 0, fit = -1;
          for (h = 0; m->conNear[into][h]; h++) {
            int oi = thHashGet(m->byName, m->conNear[into][h], strlen(m->conNear[into][h]));
            char **own = m->conDist[into][h];
            int n = 0, here = 0, k;
            if (oi < 0 || !m->isException || !m->isException[oi] || !own) continue;
            for (k = 0; own[k]; k++) { n++; if (strstr(nt, own[k])) here++; }
            if (n && (float) here >= TH_VERBATIM * (float) n) { fits++; fit = oi; }
          }
          if (fits == 1) { into = fit; swapped = 1; }
        }
        if (swapped) {
          /* An exception reported on its own is carried on both fields, so
             both move or the finding names a pair that does not exist. */
          if (out[a].license && strcmp(out[a].license, out[a].exception) == 0)
            out[a].license = m->classes[into];
          out[a].exception = m->classes[into];
        }
      }
    }
  }
  /* A declared license answers for the family it names and no other. It
     carries no match percentage, as ojo records for the same tag. */
  /* Beside the dedication the pointer is redundant; beside another license
     it stands, as a tag would. */
  { const char *nt = TH_FILE_NORM();
    int instrument = 0, a;
    /* "released to the public domain, as explained at
       creativecommons.org/publicdomain": the dedication and the instrument
       it was made through are one statement, and the instrument names it */
    for (a = 0; a < found && !instrument; a++) {
      int ci = out[a].license ? thHashGet(m->byName, out[a].license, strlen(out[a].license)) : -1;
      int k;
      if (ci < 0 || !m->nameVar || !m->nameVar[ci]) continue;
      for (k = 0; m->nameVar[ci][k]; k++)
        if (strstr(m->nameVar[ci][k], "public domain") || strstr(m->nameVar[ci][k], "publicdomain"))
          { instrument = 1; break; }
    }
    if (nt && !instrument && thDedication(m, text, nt)) {
      thResult merged[TH_KEEP_MAX];
      int w = 0, a;
      const char *name = "Public-domain";
      int ci = thHashGet(m->byName, name, strlen(name));
      memset(merged, 0, sizeof(merged[0]));
      merged[w].license = ci >= 0 ? m->classes[ci] : name;
      merged[w].status = "HIGH";
      merged[w].method = "dedication";
      /* the line that says it, so a reviewer is shown where */
      { const char *at = text, *hit = NULL;
        size_t tl = strlen(text), i;
        for (i = 0; i + 13 <= tl && !hit; i++)
          if (strncasecmp(text + i, "public domain", 13) == 0) hit = text + i;
        if (hit) {
          const char *bol = hit, *eol = hit;
          while (bol > at && bol[-1] != '\n') bol--;
          while (*eol && *eol != '\n') eol++;
          merged[w].start = (long) (bol - at);
          merged[w].len = (long) (eol - bol);
        } }
      w++;
      for (a = 0; a < found && w < maxOut; a++) {
        if (out[a].license && (strcmp(out[a].license, "Public-domain") == 0
                               || strcmp(out[a].license, "Public-domain-ref") == 0))
          continue;
        merged[w++] = out[a];
      }
      for (a = 0; a < w; a++) out[a] = merged[a];
      found = w;
    } }
  { thResult tag[TH_MAX_FINDINGS];
    int nt = thSpdxTags(m, text, tag, TH_MAX_FINDINGS);
    if (nt == 0) nt = thManifestTags(m, text, tag, TH_MAX_FINDINGS);
    if (nt > 0) {
      thResult merged[TH_KEEP_MAX];
      int w = 0, a, b;
      for (a = 0; a < nt && w < maxOut; a++) merged[w++] = tag[a];
      for (a = 0; a < found && w < maxOut; a++) {
        char ba[192], va[64], sa[16], bb[192], vb[64], sb[16];
        int drop = 0;
        thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
        for (b = 0; b < nt && !drop; b++) {
          thSplit(tag[b].license, bb, sizeof(bb), vb, sizeof(vb), sb, sizeof(sb));
          if (strcmp(ba, bb) == 0) drop = 1;
          else if (tag[b].exception &&
                   strcmp(out[a].license, tag[b].exception) == 0) drop = 1;
        }
        if (!drop) merged[w++] = out[a];
      }
      for (a = 0; a < w; a++) out[a] = merged[a];
      found = w;
    } }
  /* A module's declaration fills in what nothing else in the file said.
     Unlike an SPDX tag it names a family, not a version's constraint, so it
     is the answer only where the file's own words gave none for that
     family: a header stating LGPL-2.1-or-later beside MODULE_LICENSE("LGPL")
     is reported as the header reads. */
  { const char *fam[2] = {NULL, NULL};
    int nf = thModuleLicence(text, fam), a, k;
    for (k = 0; k < nf && found < maxOut; k++) {
      int present = 0;
      char bf[192], vf[64], sf[16], ba[192], va[64], sa[16];
      thSplit(fam[k], bf, sizeof(bf), vf, sizeof(vf), sf, sizeof(sf));
      for (a = 0; a < found && !present; a++) {
        if (!out[a].license) continue;
        thSplit(out[a].license, ba, sizeof(ba), va, sizeof(va), sa, sizeof(sa));
        if (strcmp(ba, bf) == 0) present = 1;
      }
      if (present) continue;
      memset(out + found, 0, sizeof(out[0]));
      out[found].license = fam[k];
      out[found].status = "HIGH";
      out[found].method = "module-licence";
      found++;
    } }
  free(fileNorm);
  thGramsFree(&fileGrams);
#undef TH_FILE_NORM
#undef TH_FILE_GRAMS
  return found;
}

/**
 * \brief Every license the text grants, strongest first.
 *
 * Collects without a limit and truncates at the end, or the cap would keep
 * whichever findings arrived first.
 */
/**
 * \brief The part of a file that speaks for the file.
 *
 * A gettext catalogue carries a program's strings and their translations, and
 * among them the program's own license line -- "License GPLv3+: GNU GPL
 * version 3 or later" -- which is what the program prints, not what the
 * catalogue is under. Only the translator's comments speak for the file, so
 * the rest is blanked -- blanked, not cut, so every offset still points where
 * it did.
 *
 * \return a copy to free, or NULL where the text is not a catalogue
 */
/**
 * \brief Blank the separators Unicode calls whitespace, in place.
 *
 * A document may separate its words with any of them -- a non-breaking space
 * between "Version" and "2.0" is common in documentation converted from HTML
 * -- and which one a typographer chose is not a licensing fact. Every byte of
 * the sequence becomes a space, so the text keeps its length and every offset
 * still points where it did. The set is what Python's `\s` matches beyond
 * ASCII, since the reference reads the raw text with it.
 */
static void thBlankUnicodeSpaces(char *s)
{
  unsigned char *p = (unsigned char *) s;
  while (*p) {
    size_t n = 0;
    if (p[0] == 0xC2 && (p[1] == 0x85 || p[1] == 0xA0))
      n = 2;                                              /* NEL, no-break space */
    else if (p[0] == 0xE1 && p[1] == 0x9A && p[2] == 0x80)
      n = 3;                                              /* ogham space mark */
    else if (p[0] == 0xE2 && p[1] == 0x80 && p[2] &&
             ((p[2] >= 0x80 && p[2] <= 0x8A) || p[2] == 0xA8 || p[2] == 0xA9
              || p[2] == 0xAF))
      n = 3;                     /* en/em spaces, line and paragraph, narrow nbsp */
    else if (p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0x9F)
      n = 3;                                              /* medium mathematical */
    else if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80)
      n = 3;                                              /* ideographic space */
    else if (p[0] >= 0x1C && p[0] <= 0x1F)
      n = 1;                                              /* the file separators */
    if (n) { size_t k; for (k = 0; k < n; k++) p[k] = ' '; p += n; }
    else p++;
  }
}

static char *thOwnWords(const char *text)
{
  static const char head[] = "msgid \"\"\nmsgstr \"\"\n";
  const char *at;
  char *own, *w;
  const char *p;
  own = malloc(strlen(text) + 1);
  if (!own) return NULL;
  memcpy(own, text, strlen(text) + 1);
  thBlankUnicodeSpaces(own);
  at = strstr(own, head);
  if (!at || (at > own && at[-1] != '\n')) return own;
  if (!strstr(own, "\"Project-Id-Version:") && !strstr(own, "\"Content-Type:"))
    return own;
  /* A gettext catalogue carries a program's strings, and among them the
     licence line the program prints -- which is not the catalogue's own.
     Only the translator's comments speak for the file. */
  for (p = own, w = own; *p;) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t) (nl - p) : strlen(p);
    int keep = (len == 1 && p[0] == '#') || (len >= 2 && p[0] == '#' && p[1] == ' ');
    if (keep) { memcpy(w, p, len); w += len; }
    if (nl) *w++ = '\n';
    p += len + (nl ? 1 : 0);
  }
  *w = '\0';
  return own;
}

int thClassifyAll(thModel *m, const char *text, thResult *out, int maxOut,
    char *buf, size_t bufLen)
{
  thResult keep[TH_KEEP_MAX];
  char kbuf[4096];
  size_t used = 0;
  int n, i, found = 0;
  char *own = thOwnWords(text);

  if (maxOut > TH_MAX_FINDINGS) maxOut = TH_MAX_FINDINGS;
  if (buf && bufLen) buf[0] = '\0';
  { double t0 = thProf.on ? thNow() : 0.0, before = thProf.normalise + thProf.gateDense
        + thProf.gateHead + thProf.licVec + thProf.licScore + thProf.fullDense + thProf.evidence;
    n = thCollect(m, own ? own : text, keep, TH_KEEP_MAX, kbuf, sizeof(kbuf));
    if (thProf.on) {
      double inside = thProf.normalise + thProf.gateDense + thProf.gateHead + thProf.licVec
          + thProf.licScore + thProf.fullDense + thProf.evidence - before;
      thProf.rules += thNow() - t0 - inside;  /* the file-level rules and fallbacks */
    } }
  free(own);
  for (i = 0; i < n && found < maxOut; i++) {
    size_t need = strlen(keep[i].license) + 1;
    if (!buf || used + need > bufLen) break;
    out[found] = keep[i];
    memcpy(buf + used, keep[i].license, need);
    out[found].license = buf + used;
    used += need;
    found++;
  }
  return found;
}
