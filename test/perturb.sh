#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# perturb.sh <scratch dir> — the C side of R-094 (docs/design/specs/2026-09-26-dsp-primitives.md
# §7, §8b item 3): a perturbed omx_contract_limits.h is rendered from the committed one (the LAST
# declared rate dropped, OMX_FDELAY_READ_L1_NORM moved to 1.0f), test/omxdsp_perturb.c is built
# against it ahead of the real header and must be GREEN — the contracts followed the header.
# Then the same perturbed header over LITERAL copies of the two words (omx_contract.h's rate
# loop as the committed rate list spelled out, omx_fdelay.h's bound as the committed number)
# must leave the arm RED — a copy does not follow. Both, or the run fails.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
REAL="$PKG/include/omxdsp/omx_contract_limits.h"
cd "$PKG"

rates="$(sed -n 's/^static const float OMX_DECLARED_RATES\[OMX_DECLARED_RATE_COUNT\] = { \(.*\) };$/\1/p' "$REAL")"
count="$(sed -n 's/^#define OMX_DECLARED_RATE_COUNT \([0-9]*\)u$/\1/p' "$REAL")"
l1="$(sed -n 's/^#define OMX_FDELAY_READ_L1_NORM \(.*\)$/\1/p' "$REAL")"
dropped="${rates##*, }"
[ -n "$rates" ] && [ -n "$count" ] && [ -n "$l1" ] && [ -n "$dropped" ] || {
  echo "perturb.sh: FAIL the committed header does not carry the rate list, its count and the L1 bound as expected" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/perturbed/omxdsp" "$OUT/literal/omxdsp"
cp include/omxdsp/*.h "$OUT/perturbed/omxdsp/"
P="$OUT/perturbed/omxdsp/omx_contract_limits.h"
sed -i -e "s/^#define OMX_DECLARED_RATE_COUNT ${count}u\$/#define OMX_DECLARED_RATE_COUNT $((count - 1))u/" \
       -e "s/, ${dropped} };\$/ };/" \
       -e "s/^#define OMX_FDELAY_READ_L1_NORM .*\$/#define OMX_FDELAY_READ_L1_NORM 1.0f/" "$P"
grep -q "^#define OMX_DECLARED_RATE_COUNT $((count - 1))u\$" "$P" && ! grep -q "$dropped" "$P" \
  && grep -q '^#define OMX_FDELAY_READ_L1_NORM 1.0f$' "$P" || {
  echo "perturb.sh: FAIL the perturbation did not land in the rendered header" >&2; exit 1; }

cp "$OUT/perturbed/omxdsp/"*.h "$OUT/literal/omxdsp/"
literal_rates="$(printf '%s' "$rates" | sed 's/, / || sr == /g; s/^/(sr == /; s/$/)/')"
sed -i "s#OMX_DECLARED_RATES\\[i\\] == sr#${literal_rates}#" "$OUT/literal/omxdsp/omx_contract.h"
sed -i "s|OMX_FDELAY_READ_L1_NORM|${l1}|g" "$OUT/literal/omxdsp/omx_fdelay.h"
cmp -s "$OUT/literal/omxdsp/omx_contract.h" "$OUT/perturbed/omxdsp/omx_contract.h" && { echo "perturb.sh: FAIL the rate literal was not planted" >&2; exit 1; }
cmp -s "$OUT/literal/omxdsp/omx_fdelay.h" "$OUT/perturbed/omxdsp/omx_fdelay.h" && { echo "perturb.sh: FAIL the bound literal was not planted" >&2; exit 1; }

build() { # dir out
  $CC $CFLAGS -I"$1" -Iinclude -DOMX_CONTRACTS -DOMX_PERTURB_DROPPED_RATE="$dropped" \
    -o "$2" test/omxdsp_perturb.c src/omx_oversampler.c -lm
}
build "$OUT/perturbed" "$OUT/perturb_follows"
"$OUT/perturb_follows" || { echo "perturb.sh: FAIL the contracts did not follow the perturbed header" >&2; exit 1; }
build "$OUT/literal" "$OUT/perturb_literal"
if "$OUT/perturb_literal" > "$OUT/literal.log" 2>&1; then
  echo "perturb.sh: FAIL a literal copy of the rate list and the bound stayed GREEN — the arm cannot see a copy" >&2; exit 1
fi
echo "perturb.sh: the contracts followed the perturbed header (rate $dropped dropped of $count, L1 bound $l1 -> 1.0f); the literal copy went red"
