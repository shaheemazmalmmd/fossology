#!/bin/bash
# SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD
# SPDX-License-Identifier: GPL-2.0-only
#
# Functional test for the thesmo CLI. Requires an exported model directory,
# passed as $1 or found at ../../../../build/src/thesmo/model.

THESMO=${THESMO:-../../../../build/src/thesmo/agent/thesmo}
MODEL=${1:-../../../../build/src/thesmo/model}
FAIL=0

check() {
  local desc="$1" file="$2" want="$3" got
  got=$("$THESMO" -m "$MODEL" "$file" 2>/dev/null | sed 's/.*contains license(s) //')
  if [ "$got" = "$want" ]; then
    echo "ok       - $desc"
  else
    echo "NOT ok   - $desc (want '$want', got '$got')"
    FAIL=1
  fi
}

check_has() {
  local desc="$1" file="$2" want="$3" got
  got=$("$THESMO" -m "$MODEL" "$file" 2>/dev/null | sed 's/.*contains license(s) //')
  case ",$got," in
    *",$want,"*) echo "ok       - $desc" ;;
    *) echo "NOT ok   - $desc (want '$want' among '$got')"; FAIL=1 ;;
  esac
}

check_exception() {
  local desc="$1" file="$2" want="$3" got
  got=$("$THESMO" -m "$MODEL" -J "$file" 2>/dev/null | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print(''); raise SystemExit
print(','.join(sorted({r['exception'] for r in d if r.get('exception')})))
")
  case ",$got," in
    *",$want,"*) echo "ok       - $desc" ;;
    *) echo "NOT ok   - $desc (want exception '$want', got '$got')"; FAIL=1 ;;
  esac
}

check_runs() {
  local desc="$1" file="$2"
  if "$THESMO" -m "$MODEL" "$file" >/dev/null 2>&1; then
    echo "ok       - $desc"
  else
    echo "NOT ok   - $desc (exit $?)"
    FAIL=1
  fi
}

check_exception_none() {
  local desc="$1" file="$2" got
  got=$("$THESMO" -m "$MODEL" -J "$file" 2>/dev/null | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print(''); raise SystemExit
print(','.join(sorted({r['exception'] for r in d if r.get('exception')})))
")
  if [ -z "$got" ]; then
    echo "ok       - $desc"
  else
    echo "NOT ok   - $desc (claimed exception '$got')"
    FAIL=1
  fi
}

check_method() {
  local desc="$1" file="$2" want="$3" got
  got=$("$THESMO" -m "$MODEL" -J "$file" 2>/dev/null | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print(''); raise SystemExit
print(','.join(sorted({r['method'] for r in d if r.get('license')})))
")
  if [ "$got" = "$want" ]; then
    echo "ok       - $desc"
  else
    echo "NOT ok   - $desc (want method '$want', got '$got')"
    FAIL=1
  fi
}

check_not() {
  local desc="$1" file="$2" unwanted="$3" got
  got=$("$THESMO" -m "$MODEL" "$file" 2>/dev/null | sed 's/.*contains license(s) //')
  case ",$got," in
    *",$unwanted,"*) echo "NOT ok   - $desc (must not claim '$unwanted', got '$got')"; FAIL=1 ;;
    *) echo "ok       - $desc" ;;
  esac
}

# a grant with a clear name must be identified
printf 'Licensed under the Apache License, Version 2.0 (the "License");\nyou may not use this file except in compliance with the License.\n' > /tmp/th_t1
check "apache-2.0 grant" /tmp/th_t1 "Apache-2.0"

# a bare licence-name enumeration is a mention, not a grant: must abstain
printf 'Supported values are apache-2.0, mit, gpl-2.0 and bsd-new.\n' > /tmp/th_t2
check "enumeration abstains" /tmp/th_t2 "No_license_found"

# ordinary prose must abstain
printf 'This function returns the number of bytes written to the buffer.\n' > /tmp/th_t3
check "plain comment abstains" /tmp/th_t3 "No_license_found"

# A file small enough to be one window is read whole, however short. The window
# floor rejects a fragment of a longer file, not a one-line document.
printf 'license: Vim\n' > /tmp/th_t4
check "one-line reference" /tmp/th_t4 "Vim"

# An accented name: the accent is a separator between the words of the name,
# not a longer word running through it.
printf 'name : Licence Libre du Qu\xc3\xa9bec \xe2\x80\x93 R\xc3\xa9ciprocit\xc3\xa9 forte version 1.1\n' > /tmp/th_t5
check "accented licence name" /tmp/th_t5 "LiLiQ-Rplus-1.1"

# The stated version may sit on the far side of a line break from the word
# that makes it a version rather than a number.
printf 'This program is free software; you can redistribute it and/or modify it\nunder the terms of the GNU General Public License as published by the Free\nSoftware Foundation; either version\n1, or (at your option) any later version.\n' > /tmp/th_t6
check "version across a line break" /tmp/th_t6 "GPL-1.0-or-later"

