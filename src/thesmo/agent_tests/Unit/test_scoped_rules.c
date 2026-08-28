/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/
/**
 * \file
 * \brief Which words in a file speak for the file
 *
 * Two rules read a sentence for what it says about its own subject, and both
 * are pure text work: no model, no database, no window scoring. Parity with
 * the Python implementation compares the two engines against each other, so
 * it cannot catch a fault they share. These are the properties themselves.
 *
 * A Creative Commons RDF document names a license twice and means two things
 * by it. `cc:License rdf:about` describes a license's permits and requires;
 * `cc:license rdf:resource` on the work grants it. An editor can leave the
 * two at different versions, and only the grant answers.
 *
 * A sentence licensing "the rest of" a distribution names the other files, so
 * a header stating it on both sides of its own license still carries that
 * license. The sentence is blanked rather than cut, because a highlight is a
 * byte range and every later offset must still point where it did.
 *
 * Both are reached by including the translation unit, being properties of how
 * text is read rather than of the agent's interface.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "thesmo_model.c"

static int failures;

static void check(int ok, const char *what)
{
  printf("%s - %s\n", ok ? "ok      " : "NOT ok  ", what);
  if (!ok) failures++;
}

static const char *ccVersion(const char *text)
{
  static char got[64];
  got[0] = '\0';
  if (!thCcRdfGrant(text, got, sizeof(got))) return NULL;
  return got;
}

/** The sentence blanked, for comparing against what was expected. */
static char *blanked(const char *text)
{
  char *copy = malloc(strlen(text) + 1);
  if (!copy) { perror("malloc"); exit(2); }
  memcpy(copy, text, strlen(text) + 1);
  thBlankScopedElsewhere(copy);
  return copy;
}

static void ccRules(void)
{
  static const char both[] =
    "<cc:License rdf:about=\"http://creativecommons.org/licenses/by-sa/4.0/\">\n"
    "  <cc:permits rdf:resource=\"http://creativecommons.org/ns#Reproduction\" />\n"
    "</cc:License>\n"
    "<cc:Work rdf:about=\"\">\n"
    "  <cc:license rdf:resource=\"http://creativecommons.org/licenses/by-sa/3.0/\" />\n"
    "</cc:Work>\n";
  static const char aboutOnly[] =
    "<cc:License rdf:about=\"http://creativecommons.org/licenses/by-sa/4.0/\">\n"
    "  <cc:permits rdf:resource=\"http://creativecommons.org/ns#Reproduction\" />\n"
    "</cc:License>\n";
  static const char twoGrants[] =
    "<cc:license rdf:resource=\"http://creativecommons.org/licenses/by-sa/3.0/\" />\n"
    "<cc:license rdf:resource=\"http://creativecommons.org/licenses/by-sa/2.0/\" />\n";
  static const char secure[] =
    "<cc:license rdf:resource=\"https://creativecommons.org/licenses/by/4.0/\" />\n";
  const char *v;

  v = ccVersion(both);
  check(v && !strcmp(v, "3"), "the granted version is read, not the described one");

  check(!ccVersion(aboutOnly), "a license definition alone grants nothing");

  check(!ccVersion(twoGrants), "a file granting two versions decides neither");

  v = ccVersion(secure);
  check(v && !strcmp(v, "4"), "https is read like http");

  check(!ccVersion("<cc:permits rdf:resource=\"http://creativecommons.org/ns#Distribution\" />\n"),
        "a resource that is not a license URL carries no version");

  check(!ccVersion("see http://creativecommons.org/licenses/by-sa/3.0/ for terms\n"),
        "a bare URL is not an RDF grant");

  /* Another host's path can carry a version-shaped segment; only Creative
     Commons' own license URL names a CC version. */
  check(!ccVersion("<cc:license rdf:resource=\"http://example.org/licenses/by-sa/3.0/\" />\n"),
        "a license URL at another host carries no CC version");
  check(!ccVersion("<cc:license rdf:resource=\"http://creativecommons.example/licenses/by/4.0/\" />\n"),
        "a look-alike host is not creativecommons.org");

  /* The reader steps over "creativecommons.org/licenses/" by its length, so
     that comparison is what proves the bytes are there to step over. A URL
     shorter than the prefix would otherwise be walked past its end. */
  check(!ccVersion("<cc:license rdf:resource=\"http://x\" />\n"),
        "a URL shorter than the prefix is refused, not walked past");
  check(!ccVersion("<cc:license rdf:resource=\"http://\" />\n"),
        "an empty host is refused");
}

static void scopedRules(void)
{
  static const char header[] =
    "Notice that the following BSD-style license applies to this one\n"
    "file (demo.h) only.  The rest of Demo is licensed under the\n"
    "terms of the GNU General Public License, version 2, unless\n"
    "otherwise indicated.  See the COPYING file for details.\n";
  static const char entire[] =
    "The entire rest of Demo is licensed under the terms of the GNU\n"
    "General Public License, version 2.\n";
  static const char plainGrant[] =
    "This file is licensed under the GNU General Public License, version 2.\n";
  static const char library[] =
    "The library is distributed under the GPL, version 2.\n";
  char *out;

  out = blanked(header);
  check(strlen(out) == strlen(header), "blanked, not cut: the length is unchanged");
  check(!strstr(out, "General Public"), "the scoped sentence is gone");
  check(strstr(out, "BSD-style") != NULL,
        "only the scoped sentence goes; the rest of the notice stays");
  { size_t i, lines = 0, was = 0;
    for (i = 0; header[i]; i++) if (header[i] == '\n') was++;
    for (i = 0; out[i]; i++) if (out[i] == '\n') lines++;
    check(lines == was, "every newline survives, so the line structure holds"); }
  free(out);

  out = blanked(entire);
  check(!strstr(out, "General Public"), "\"The entire rest of\" is the same statement");
  free(out);

  out = blanked(plainGrant);
  check(!strcmp(out, plainGrant),
        "without the words \"rest of\" this is an ordinary grant");
  free(out);

  out = blanked(library);
  check(!strcmp(out, library), "a distribution verb alone is not a scoped sentence");
  free(out);

  /* The subject is one word. A sentence that merely contains "rest" must not
     take the reader to the next full stop. */
  out = blanked("The code needs rest of a kind we cannot give it. Licensed under MIT.\n");
  check(strstr(out, "Licensed under MIT") != NULL,
        "\"rest of\" without a licensed subject blanks nothing");
  free(out);

  /* "of" is part of the phrase, not decoration: without it the words are
     an ordinary sentence that happens to contain "rest". */
  out = blanked("The rest Demo is licensed under the GPL, version 2.\n");
  check(strstr(out, "General Public") == NULL
        && strstr(out, "licensed under the GPL") != NULL,
        "\"rest\" with no \"of\" is not the scoped phrase");
  free(out);
}

int main(void)
{
  ccRules();
  scopedRules();
  printf("%s\n", failures ? "FAILED"
         : "a sentence is read for what it says about its own subject");
  return failures ? 1 : 0;
}
