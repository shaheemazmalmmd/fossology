/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief Thesmo model: load, score, decide
 */
#ifndef THESMO_MODEL_H
#define THESMO_MODEL_H

#include <stddef.h>

#define TH_DENSE 14 /**< grant/mention dense features */

/** one TF-IDF block: a vocabulary plus its idf weights */
/* A file larger than this cannot be read whole: the license in it is too
   small a share of the text to clear the verifier. */
#define TH_WHOLE_MAX 65536

#define TH_DECLARED_MAX 16 /**< identifiers a notice may declare */
#define TH_CLAUSE_MAX  16 /**< conditions all license families are read by */
#define TH_CONDITIONS_SPAN 2000 /**< a conditions list is a few hundred words */
#define TH_CONDITIONS_LEAD 60 /**< a numbered list begins this close to its opener */
#define TH_HOLDER_NAMED 3 /**< phrases a holder named inside its wording takes with it */
#define TH_HOLDER_NEAR 80 /**< how far a copyright statement reaches past the word */
#define TH_VERSION_REACH 60 /**< a version stated this far after a licence name belongs to it */
#define TH_CONDITIONS_SHARE 0.5 /**< a list shorter than this share of the licence's, numbering nothing, is not its list */
#define TH_FAMILY_MAX   4 /**< containment families read by conditions */

typedef struct
{
  int isChar; /**< char_wb analyzer, else word */
  int lo, hi; /**< ngram range */
  int n; /**< vocabulary size */
  size_t offset; /**< column offset inside the concatenated vector */
  struct thHash *vocab;
  float *idf;
  /* Counting scratch, kept across windows: a window touches a few hundred
     of the vocabulary's terms, so only those are visited and reset, rather
     than a fresh vocabulary-sized buffer per window. */
  float *acc;
  int *touched, nTouched, touchedCap;
  int *sortTmp, sortCap;
  /* The n-grams of a word are the same wherever it occurs, so each distinct
     word is cut and looked up once and its term counts replayed after. */
  struct thWordCache *words;
} thBlock;

/** a linear model over the concatenated blocks */
typedef struct
{
  int rows, cols;
  float *bias;
  float *dense; /**< rows*cols, NULL when sparse */
  int **idx; /**< sparse rows */
  float **val;
  int *nnz;
  /* The same coefficients by column: a window touches a few hundred of the
     columns, and each names the few rows that use it, so scoring visits only
     those instead of walking every row's list. Each row still receives its
     terms in ascending column order, so the sums are the ones the row walk
     gives. */
  int *colPtr;   /**< cols + 1 offsets into colRow / colVal */
  int *colRow;
  float *colVal;
  double *acc;   /**< rows of scratch, so a window allocates nothing */
} thLinear;