# "Licence_CeCILL-B_V1-fr.html" is CeCILL-B's own URL, so it names that
# licence -- but the V is inside an identifier and states no version, so
# nothing here says CECILL-1.0.
# A licence field is a grant written as a field, not a sentence, so no grant
# pattern matches and the window is rejected. Abstaining is right; naming a
# family the file never mentioned is not -- `"license": "MIT", "type":
# "module"` normalises to `mit type`, which the MIT-style pattern read as
# "MIT-type".
printf '{\n  "name": "widget",\n  "version": "1.2.3",\n  "license": "MIT",\n  "type": "module"\n}\n' > /tmp/th_t20
check_not "a manifest is not an MIT-style guess" /tmp/th_t20 "MIT-style"

printf '{\n  "name": "widget",\n  "license": "UNLICENSED"\n}\n' > /tmp/th_t22
check_not "an unlicensed manifest states none" /tmp/th_t22 "MIT"

# BSD-3-Clause under someone else's name, with bulleted conditions: its
# wording is shared with 56 BSD-derived families, so the phrases that survived
# a hard sharing filter were all canonical-spelling artefacts.
printf 'Copyright (c) 2013, The GoGo Authors. All rights reserved.\n\nRedistribution and use in source and binary forms, with or without\nmodification, are permitted provided that the following conditions are met:\n\n    * Redistributions of source code must retain the above copyright\nnotice, this list of conditions and the following disclaimer.\n    * Redistributions in binary form must reproduce the above\ncopyright notice, this list of conditions and the following disclaimer\nin the documentation and/or other materials provided with the\ndistribution.\n    * Neither the name of Google Inc. nor the names of its\ncontributors may be used to endorse or promote products derived from\nthis software without specific prior written permission.\n\nTHIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.\n' > /tmp/th_t23
check_has "a licence under any holder" /tmp/th_t23 "BSD-3-Clause"

printf 'See http://www.cecill.info/licences/Licence_CeCILL-B_V1-fr.html for the licence.\n' > /tmp/th_t7
check "a licence URL names its licence" /tmp/th_t7 "CECILL-B"
# The V is inside an identifier and states no version. A URL standing alone on
# a line, with no words around it, scores below the threshold and is reported
# as nothing -- which is a miss, not a wrong answer.
printf 'http://www.cecill.info/licences/Licence_CeCILL-B_V1-fr.html\n' > /tmp/th_t7b
check_not "a filename states no version" /tmp/th_t7b "CECILL-1.0"

# A name written in curly quotes is still a name.
printf 'The text of this license is released under the Creative Commons\nAttribution-ShareAlike 4.0 International License, with the caveat that any\nmodifications of this license may not use the name \xe2\x80\x9cCryptographic Autonomy\nLicense\xe2\x80\x9d or any name confusingly similar thereto.\n' > /tmp/th_t8
check_has "a quoted name is a name" /tmp/th_t8 "CAL-1.0"

# A NonCommercial notice must not come back as the permissive sibling. The word
# "attribution" alone is evidence for CC-BY only where a licence word sits
# beside it, and "...about the licens" is a window cut mid-word, not one.
printf 'is licensed\nunder a Creative Commons license. This license permits\nnon-commercial use of this work,\nso long as attribution is given.\nFor more information about the license,\nclick the icon above, or visit\n<http://creativecommons.org/licenses/by-nc/1.0/>.\n' > /tmp/th_t9
check_not "no permissive over-grant on a NonCommercial notice" /tmp/th_t9 "CC-BY-3.0"

# A bare family word names the family, not one of its members. "BSD" was listed
# as a name of BSD-2-Clause and of nothing else, so every unqualified BSD
# reference came back with a clause count the text never states.
printf 'This file is distributed under the BSD licence.\n' > /tmp/th_t10
check "a bare BSD reference is the family" /tmp/th_t10 "BSD"

# The Creative Commons modifiers are written in any order. Only the canonical
# by-nc-nd order was generated, so a reversed name matched nothing for the full
# licence and the nearest partial match won -- dropping the NonCommercial term,
# which is the one direction a licence scanner must not err in.
printf 'Creative Commons Attribution-NoDerivs-NonCommercial 1.0\n' > /tmp/th_t11
check "reversed CC modifiers keep NonCommercial" /tmp/th_t11 "CC-BY-NC-ND-1.0"

