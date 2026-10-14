#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# loudness-perturb.sh — the loudness kernels' tests see each part of the measurement break. Five
# SABOTAGED copies of omx_loudness.h, one break each: the K-weighting shelf's b0 6 % high, no
# relative gate, the range's high end read at the low percentile, the true peak read 1 dB low and
# a 300 ms momentary window. Built against each, test/fx/loudness.test.c must go red (at 48 kHz,
# its quick arm) and test/fx/loudness_golden.test.c must move at every rate it holds. The tree is
# never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… loudness-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"

sabotage() { # <name> <sed expression>
  local dir="$OUT/$1"
  mkdir -p "$dir/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$dir/omxdsp/"
  sed -i "$2" "$dir/omxdsp/omx_loudness.h"
  if cmp -s "$PKG/include/omxdsp/omx_loudness.h" "$dir/omxdsp/omx_loudness.h"; then
    echo "loudness-perturb: FAIL — the $1 sabotage matched nothing in omx_loudness.h"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$dir" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$dir/oracle" "$PKG/test/fx/loudness.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  # shellcheck disable=SC2086
  $CC -I"$dir" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -include "$PKG/test/support/log10f_subst.h" \
    -o "$dir/golden" "$PKG/test/fx/loudness_golden.test.c" "$PKG"/src/*.c -lm
  if "$dir/oracle" 48000 >"$dir/oracle.log" 2>&1; then echo "loudness-perturb: FAIL — the $1 sabotage left the loudness oracle green"; exit 1; fi
  local red
  red="$(grep -c '^FAIL' "$dir/oracle.log" || true)"
  "$dir/golden" "$PKG/test/golden/loudness.sha256" >"$dir/golden.log" 2>&1 || true
  local g
  g="$(grep '^fx/loudness_golden:' "$dir/golden.log")"
  [[ "$g" =~ ([0-9]+)\ rates,\ ([0-9]+)\ moved ]] && [ "${BASH_REMATCH[1]}" -gt 0 ] && [ "${BASH_REMATCH[1]}" = "${BASH_REMATCH[2]}" ] \
    || { echo "loudness-perturb: FAIL — the $1 sabotage did not move every digest: $g"; exit 1; }
  echo "loudness-perturb: the $1 sabotage turned $red loudness checks red and moved the digest at ${BASH_REMATCH[1]} rates"
}

sabotage shelf 's|^  c\[0\]\[0\] = (vh + vb \* k1 / q1 + k1 \* k1) / d1;$|  c[0][0] = 1.06 * (vh + vb * k1 / q1 + k1 * k1) / d1;|'
sabotage relative-gate 's|^  const double gate = e / n \* pow(10.0, rel_lu / 10.0);$|  const double gate = 0.0 * e / n * pow(10.0, rel_lu / 10.0);|'
sabotage range-percentile 's|^  const uint64_t rhi = (uint64_t)((double)(n - 1u) \* OMX_LOUDNESS_RANGE_HIGH + 0.5);$|  const uint64_t rhi = (uint64_t)((double)(n - 1u) * OMX_LOUDNESS_RANGE_LOW + 0.5);|'
sabotage true-peak 's|^      const float p = omx_truepeak_block(&s->tp\[c\], in\[c\] + done, k, pk);$|      const float p = 0.89f * omx_truepeak_block(\&s->tp[c], in[c] + done, k, pk);|'
sabotage momentary-window 's|^  return omx_loudness_window_ms(s, OMX_LOUDNESS_MOMENTARY);$|  return omx_loudness_window_ms(s, 30u);|'
echo "loudness-perturb: every sabotage turned the oracle red and moved every digest"