typedef struct
{
  thBlock *gateBlk; int nGateBlk;
  thBlock *licBlk;  int nLicBlk;
  thBlock *sfxBlk;  int nSfxBlk;
  thLinear gate, lic, sfx;
  char **classes; int nClasses;
  unsigned char *isException; /**< class is an SPDX exception, not a license */
  float scMean[TH_DENSE], scScale[TH_DENSE];
  struct thHash *gaz; /**< license-name gazetteer */
  struct thHash *gazPrefix; /**< every opening run of words of every name */
  /* ScanCode-style required phrases: text a license has and its near
     duplicates do not, used to settle which of two siblings a file holds */
  char ***reqPhr; /**< per class, NULL-terminated phrase list */
  char ***reqSib; /**< per class, NULL-terminated sibling list */
  char ***conIn;  /**< per class, NULL-terminated list of licences holding it */
  char ***conNear; /**< the same relation, looser, for the rules that read wording */
  char ****conDist; /**< per class, per near holder, what the holder says beyond it */
  char ***conHolder; /**< per class, the words naming its holder, or NULL */
  char ***unver;  /**< set where the identifier names no version */
  /* A license is claimed only where the text names or quotes it; without that
     the top of the ranking on a lock file is noise. */
  char ***nameVar; /**< per class, NULL-terminated name variants */
  float **nameW; /**< how surprising each variant is in ordinary code */
  /* Where a pattern is matched: 0 normalized, 1 both, 2 as written.
     Normalization suits prose and breaks punctuation. */
  unsigned char *refWhere;
  char ***bodyPhr; /**< per class, NULL-terminated phrases of its text */
  float **bodyW; /**< what each phrase narrows down, -log(share) */
  int **bodyFam; /**< license families that share each phrase */
  /* Statements that point at a license without naming one. nomos's own
     patterns; reported only where nothing could be identified. */
  char **refName; void *refRe; int nRef;
  char **refLit; /**< per pattern, a run the text must carry, or NULL */
  struct thHash *byName; /**< class name -> index */
  /* A license and the variants SPDX derives from it. They share almost all
     their text, so only unshared wording decides. -1 terminated. */
  int **variant;
  int *variantBase; /**< per class, the nearest license it extends, or -1 */
  /* An identifier SPDX chooses by a declaration beside the notice rather than
     by the license's own words: OFL-1.1 and OFL-1.1-RFN are the same text. */
  char *declBase[TH_DECLARED_MAX];
  char *declTo[TH_DECLARED_MAX];
  char *declPhr[TH_DECLARED_MAX];
  int nLicensor;                    /**< the same words with a licensor named inside */
  char *licBase[TH_DECLARED_MAX];
  char *licTo[TH_DECLARED_MAX];
  char *licPhr[TH_DECLARED_MAX];
  int nDecl;
  char ***varPhr; /**< what tells each variant from what it extends */
  /* How each license has actually been written. The pooled phrases are one
     rendering's, and a file carries one rendering rather than their union. */
  char ****rendPhr; /**< per class, NULL-terminated renderings of phrases */
  /* Which conditions a license of a containment family writes. Such a family
     sits inside itself -- BSD-2-Clause is most of BSD-3-Clause, and
     HPND-sell-regexpr is the opening sentence of every longer HPND -- so no
     wording is exclusive to the shorter ones; the conditions present are what
     separate them, and an absent clause has no n-gram to match. */
  char *clauseKey[TH_CLAUSE_MAX]; /**< condition letters */
  char *clausePhr[TH_CLAUSE_MAX]; /**< the wording each is recognised by */
  int nClause;
  char *famName[TH_FAMILY_MAX];   /**< family names */
  char *famKeys[TH_FAMILY_MAX];   /**< the condition letters each family owns */
  int *clauseCount; /**< per class, how many conditions its own text numbers; 0 unread */
  int *clauseSpan;  /**< per class, how long its numbered conditions list is; 0 where it numbers under two */
  int nFamily;
  char **clauseSig; /**< per class, "family:" and the conditions its text writes */
#define TH_VARIANT_SHARE 0.5f /**< of a variant's own phrases, to displace */
  int gazMax; /**< longest gazetteer entry, in words */
  void *grantRe; char **grantLit; int nGrant;
  void *mentionRe; char **mentionLit; int nMention;
  float thGate, thMargin, thSuffix, thVersionMargin, thException;
  float thNameMin; /**< evidence a name must carry to count */
  float thAloneMargin; /**< margin when only one license is credible */
  float thNamedMargin; /**< margin when the text names the pick most fully */
  float thExcGrant; /**< grant density below which an exception's license
                               is named rather than granted */

  /* The verifier: how likely a finding is to be right, given its evidence.
     A margin alone cannot tell a confident finding from a lucky one. */
#define TH_EVID 20 /**< evidence features, in the reference's order */
#define TH_DISTINCT 4 /**< families at or below which a phrase is its own */
#define TH_ISO 101 /**< knots of the isotonic calibration */
  int haveVerifier;
  float verifW[TH_EVID];
  float verifB;
  float isoX[TH_ISO], isoY[TH_ISO];
  int nIso;
  float thBodyMin; /**< summed phrase weight that stands in for a name */
  float thSubsumed; /**< share of a claim's text another claim explains */
  int thCandidates; /**< how far down the ranking to look for evidence */
  int minScore; /**< suppress findings scored below this */
  float *calLo, *calHi; /**< margin bands ... */
  int *calScore; /**< ... and their measured precision, as a percent */
  int nCal;
  char version[32];
} thModel;