# An exception is reported on its own axis. The window carrying the exception
# clause usually names no licence, so it used to be discarded and the exception
# with it -- the exception only survived if it happened to land on the window
# that won the licence.
printf 'This library is free software; you can redistribute it and/or modify it\n    under the terms of the GNU General Public License as published by the Free\n    Software Foundation; either version 2, or (at your option) any later\n    version.\n    This library is distributed in the hope that it will be useful, but WITHOUT\n    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or\n    FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for\n    more details.\n    You should have received a copy of the GNU General Public License along\n    with this library; see the file COPYING. If not, write to the Free Software\n    Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.\n    GCC Linking Exception\n    In addition to the permissions in the GNU General Public License, the Free\n    Software Foundation gives you unlimited permission to link the compiled\n    version of this file into combinations with other programs, and to\n    distribute those combinations without any restriction coming from the use\n    of this file. (The General Public License restrictions do apply in other\n    respects; for example, they cover modification of the file, and\n    distribution when not linked into a combine executable.\n' > /tmp/th_t12
check_exception "an exception beside its licence" /tmp/th_t12 "GCC-exception-2.0"

# An exception clause on its own is still an identification, not nothing.
printf 'license: 389-exception\n' > /tmp/th_t13
check "an exception on its own" /tmp/th_t13 "389-exception"

# A copyright line names the holder, not the licence. "Copyright (c) Intel
# Corporation" sits within a few words of "licensed", which was enough to make
# the bare name evidence for the Intel licence on every EDK2 header there is.
printf '/** @file\n  Provides services to create and parse HOBs.\n\nCopyright (c) 2006 - 2012, Intel Corporation. All rights reserved.<BR>\nThis program and the accompanying materials\nare licensed and made available under the terms and conditions of the BSD License\nwhich accompanies this distribution.  The full text of the license may be found at\nhttp://opensource.org/licenses/bsd-license.php\n**/\n' > /tmp/th_t26
check_not "a copyright holder is not a licence name" /tmp/th_t26 "Intel"

# GPL-3.0 names the GNU Affero General Public License in its own section 13, to
# permit linking with it. A name written inside another licence's text is that
# licence's wording, not a second grant. AGPL-3.0 is GPL-3.0 plus a network
# clause, so GPL-3.0 has no sentence AGPL lacks and could never win on its own
# distinctive text; being reported is the evidence.
GPL3=../../../nomos/agent_tests/testdata/NomosTestfiles/GPL/GPL-3.0.txt
if [ -f "$GPL3" ]; then
  check_not "a licence named inside another is not a grant" "$GPL3" "AGPL-3.0-only"
  check_has "the licence the text is" "$GPL3" "GPL-3.0-only"
fi

# A dedication to the public domain has no SPDX identifier, so the class
# vocabulary has never had one and these files came back as nothing. nomos
# reports a bare mention as Public-domain-ref and its denial separately; the
# patterns and the names are its own.
printf 'This file is provided as-is.\nIt has been placed in the public domain by the author.\nThe copyright holder claims no rights in it.\n' > /tmp/th_t28
check_has "a public domain dedication" /tmp/th_t28 "Public-domain-ref"
printf 'No part of this document is in the public domain; all rights are reserved under copyright law.\n' > /tmp/th_t29
check_not "a denial is not a dedication" /tmp/th_t29 "Public-domain-ref"
check_has "a denial is reported as one" /tmp/th_t29 "NOT-public-domain"

# An SPDX-License-Identifier tag is a declaration, not prose, and is read
# rather than weighed: the same line scores 82 alone and is rejected outright
# with two #include lines under it, which lost 77% of a firmware tree.
printf '/* SPDX-License-Identifier: GPL-2.0-only */\n\n#include <acpi/acpigen.h>\n#include <assert.h>\n#include <device/device.h>\n\nvoid f(void) { return; }\n' > /tmp/th_t30
check "a tag beside code" /tmp/th_t30 "GPL-2.0-only"

# SPDX deprecated the bare id and the "+" suffix; both still appear in the wild.
printf '// SPDX-License-Identifier: GPL-2.0+\nint x;\n' > /tmp/th_t31
check "a deprecated tag spelling" /tmp/th_t31 "GPL-2.0-or-later"

# WITH attaches an exception to the licence before it.
printf '/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */\nint x;\n' > /tmp/th_t32
check_exception "a tag naming an exception" /tmp/th_t32 "Linux-syscall-note"

# A sentence about tags is not a tag, and neither is a declaration of nothing.
printf 'Every file must carry an SPDX-License-Identifier: tag naming its licence.\n' > /tmp/th_t33
check "a sentence about tags" /tmp/th_t33 "No_license_found"
printf '// SPDX-License-Identifier: NONE\nint x;\n' > /tmp/th_t34
check "a tag declaring nothing" /tmp/th_t34 "No_license_found"

