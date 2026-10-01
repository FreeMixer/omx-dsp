#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# perturb.sh <dir> — the C reads the generated limits header, not a copy of it
# (docs/design/specs/2026-09-26-dsp-primitives.md §7). A PERTURBED omx_contract_limits.h is
# rendered from the committed one (one declared rate dropped, the comp's ratio floor moved +0.5, the
# opto release's fastMs moved +5 ms; then, one at a time, the LR4 section Q and the
# all-pass/crossover tolerances; the ledger cap untouched) inside a
# copy of the include directory (the headers include each other by quoted name, so the copy is
# whole), and test/omxdsp_perturb.c is built against that copy: the rate
# that was dropped must now be refused by omx_rate_is_declared and recorded by a rate
# precondition, and a ratio between the real floor and the moved one recorded by the gain
# computer's precondition. The same source built against the REAL header is the control and records
# nothing. omxdsp_perturb.c's own cases are collected from test/kernels/*.perturb.c by
# tools/kernels-gen.sh (a kernel lane adds one file there, never edits this script or that one).
# Usage: CC=… CFLAGS=… perturb.sh <scratch-dir>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
bash "$PKG/tools/kernels-gen.sh" "$PKG/build"
rm -rf "$OUT" && mkdir -p "$OUT/omxdsp"
cp "$PKG"/include/omxdsp/*.h "$OUT/omxdsp/"
REAL="$PKG/include/omxdsp/omx_contract_limits.h"
# drop 96000 from the declared list and lower the count by one
sed -i 's|, 96000.0f||; s|^#define OMX_DECLARED_RATE_COUNT \([0-9]*\)u$|#define OMX_DECLARED_RATE_COUNT $((\1 - 1))u|' "$OUT/omxdsp/omx_contract_limits.h"
count="$(grep -oE '^#define OMX_DECLARED_RATE_COUNT \$\(\([0-9]+ - 1\)\)u' "$OUT/omxdsp/omx_contract_limits.h" | grep -oE '[0-9]+' | head -1)"
[ -n "$count" ] || { echo "perturb.sh: FAIL — the declared rate count line was not found in the header"; exit 1; }
sed -i "s|^#define OMX_DECLARED_RATE_COUNT .*|#define OMX_DECLARED_RATE_COUNT $((count - 1))u|" "$OUT/omxdsp/omx_contract_limits.h"
# Every moved value is the REAL one plus a step, read off the header — never a typed target, which
# a header that is itself perturbed (harness/declaration-perturbation-native.sh) could already hold.
define_of() { sed -n "s/^#define $1 \([-0-9.e]*\)f\{0,1\}\$/\1/p" "$REAL"; }
real_floor="$(define_of OMX_COMP_RATIO_MIN)"
real_opto="$(define_of OMX_PROGRAM_RELEASE_PROFILES_OPTO_FAST_MS)"
[ -n "$real_floor" ] || { echo "perturb.sh: FAIL — the comp ratio floor line was not found in the header"; exit 1; }
[ -n "$real_opto" ] || { echo "perturb.sh: FAIL — the opto fastMs line was not found in the header"; exit 1; }
moved_floor="$(awk -v r="$real_floor" 'BEGIN { printf "%.6f", r + 0.5 }')"
between="$(awk -v r="$real_floor" 'BEGIN { printf "%.6f", r + 0.25 }')"
moved_opto="$(awk -v r="$real_opto" 'BEGIN { printf "%.6f", r + 5 }')"
# the moved values reach every build of omxdsp_perturb.c, the control's too (the ratio between the
# two floors is its input; the moved fastMs is what the control must NOT equal)
PDEFS="-DOMXDSP_COMP_RATIO_BETWEEN=${between}f -DOMXDSP_MOVED_COMP_RATIO_MIN=${moved_floor}f -DOMXDSP_MOVED_OPTO_FAST_MS=${moved_opto}f"
# move one bound: the comp's ratio floor from its declared value by +0.5
sed -i "s|^#define OMX_COMP_RATIO_MIN .*\$|#define OMX_COMP_RATIO_MIN ${moved_floor}f|" "$OUT/omxdsp/omx_contract_limits.h"
grep -q "^#define OMX_COMP_RATIO_MIN ${moved_floor}f\$" "$OUT/omxdsp/omx_contract_limits.h" || { echo "perturb.sh: FAIL — the comp ratio floor was not moved"; exit 1; }
# move one profile constant: the opto release's fastMs from its declared value by +5 ms
sed -i "s|^#define OMX_PROGRAM_RELEASE_PROFILES_OPTO_FAST_MS .*\$|#define OMX_PROGRAM_RELEASE_PROFILES_OPTO_FAST_MS ${moved_opto}f|" "$OUT/omxdsp/omx_contract_limits.h"
grep -q "^#define OMX_PROGRAM_RELEASE_PROFILES_OPTO_FAST_MS ${moved_opto}f\$" "$OUT/omxdsp/omx_contract_limits.h" || { echo "perturb.sh: FAIL — the opto fastMs was not moved"; exit 1; }
if cmp -s "$REAL" "$OUT/omxdsp/omx_contract_limits.h"; then echo "perturb.sh: FAIL — the perturbation changed nothing"; exit 1; fi
grep -q "96000.0f" "$OUT/omxdsp/omx_contract_limits.h" && { echo "perturb.sh: FAIL — 96000 is still declared in the perturbed header"; exit 1; }

# shellcheck disable=SC2086
$CC $CFLAGS $PDEFS -I"$OUT" -I"$PKG/build" -DOMX_CONTRACTS -DOMXDSP_PERTURBED=1 -o "$OUT/perturbed" "$HERE/omxdsp_perturb.c" -lm
# shellcheck disable=SC2086
$CC $CFLAGS $PDEFS -I"$PKG/include" -I"$PKG/build" -DOMX_CONTRACTS -o "$OUT/control" "$HERE/omxdsp_perturb.c" -lm
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
  # shellcheck disable=SC2086
  $CC $CFLAGS $PDEFS -I"$dir" -I"$PKG/build" -DOMX_CONTRACTS -D"$3"=1 -o "$dir/perturbed" "$HERE/omxdsp_perturb.c" -lm
  "$dir/perturbed"
}
perturb_one xover-q 's|^#define OMX_XOVER_LR4_SECTION_Q_DOUBLE .*|#define OMX_XOVER_LR4_SECTION_Q_DOUBLE 0.6|' OMXDSP_PERTURBED_XOVER_Q
perturb_one tolerances 's|^#define OMX_ALLPASS_UNITY_TOLERANCE .*|#define OMX_ALLPASS_UNITY_TOLERANCE 1e-12f|; s|^#define OMX_XOVER_PARTITION_TOLERANCE .*|#define OMX_XOVER_PARTITION_TOLERANCE 1e-12f|' OMXDSP_PERTURBED_TOLERANCES
echo "perturb.sh: the C followed every perturbed declaration and the control followed the real one"
