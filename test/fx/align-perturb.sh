#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# align-perturb.sh — the alignment stage's tests see a broken slew. A SABOTAGED copy of
# omx_align.h moves the read point up by twice OMX_ALIGN_SLEW per sample; built against it,
# test/fx/align_fdelay.test.c must go red (a move no longer lands after |delta| / OMX_ALIGN_SLEW
# samples) and test/fx/align_golden.test.c must move at every rate it holds. The tree is never
# edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… align-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT/omxdsp"
cp "$PKG"/include/omxdsp/*.h "$OUT/omxdsp/"
sed -i 's|cur = (gap > OMX_ALIGN_SLEW) ? cur + OMX_ALIGN_SLEW|cur = (gap > OMX_ALIGN_SLEW) ? cur + 2.0f * OMX_ALIGN_SLEW|' "$OUT/omxdsp/omx_align.h"
if cmp -s "$PKG/include/omxdsp/omx_align.h" "$OUT/omxdsp/omx_align.h"; then
  echo "align-perturb: FAIL — the slew sabotage matched nothing in omx_align.h"; exit 1
fi
# shellcheck disable=SC2086 # CFLAGS is a flag list
$CC -I"$OUT" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
  -o "$OUT/oracle" "$PKG/test/fx/align_fdelay.test.c" "$PKG"/src/*.c -lm
# shellcheck disable=SC2086
$CC -I"$OUT" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -include "$PKG/test/support/log10f_subst.h" \
  -o "$OUT/golden" "$PKG/test/fx/align_golden.test.c" "$PKG"/src/*.c -lm
if "$OUT/oracle" >"$OUT/oracle.log"; then echo "align-perturb: FAIL — the sabotage left align_fdelay green"; exit 1; fi
red="$(grep -c '^FAIL' "$OUT/oracle.log" || true)"
"$OUT/golden" "$PKG/test/golden/align.sha256" >"$OUT/golden.log" 2>&1 || true
g="$(grep '^fx/align_golden:' "$OUT/golden.log")"
[[ "$g" =~ ([0-9]+)\ rates,\ ([0-9]+)\ moved ]] && [ "${BASH_REMATCH[1]}" -gt 0 ] && [ "${BASH_REMATCH[1]}" = "${BASH_REMATCH[2]}" ] \
  || { echo "align-perturb: FAIL — the sabotage did not move every digest: $g"; exit 1; }
echo "align-perturb: the slew sabotage turned $red align_fdelay checks red and moved the digest at ${BASH_REMATCH[1]} rates"
