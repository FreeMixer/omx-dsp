#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# perturb.sh <dir> — the C reads the generated limits header, not a copy of it
# (docs/design/specs/2026-09-26-dsp-primitives.md §7). A PERTURBED omx_contract_limits.h is
# rendered from the committed one (one declared rate dropped, the comp's ratio floor moved; the
# ledger cap untouched) inside a
# copy of the include directory (the headers include each other by quoted name, so the copy is
# whole), and test/omxdsp_perturb.c is built against that copy: the rate
# that was dropped must now be refused by omx_rate_is_declared and recorded by a rate
# precondition, and a ratio between the real floor and the moved one recorded by the gain
# computer's precondition. The same source built against the REAL header is the control and records
# nothing. Usage: CC=… CFLAGS=… perturb.sh <scratch-dir>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
rm -rf "$OUT" && mkdir -p "$OUT/omxdsp"
cp "$PKG"/include/omxdsp/*.h "$OUT/omxdsp/"
REAL="$PKG/include/omxdsp/omx_contract_limits.h"
# drop 96000 from the declared list and lower the count by one
sed -i 's|, 96000.0f||; s|^#define OMX_DECLARED_RATE_COUNT \([0-9]*\)u$|#define OMX_DECLARED_RATE_COUNT $((\1 - 1))u|' "$OUT/omxdsp/omx_contract_limits.h"
count="$(grep -oE '^#define OMX_DECLARED_RATE_COUNT \$\(\([0-9]+ - 1\)\)u' "$OUT/omxdsp/omx_contract_limits.h" | grep -oE '[0-9]+' | head -1)"
[ -n "$count" ] || { echo "perturb.sh: FAIL — the declared rate count line was not found in the header"; exit 1; }
sed -i "s|^#define OMX_DECLARED_RATE_COUNT .*|#define OMX_DECLARED_RATE_COUNT $((count - 1))u|" "$OUT/omxdsp/omx_contract_limits.h"
# move one bound: the comp's ratio floor from its declared value to 1.5
sed -i 's|^#define OMX_COMP_RATIO_MIN .*$|#define OMX_COMP_RATIO_MIN 1.5f|' "$OUT/omxdsp/omx_contract_limits.h"
grep -q '^#define OMX_COMP_RATIO_MIN 1.5f$' "$OUT/omxdsp/omx_contract_limits.h" || { echo "perturb.sh: FAIL — the comp ratio floor line was not found in the header"; exit 1; }
grep -q '^#define OMX_COMP_RATIO_MIN 1.5f$' "$REAL" && { echo "perturb.sh: FAIL — the real comp ratio floor is already the perturbed value"; exit 1; }
if cmp -s "$REAL" "$OUT/omxdsp/omx_contract_limits.h"; then echo "perturb.sh: FAIL — the perturbation changed nothing"; exit 1; fi
grep -q "96000.0f" "$OUT/omxdsp/omx_contract_limits.h" && { echo "perturb.sh: FAIL — 96000 is still declared in the perturbed header"; exit 1; }

$CC $CFLAGS -I"$OUT" -DOMX_CONTRACTS -DOMXDSP_PERTURBED=1 -o "$OUT/perturbed" "$HERE/omxdsp_perturb.c" -lm
$CC $CFLAGS -I"$PKG/include" -DOMX_CONTRACTS -o "$OUT/control" "$HERE/omxdsp_perturb.c" -lm
"$OUT/perturbed"
"$OUT/control"
echo "perturb.sh: the C followed the perturbed declaration and the control followed the real one"