typedef struct
{
  const char *license; /**< NULL when the file is not a license grant */
  const char *status; /**< HIGH | UNKNOWN */
  float gate; /**< grant-gate score */
  float margin; /**< top1 - top2 */
  int score; /**< calibrated confidence, 0-100 */
  const char *exception; /**< SPDX exception found alongside, may be NULL */
  float excScore;
  const char *alt; /**< runner-up, may be NULL */
  float altMargin;
  const char *method; /**< which heads altered the answer */
  /* Where in the file the finding was read, so a reviewer can see why. Zero
     when there is nothing to point at, as for a referential finding. */
  long start;
  long len;
  float grant; /**< grant phrases in the window that claimed it */
} thResult;

thModel *thLoad(const char *dir, char *err, size_t errLen);
void thFree(thModel *m);
void thProfileReport(FILE *to); /**< where the time went, when THESMO_PROFILE is set */
int thClassify(thModel *m, const char *text, thResult *out);

/**
 * \brief Classify a whole file by scoring overlapping line windows.
 *
 * Vectorizing a whole document dilutes a notice inside it, since the L2 norm
 * runs over every n-gram. `out->license` points into `buf`, owned by the
 * caller.
 */
int thClassifyFile(thModel *m, const char *text, thResult *out,
    char *buf, size_t bufLen);

#define TH_CAND_MAX     8 /**< ceiling on thCandidates */
#define TH_VER_HITS    32 /**< versions of one family a text may state */
#define TH_MAX_FINDINGS 8 /**< a file rarely grants more licenses than this */
#define TH_TAG_MAX      4 /**< more SPDX tags than this is a catalog */
#define TH_KEEP_MAX    64 /**< collected before the limit is applied */
#define TH_FINE_LINES   3 /**< narrow pass: separates co-licensed lines */
#define TH_FINE_STEP    1
#define TH_FINE_MAX  6000 /**< cap the narrow pass on very large files */
#define TH_EXPRESSION_MAX   3 /**< licenses one coordinating window may name */
#define TH_EXPRESSION_DEPTH 10 /**< how far down the ranking a named one may sit */
#define TH_COORD_REACH   240 /**< chars past a coordinating phrase it governs */
#define TH_COORD_BACK     80 /**< and before it */
#define TH_COORD_CLAUSES   8 /**< coordinating phrases read per window */
#define TH_SENTENCE_CAP  240 /**< how far a sentence's edge is looked for */
#define TH_NOTICE_MAX     32 /**< "or later" grants remembered by position */
#define TH_HOLDERS_MAX    64 /**< holders of one licence considered by the mirror rule */
/** A license's own text runs to many times the evidence that merely says the
    text is present, so the two regimes are far apart: the GPL-3.0 document
    scores about fifty times thBodyMin, a file that grants it about one. */
#define TH_IS_THE_TEXT 4.0f
/** A license's distinguishing phrases exist to tell it from its siblings, so
    a file carrying nearly all of them carries that license's text. The
    highest share any file that grants nothing reaches is 0.55. */
#define TH_VERBATIM      0.72f
#define TH_VERBATIM_MIN  8   /**< below this a high share is cheap */
/** What a rendering has to be to settle it. Asking several ways a license has
    been written gives several chances at a high share, so the answer has to be
    that the file carries one of them and not merely resembles it. */
#define TH_RENDER_MIN    0.99

/**
 * \brief Every distinct license the file grants, best window per license.
 *
 * A multi-licensed header grants several at once. Names are copied into `buf`,
 * owned by the caller.
 *
 * \return how many findings were written to `out`
 */
int thClassifyAll(thModel *m, const char *text, thResult *out, int maxOut,
    char *buf, size_t bufLen);

#endif