# Most packaging metadata states a licence as a field with a family for its
# value, and the family is all it says. Matched in the file as written:
# normalising `"license": "BSD-2-Clause"` leaves `license bsd 2 clause`, whose
# first two words are the pattern, so a lock file would report the family for
# every dependency it lists.
printf ':copyright: Copyright 2007-2017 by the Sphinx team.\n:license: BSD, see LICENSE for details.\n' > /tmp/th_t35
check_has "a licence field naming a family" /tmp/th_t35 "BSD"
printf 'Metadata-Version: 1.2\nName: numpy\nVersion: 1.18.1\nLicense: BSD\nDescription: NumPy is the fundamental package\n' > /tmp/th_t36
check "a metadata licence field" /tmp/th_t36 "BSD"
printf '{"node_modules/abbrev": {"version": "3.0.0", "dev": true, "license": "BSD-2-Clause"}}\n' > /tmp/th_t37
check_not "a lock file is not a family claim" /tmp/th_t37 "BSD"
printf 'touch MODULE_LICENSE_GPL\n' > /tmp/th_t38
check "an identifier is not a licence field" /tmp/th_t38 "No_license_found"

# "under the terms of the NumPy License" grants a licence and names a project,
# not a licence. Saying which one would be a guess; saying nothing loses the
# grant.
printf 'Copyright 1999,2000 Pearu Peterson all rights reserved,\nPermission to use, modify, and distribute this software is given under the\nterms of the NumPy License.\n\nNO WARRANTY IS EXPRESSED OR IMPLIED.  USE AT YOUR OWN RISK.\n' > /tmp/th_t39
check "a grant naming a project" /tmp/th_t39 "UnclassifiedLicense"

# SPDX names a variant by extending the identifier of the licence it varies,
# and the two share almost every word, so the margin between them is arbitrary
# and per window the variant does not reach the shortlist at all. What settles
# it is the wording only one of them has -- LAPACK's licence is the
# BSD-3-Clause-Open-MPI reference text and was reported as BSD-3-Clause.
cat > /tmp/th_t40 <<'VARIANT'
Copyright (c) 1992-2013 The University of Tennessee and The University
                        of Tennessee Research Foundation.  All rights
                        reserved.
Copyright (c) 2000-2013 The University of California Berkeley. All
                        rights reserved.
Copyright (c) 2006-2013 The University of Colorado Denver.  All rights
                        reserved.

$COPYRIGHT$

Additional copyrights may follow

$HEADER$

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

- Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer listed
  in this license in the documentation and/or other materials
  provided with the distribution.

- Neither the name of the copyright holders nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

The copyright holders provide no reassurances that the source code
provided does not infringe any patent, copyright, or any other
intellectual property rights of third parties.  The copyright holders
disclaim any liability to any recipient for claims brought against
recipient by any third party for infringement of that parties
intellectual property rights.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
VARIANT
check "a variant of the licence it extends" /tmp/th_t40 "BSD-3-Clause-Open-MPI"

# And the same licence without that paragraph is the licence it extends. A
# fragment of the text the two share shows one or two of a variant's phrases by
# chance, which is why a share of them is required and not a count.
printf 'Redistribution and use in source and binary forms, with or without\nmodification, are permitted provided that the following conditions are met:\n\n1. Redistributions of source code must retain the above copyright notice,\n   this list of conditions and the following disclaimer.\n\n2. Redistributions in binary form must reproduce the above copyright notice,\n   this list of conditions and the following disclaimer in the documentation\n   and/or other materials provided with the distribution.\n\n3. Neither the name of the copyright holder nor the names of its contributors\n   may be used to endorse or promote products derived from this software\n   without specific prior written permission.\n' > /tmp/th_t41
check "the licence a variant extends" /tmp/th_t41 "BSD-3-Clause"

# A directory inside a longer path names a place, not a licence. `DOC` and
# `www.khronos.org/registry` are both licence names and both turned up as path
# segments in numpy: a manifest of per-directory licences and a link to an
# OpenGL extension spec.
printf 'Files: doc/sphinxext/numpydoc/*\nLicense: 2-clause BSD\n  For details, see doc/sphinxext/LICENSE.txt\n' > /tmp/th_t42
check_not "a path segment is not a licence name" /tmp/th_t42 "DOC"
printf 'Half-float support is described in the ARB extension:\n\n__ https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_half_float_pixel.txt\n' > /tmp/th_t43
check "a URL prefix is not a licence reference" /tmp/th_t43 "No_license_found"

# One slash is an alternation between two names, not a path.
printf 'This project is released under the MIT/X11 license.\n' > /tmp/th_t44
check_has "a slash between two names" /tmp/th_t44 "MIT"

# A name inside a copyright notice is the holder's. BIND writes the holder's
# abbreviation and declares a different licence on the next line, which put ISC
# on 3,550 files that are not under it.
printf '/*\n * Copyright (C) Internet Systems Consortium, Inc. ("ISC")\n *\n * SPDX-License-Identifier: MPL-2.0\n *\n * This Source Code Form is subject to the terms of the Mozilla Public\n * License, v. 2.0. If a copy of the MPL was not distributed with this\n * file, you can obtain one at https://mozilla.org/MPL/2.0/.\n */\n' > /tmp/th_t45
check "a holder abbreviation is not a licence" /tmp/th_t45 "MPL-2.0"

