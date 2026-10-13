#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reverse_render-perturb.sh — the reverse render's tests see a missing leading block. A SABOTAGED
# copy of fx/omx_reverse_render.h starts the Bartlett grid at 0 instead of -H, so the region's last
# H samples render under a triangle still ramping up; built against it,
# test/fx/reverse_render.test.c must go red in its last-sample arm and
# test/fx/reverse_render_golden.test.c must move at every rate it holds. The tree is never edited;
# a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… reverse_render-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT/omxdsp/fx"
cp "$PKG/include/omxdsp/fx/omx_reverse_render.h" "$OUT/omxdsp/fx/"
sed -i 's|for (int64_t start = -(int64_t)h; start < (int64_t)n; start += (int64_t)h) {|for (int64_t start = 0; start < (int64_t)n; start += (int64_t)h) {|' "$OUT/omxdsp/fx/omx_reverse_render.h"
if cmp -s "$PKG/include/omxdsp/fx/omx_reverse_render.h" "$OUT/omxdsp/fx/omx_reverse_render.h"; then
  echo "reverse_render-perturb: FAIL — the leading-block sabotage matched nothing in fx/omx_reverse_render.h"; exit 1
fi
# shellcheck disable=SC2086 # CFLAGS is a flag list
$CC -I"$OUT" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS \
  -o "$OUT/oracle" "$PKG/test/fx/reverse_render.test.c" "$PKG"/src/*.c -lm
# shellcheck disable=SC2086
$CC -I"$OUT" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -include "$PKG/test/support/log10f_subst.h" \
  -o "$OUT/golden" "$PKG/test/fx/reverse_render_golden.test.c" "$PKG"/src/*.c -lm
if "$OUT/oracle" >"$OUT/oracle.log" 2>&1; then echo "reverse_render-perturb: FAIL — the sabotage left the oracle green"; exit 1; fi
grep -q "^FAIL: a click at the region's LAST sample swells like one in the middle" "$OUT/oracle.log" \
  || { echo "reverse_render-perturb: FAIL — the oracle went red, but not in its last-sample arm"; head -5 "$OUT/oracle.log"; exit 1; }
"$OUT/golden" "$PKG/test/golden/reverse_render.sha256" >"$OUT/golden.log" 2>&1 || true
g="$(grep '^fx/reverse_render_golden:' "$OUT/golden.log")"
[[ "$g" =~ ([0-9]+)\ rates,\ ([0-9]+)\ moved ]] && [ "${BASH_REMATCH[1]}" -gt 0 ] && [ "${BASH_REMATCH[1]}" = "${BASH_REMATCH[2]}" ] \
  || { echo "reverse_render-perturb: FAIL — the sabotage did not move every digest: $g"; exit 1; }
echo "reverse_render-perturb: the leading-block sabotage turned the last-sample arm red and moved the digest at ${BASH_REMATCH[1]} rates"
