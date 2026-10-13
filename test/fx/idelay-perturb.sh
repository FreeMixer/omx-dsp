#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# idelay-perturb.sh — the integer delay line's tests see a re-point that never lands. A SABOTAGED
# copy of omx_idelay.h leaves the applied delay at the old tap after the cross-fade, so every later
# block cross-fades again; built against it, test/fx/idelay.test.c must go red and
# test/fx/idelay_golden.test.c must move at every rate it holds. The tree is never edited; a
# sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… idelay-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT/omxdsp"
cp "$PKG"/include/omxdsp/*.h "$OUT/omxdsp/"
sed -i 's|^    \*cur = tgt;$|    *cur = d0;|' "$OUT/omxdsp/omx_idelay.h"
if cmp -s "$PKG/include/omxdsp/omx_idelay.h" "$OUT/omxdsp/omx_idelay.h"; then
  echo "idelay-perturb: FAIL — the re-point sabotage matched nothing in omx_idelay.h"; exit 1
fi
# shellcheck disable=SC2086 # CFLAGS is a flag list
$CC -I"$OUT" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
  -o "$OUT/oracle" "$PKG/test/fx/idelay.test.c" "$PKG"/src/*.c -lm
# shellcheck disable=SC2086
$CC -I"$OUT" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -include "$PKG/test/support/log10f_subst.h" \
  -o "$OUT/golden" "$PKG/test/fx/idelay_golden.test.c" "$PKG"/src/*.c -lm
if "$OUT/oracle" >"$OUT/oracle.log" 2>&1; then echo "idelay-perturb: FAIL — the sabotage left the idelay oracle green"; exit 1; fi
red="$(grep -c '^FAIL' "$OUT/oracle.log" || true)"
"$OUT/golden" "$PKG/test/golden/idelay.sha256" >"$OUT/golden.log" 2>&1 || true
g="$(grep '^fx/idelay_golden:' "$OUT/golden.log")"
[[ "$g" =~ ([0-9]+)\ rates,\ ([0-9]+)\ moved ]] && [ "${BASH_REMATCH[1]}" -gt 0 ] && [ "${BASH_REMATCH[1]}" = "${BASH_REMATCH[2]}" ] \
  || { echo "idelay-perturb: FAIL — the sabotage did not move every digest: $g"; exit 1; }
echo "idelay-perturb: the re-point sabotage turned $red idelay checks red and moved the digest at ${BASH_REMATCH[1]} rates"