# A notice and not merely the word: the marker has to carry a year or a holder,
# or a licence named as the value of a copyright tag would be lost.
printf '@copyright GNU General Public License version. 2 (GPLv2)\n' > /tmp/th_t46
check_has "a licence named by a copyright tag" /tmp/th_t46 "GPL-2.0-only"
printf 'we waive copyright and related rights in the work worldwide through the CC0 1.0 Universal public domain dedication.\n' > /tmp/th_t47
check_has "a licence named beside a waiver" /tmp/th_t47 "CC0-1.0"

cat > /tmp/th_t48 <<'EOF'
Font Maki definition
Dual-licensed under the CeCILL-B Licence (http://www.cecill.info/) and the Beerware license: as long as you retain this notice you can do whatever you want with this stuff. If we meet some day, and you think this stuff is worth it, you can buy me a beer in return.
This module also bundles a table of glyph names, a small parser for the icon
metadata, and a compatibility shim for older browsers. None of that is covered
by the notice above; it was written from scratch for this project and carries
no separate terms of its own beyond what the header already states.
Historical note: an early prototype of this file was ported from a program that
was distributed under the GNU General Public License version 2, but no code from
it survives here.
EOF
check_has "a choice of licence names the first"  /tmp/th_t48 "Beerware"
check_has "a choice of licence names the second" /tmp/th_t48 "CECILL-B"
check_not "a named choice does not also report the marker" /tmp/th_t48 "Dual-license"
check_not "a licence written past the coordinating clause" /tmp/th_t48 "GPL-2.0-only"

printf 'Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0\n' > /tmp/th_t49
check "a single grant names one licence" /tmp/th_t49 "Apache-2.0"

cat > /tmp/th_t50 <<'EOF'
# This originates from X11R5 (mit/util/scripts/install.sh), which was
# later released in X11R6 (xc/config/util/install.sh) with the
# following copyright and license.
#
# Copyright (C) 1994 X Consortium
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# Except as contained in this notice, the name of the X Consortium shall not
# be used in advertising or otherwise to promote the sale, use or other deal-
# ings in this Software without prior written authorization from the X Consor-
# tium.
EOF
check_has "a quoted licence outranks its name being a bare word" /tmp/th_t50 "X11"

printf 'MySQL Connector for Python\nThis package is part of the MySQL Connector distribution.\n' > /tmp/th_t51
check_not "a technology word alone is not a licence" /tmp/th_t51 "Python-2.0"

printf 'Name:           demo\nVersion:        1.0\nLicense:        GPLv3+\nURL:            https://example.invalid/demo\n' > /tmp/th_t52
check "a package metadata field in the v spelling" /tmp/th_t52 "GPL-3.0-or-later"
printf 'Name:           demo\nVersion:        1.0\nLicense:        GPLv3\nURL:            https://example.invalid/demo\n' > /tmp/th_t53
check "the same field without the plus" /tmp/th_t53 "GPL-3.0-only"

cat > /tmp/th_t54 <<'EOF'
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#
# As a special exception to the GNU General Public License, if you
# distribute this file as part of a program that contains a
# configuration script generated by Autoconf, you may include it under
# the same distribution terms that you use for the rest of that
# program.  This Exception is an additional permission under section 7
# of the GNU General Public License, version 3 ("GPLv3").
EOF
check_has "a grant outranks the version an exception names" /tmp/th_t54 "GPL-3.0-or-later"
check_not "the exception's bare version is not the grant" /tmp/th_t54 "GPL-3.0-only"

# A notice longer than one window that never writes the license's name: every
# window sees a fragment, and only the whole file carries the wording that
# tells this license from the rest of the BSD family.
cat > /tmp/th_t55 <<'EOF'
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
# - Redistributions of source code must retain the above copyright
#   notice, this list of conditions and the following disclaimer.
#
# - Redistributions in binary form must reproduce the above copyright
#   notice, this list of conditions and the following disclaimer listed
#   in this license in the documentation and/or other materials
#   provided with the distribution.
#
# - Neither the name of the copyright holders nor the names of its
#   contributors may be used to endorse or promote products derived from
#   this software without specific prior written permission.
#
# The copyright holders provide no reassurances that the source code
# provided does not infringe any patent, copyright, or any other
# intellectual property rights of third parties.  The copyright holders
# disclaim any liability to any recipient for claims brought against
# recipient by any third party for infringement of that parties
# intellectual property rights.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
EOF
check "a license carried in full but never named" /tmp/th_t55 "BSD-3-Clause-Open-MPI"

# The exception clause names the license it modifies, which is not the
# strongest finding here.
cat > /tmp/th_t56 <<'EOF'
# Copyright (C) 2011 Free Software Foundation, Inc.
#
# This file is dual licensed under the terms of the MIT license, and
# under the terms of the GNU General Public License.
#
# GNU Libtool is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# As a special exception to the GNU General Public License,
# if you distribute this file as part of a program or library that
# is built using GNU Libtool, you may include this file under the
# same distribution terms that you use for the rest of that program.
EOF
check_exception "an exception found beside two licenses" /tmp/th_t56 "Libtool-exception"
check_not "the exception does not land on the other license" /tmp/th_t56 "MIT WITH Libtool-exception"

# A pattern is run only over text carrying a run of plain characters it
# cannot match without. The run is read from the pattern, so what it skips
# over must still match: a case-insensitive field written in capitals, a
# pointer written as a URL whose host the run does not contain, and a choice
# whose two names sit on either side of a group.
printf 'LICENSE = BSD\n' > /tmp/th_t57
check "a field in capitals" /tmp/th_t57 "BSD"
printf 'See https://example.org/license for the terms.\n' > /tmp/th_t58
check "a pointer written as a URL" /tmp/th_t58 "See-URL"
printf 'This file may be used under the terms of either the MIT license or, at your option, the Apache-2.0 license.\n' > /tmp/th_t60
check_has "a choice names its first licence" /tmp/th_t60 "Apache-2.0"
check_has "a choice names its second licence" /tmp/th_t60 "MIT"

# A licence text is found by its phrases, each asked of the file only where a
# table of the file's own runs allows it: the whole text is still found.
printf 'Permission is hereby granted, free of charge, to any person obtaining a copy\nof this software and associated documentation files (the "Software"), to deal\nin the Software without restriction, including without limitation the rights\nto use, copy, modify, merge, publish, distribute, sublicense, and/or sell\ncopies of the Software, and to permit persons to whom the Software is\nfurnished to do so, subject to the following conditions:\n\nThe above copyright notice and this permission notice shall be included in\nall copies or substantial portions of the Software.\n\nTHE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\nIMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\nFITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\nAUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\nLIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\nOUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN\nTHE SOFTWARE.\n' > /tmp/th_t59
check_has "a licence text found by its phrases" /tmp/th_t59 "MIT"

# A BSD-3-Clause notice numbering a fourth condition the family has no marker
# for is not BSD-3-Clause and no other member: a licence is here that cannot
# be named. The same notice numbering three is BSD-3-Clause exactly.
BSD3='Redistribution and use in source and binary forms, with or without\nmodification, are permitted provided that the following conditions are met:\n1. Redistributions of source code must retain the above copyright notice,\n   this list of conditions and the following disclaimer.\n2. Redistributions in binary form must reproduce the above copyright notice,\n   this list of conditions and the following disclaimer in the documentation\n   and/or other materials provided with the distribution.\n3. The name of Example may not be used to endorse or promote products derived\n   from this software without specific prior written permission.\n'
TAIL='THIS SOFTWARE IS PROVIDED BY EXAMPLE "AS IS" AND ANY EXPRESS OR IMPLIED\nWARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL EXAMPLE BE LIABLE FOR ANY DAMAGES.\n'
printf "${BSD3}4. This software may only be redistributed and used in connection with an\n   Example microcontroller product.\n${TAIL}" > /tmp/th_t61
check "a numbered condition beyond the licence" /tmp/th_t61 "UnclassifiedLicense"
printf "${BSD3}${TAIL}" > /tmp/th_t62
check "three numbered conditions" /tmp/th_t62 "BSD-3-Clause"

# More shapes of a dedication: the authors below the line, an adverb inside
# the dedication, and the author named first.
printf '/*\n * Public Domain, Authors:\n * - Example Author\n * - Other Author\n */\n' > /tmp/th_t63
check_has "a dedication with the authors below" /tmp/th_t63 "Public-domain"
printf '/*\n * This code is explicitely put in the public domain\n */\n' > /tmp/th_t64
check_has "a dedication with an adverb" /tmp/th_t64 "Public-domain"
printf 'Originally written by Example Author <ex@example.org> and placed into\nthe Public Domain, do with it what you will.\n' > /tmp/th_t65
check_has "an author-first dedication" /tmp/th_t65 "Public-domain"

# A LibreJS declaration names the licence by the text file its magnet points
# at, and a tag carrying attributes is markup, not words beside the notice.
printf '<script>\n/* @license magnet:?xt=urn:btih:cf05388f2679ee054f2beb29a391d25f4e673ac3&amp;dn=gpl-2.0.txt GPL-v2 */\nvar x = 1;\n/* @license-end */\n</script>\n' > /tmp/th_t66
check "a LibreJS magnet declaration" /tmp/th_t66 "GPL-2.0-only"
printf '<span class="comment">Licensed under the Apache License, Version 2.0 (the "License");</span>\n<span class="comment">you may not use this file except in compliance with the License.</span>\n' > /tmp/th_t67
check "a notice inside attributed markup" /tmp/th_t67 "Apache-2.0"
printf '// Copyright (c) 2012 - present, Example Author\n// All rights reserved.\n//\n// For the license information refer to format.h.\n' > /tmp/th_t68
check "a pointer that refers to a file" /tmp/th_t68 "See-file"

# The mirror: a licence whose text numbers its conditions, found in a file
# carrying its grant sentence and disclaimer with none of them.
C=/home/fossology/development/community/thesmo/tests/corpus
if [ -f "$C/grant_without_its_conditions.txt" ]; then
  check "a grant without the conditions the licence numbers" "$C/grant_without_its_conditions.txt" "UnclassifiedLicense"
  check "a grant with its conditions" "$C/grant_with_its_conditions.txt" "TekHVC"
fi

# A GNU notice naming no version grants any version: GPL-1.0-or-later by the
# licence's own section 9. A mention of the GPL is not a grant.
printf '/*\n * Copyright (C) 2008 Example Author\n *\n * This file may be redistributed under the terms of the GNU Public License.\n */\n' > /tmp/th_t69
check "a GNU notice without a version" /tmp/th_t69 "GPL-1.0-or-later"
printf 'Contributions: the code must be released to us under the GNU GPL.\n' > /tmp/th_t70
check_not "a policy naming the GPL is a mention" /tmp/th_t70 "GPL-1.0-or-later"
printf 'It may be distributed under the GNU General Public License, version 2, or\nany higher version. See section COPYING of the GNU Public license\nfor conditions under which this file may be redistributed.\n' > /tmp/th_t71
check_not "a stated version is not any version" /tmp/th_t71 "GPL-1.0-or-later"

# A holder is read by its wording, not its name: the modify variant's own
# sentence lifts the base licence to it under any holder.
if [ -f "$C/holder_of_a_licence_written_here.txt" ]; then
  check "a holder read by its wording" "$C/holder_of_a_licence_written_here.txt" "HPND-export-US-modify"
fi

# A licence document is the licence as published: its "How to Apply"
# appendix is text, not a grant the file makes.
RF=/home/fossology/development/community/thesmo/datasets/real-findings
if [ -f "$RF/AGPL-3.0-only.txt" ]; then
  check_has "a licence document is -only" "$RF/AGPL-3.0-only.txt" "AGPL-3.0-only"
  check_not "its appendix grants nothing" "$RF/AGPL-3.0-only.txt" "AGPL-3.0-or-later"
fi

# Pointers say where to look: a man page's copying line, and a pointer that
# stays where the name beside it stated no version.
printf '.\\" Copying restrictions apply.  See COPYRIGHT/LICENSE.\n.TH EXAMPLE 5\n' > /tmp/th_t72
check "a man page pointer" /tmp/th_t72 "See-file.LICENSE"
printf '/* This code is released under the libpng license.\n * For conditions of distribution and use, see the disclaimer\n * and license in png.h\n */\n' > /tmp/th_t73
check "a pointer beside an unversioned name" /tmp/th_t73 "See-file"

# A finding carries the rule that changed it: the stage, then the rule.
if [ -f "$C/holder_of_a_licence_written_here.txt" ]; then
  check_method "the trail of a lifted holder" "$RF/BSD-2-Clause-7.txt" "ml+container"
  check_method "the trail of a demoted licence" "$C/bsd_with_a_vendor_condition.txt" "ml+clause-count"
  check_method "no trail where no rule changed the head" /tmp/th_t1 "ml"
fi

# zlib's grant with no restrictions list at all is not zlib; with its three
# restrictions, which close nowhere but run into the code, it is.
if [ -f "$C/grant_without_its_restrictions.txt" ]; then
  check "a grant with no restrictions list" "$C/grant_without_its_restrictions.txt" "UnclassifiedLicense"
fi
printf '/*\n  Copyright (C) 1997-2025 Example Author\n  This software is provided as-is, without any express or implied\n  warranty.  In no event will the authors be held liable for any damages\n  arising from the use of this software.\n  Permission is granted to anyone to use this software for any purpose,\n  including commercial applications, and to alter it and redistribute it\n  freely, subject to the following restrictions:\n  1. The origin of this software must not be misrepresented; you must not\n     claim that you wrote the original software.\n  2. Altered source versions must be plainly marked as such, and must not be\n     misrepresented as being the original software.\n  3. This notice may not be removed or altered from any source distribution.\n*/\n#include <stdlib.h>\nint main(void) { return 0; }\n' > /tmp/th_t74
check "zlib with its restrictions" /tmp/th_t74 "Zlib"

# The profile is written on request and nowhere else.
if THESMO_PROFILE=1 "$THESMO" -m "$MODEL" /tmp/th_t59 2>&1 >/dev/null | grep -q "^thesmo profile:"; then
  echo "ok       - the profile on request"
else
  echo "NOT ok   - the profile on request"; FAIL=1
fi
if "$THESMO" -m "$MODEL" /tmp/th_t59 2>&1 >/dev/null | grep -q "^thesmo profile:"; then
  echo "NOT ok   - no profile unasked"; FAIL=1
else
  echo "ok       - no profile unasked"
fi

# A licence granted with an exception is one SPDX expression. The exception
# head's scores are all negative, so the old floor of zero vetoed exceptions
# the evidence gate had already admitted, and the reference dropped every one
# of them at file level besides.
printf '# This program is free software; you can redistribute it and/or modify\n# it under the terms of the GNU General Public License as published by\n# the Free Software Foundation; either version 2, or (at your option)\n# any later version.\n#\n# As a special exception to the GNU General Public License, if you\n# distribute this file as part of a program that contains a\n# configuration script generated by Autoconf, you may include it under\n# the same distribution terms that you use for the rest of that program.\n' > /tmp/th_t48
check_exception "an exception on the licence it modifies" /tmp/th_t48 "Autoconf-exception-generic"
check_has "the licence the exception modifies" /tmp/th_t48 "GPL-2.0-or-later"

# Most of an exception's text is the licence it modifies -- SHL-2.1 quotes
# Apache-2.0 -- so it is claimed only on its own name or on wording few
# families share, never on the licence's.
printf 'Licensed under the Apache License, Version 2.0 (the "License");\nyou may not use this file except in compliance with the License.\nYou may obtain a copy of the License at\n\n    http://www.apache.org/licenses/LICENSE-2.0\n' > /tmp/th_t49
check_exception_none "an ordinary notice carries no exception" /tmp/th_t49

# Normalisation rewrites "v 2" to "version 2", the one rewrite that writes more
# than it reads, so a file dense with version strings outgrew the output buffer
# and the agent aborted on the corrupted heap.
python3 -c "print('v.1 v.2 v.3 v.4 v.5 ver.1 ver.2 ver.3 ' * 200)" > /tmp/th_t24
check_runs "a file dense with version strings" /tmp/th_t24
printf 'Released under GPL version 2 in v.1, v.2, v.3, v.4, v.5 and v.6.\nThis program is free software; you can redistribute it and/or modify it under\nthe terms of the GNU General Public License, version 2, as published by the\nFree Software Foundation.\n' > /tmp/th_t25
check "a grant among version strings" /tmp/th_t25 "GPL-2.0-only"

# JSON output must be parseable and carry provenance
if "$THESMO" -m "$MODEL" -J /tmp/th_t1 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
assert d[0]['engine']=='thesmo', d
assert d[0]['model_version']
assert 0 <= d[0]['match_pct'] <= 100
" 2>/dev/null; then
  echo "ok       - JSON output shape"
else
  echo "NOT ok   - JSON output shape"
  FAIL=1
fi

rm -f /tmp/th_t1 \
      /tmp/th_t10 \
      /tmp/th_t11 \
      /tmp/th_t12 \
      /tmp/th_t13 \
      /tmp/th_t2 \
      /tmp/th_t20 \
      /tmp/th_t22 \
      /tmp/th_t23 \
      /tmp/th_t24 \
      /tmp/th_t25 \
      /tmp/th_t26 \
      /tmp/th_t28 \
      /tmp/th_t29 \
      /tmp/th_t3 \
      /tmp/th_t30 \
      /tmp/th_t31 \
      /tmp/th_t32 \
      /tmp/th_t33 \
      /tmp/th_t34 \
      /tmp/th_t35 \
      /tmp/th_t36 \
      /tmp/th_t37 \
      /tmp/th_t38 \
      /tmp/th_t39 \
      /tmp/th_t4 \
      /tmp/th_t40 \
      /tmp/th_t41 \
      /tmp/th_t42 \
      /tmp/th_t43 \
      /tmp/th_t44 \
      /tmp/th_t45 \
      /tmp/th_t46 \
      /tmp/th_t47 \
      /tmp/th_t48 \
      /tmp/th_t49 \
      /tmp/th_t50 \
      /tmp/th_t51 \
      /tmp/th_t52 \
      /tmp/th_t53 \
      /tmp/th_t54 \
      /tmp/th_t55 \
      /tmp/th_t56 \
      /tmp/th_t57 \
      /tmp/th_t58 \
      /tmp/th_t59 \
      /tmp/th_t60 \
      /tmp/th_t61 \
      /tmp/th_t62 \
      /tmp/th_t63 \
      /tmp/th_t64 \
      /tmp/th_t65 \
      /tmp/th_t66 \
      /tmp/th_t67 \
      /tmp/th_t68 \
      /tmp/th_t69 \
      /tmp/th_t70 \
      /tmp/th_t71 \
      /tmp/th_t72 \
      /tmp/th_t73 \
      /tmp/th_t74 \
      /tmp/th_t5 \
      /tmp/th_t6 \
      /tmp/th_t7 \
      /tmp/th_t7b \
      /tmp/th_t8 \
      /tmp/th_t9

exit $FAIL
