#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# perturb.sh <dir> — the C reads the generated limits header, not a copy of it
# (docs/design/specs/2026-09-26-dsp-primitives.md §7). A PERTURBED omx_contract_limits.h is
# rendered from the committed one (one declared rate dropped; then, one at a time, the LR4 section Q
# and the all-pass/crossover tolerances) inside a
# copy of the include directory (the headers include each other by quoted name, so the copy is
# whole), and test/omxdsp_perturb.c is built against that copy: the rate
# that was dropped must now be refused by omx_rate_is_declared and recorded by a rate
# precondition. The same source built against the REAL header is the control and records
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
if cmp -s "$REAL" "$OUT/omxdsp/omx_contract_limits.h"; then echo "perturb.sh: FAIL — the perturbation changed nothing"; exit 1; fi
grep -q "96000.0f" "$OUT/omxdsp/omx_contract_limits.h" && { echo "perturb.sh: FAIL — 96000 is still declared in the perturbed header"; exit 1; }

$CC $CFLAGS -I"$OUT" -DOMX_CONTRACTS -DOMXDSP_PERTURBED=1 -o "$OUT/perturbed" "$HERE/omxdsp_perturb.c" -lm
$CC $CFLAGS -I"$PKG/include" -DOMX_CONTRACTS -o "$OUT/control" "$HERE/omxdsp_perturb.c" -lm
"$OUT/perturbed"
"$OUT/control"

# The primitives' numbers (core's ALLPASS_LIMITS / XOVER_LIMITS): the LR4 section Q moved off
# 1/sqrt2, and both tolerances moved to 1e-12 — each a copy of the whole include directory with ONE
# define changed, each built against the same source with the define naming what moved.
perturb_one() { # <name> <sed expression> <define>
  local dir="$OUT/$1"
  rm -rf "$dir" && mkdir -p "$dir/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$dir/omxdsp/"
  sed -i "$2" "$dir/omxdsp/omx_contract_limits.h"
  if cmp -s "$REAL" "$dir/omxdsp/omx_contract_limits.h"; then echo "perturb.sh: FAIL — the $1 perturbation changed nothing"; exit 1; fi
  $CC $CFLAGS -I"$dir" -DOMX_CONTRACTS -D"$3"=1 -o "$dir/perturbed" "$HERE/omxdsp_perturb.c" -lm
  "$dir/perturbed"
}
perturb_one xover-q 's|^#define OMX_XOVER_LR4_SECTION_Q .*|#define OMX_XOVER_LR4_SECTION_Q 0.6|' OMXDSP_PERTURBED_XOVER_Q
perturb_one tolerances 's|^#define OMX_ALLPASS_UNITY_TOL .*|#define OMX_ALLPASS_UNITY_TOL 1e-12f|; s|^#define OMX_XOVER_PARTITION_TOL .*|#define OMX_XOVER_PARTITION_TOL 1e-12f|' OMXDSP_PERTURBED_TOLERANCES
echo "perturb.sh: the C followed every perturbed declaration and the control followed the real one"
