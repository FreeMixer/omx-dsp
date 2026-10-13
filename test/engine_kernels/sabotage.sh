#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# sabotage.sh <dir> — the golden digests of the kernels moved from the engine see a break in each:
# one line of omx_src.h, omx_gen.h and analysis/omx_rta.h is changed in a COPY of include/, the golden
# program is built against the copy, and it must report that kernel's digests moved at every RME
# rate and no other kernel's. The tree is never edited; a sed that matches nothing is a failure.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"
RATES="$(awk '/^src /' "$PKG/test/golden/engine_kernels.sha256" | wc -l)"
fail=0
# kernel | header | sed expression: each one moves that kernel's arithmetic
while IFS='|' read -r kernel header expr; do
  rm -rf "$OUT" && mkdir -p "$OUT"
  cp -r "$PKG/include" "$OUT/include"
  sed -i "$expr" "$OUT/include/omxdsp/$header"
  if cmp -s "$PKG/include/omxdsp/$header" "$OUT/include/omxdsp/$header"; then
    echo "SABOTAGE $kernel: the edit changed nothing in $header"; fail=1; continue
  fi
  # A sabotaged copy need not be warning-clean: GCC 16 reads the src break's shortened loop as a
  # possible overflow of the record (-Wstringop-overflow), so the copy builds without -Werror.
  # shellcheck disable=SC2086
  $CC $CFLAGS -Wno-error -I"$OUT/include" -isystem "$CONTRACT_INC" -o "$OUT/golden" "$PKG/test/engine_kernels/golden.test.c" -lm
  if "$OUT/golden" "$PKG/test/golden/engine_kernels.sha256" >"$OUT/log" 2>&1; then
    echo "SABOTAGE $kernel: STAYED GREEN"; fail=1; continue
  fi
  moved="$(grep -c "^FAIL: $kernel " "$OUT/log" || true)"
  other="$(grep '^FAIL: ' "$OUT/log" | grep -vc "^FAIL: $kernel " || true)"
  if [ "$moved" != "$RATES" ] || [ "$other" != 0 ]; then
    echo "SABOTAGE $kernel: red, but $moved of $RATES $kernel digests moved and $other of the others"; fail=1; continue
  fi
  echo "SABOTAGE $kernel: red at all $RATES rates, no other kernel moved"
done <<'EOF'
src|omx_src.h|s/  for (uint32_t k = 0; k < s->taps; k++) acc += c\[k\] \* h\[k\];/  for (uint32_t k = 0; k + 1u < s->taps; k++) acc += c[k] * h[k];/
gen|omx_gen.h|s/  b\[6\] = w \* 0.115926f;/  b[6] = w * 0.115927f;/
rta|analysis/omx_rta.h|s/    if (hi > last) hi = last;/    if (hi > last) hi = last - 1;/
EOF
rm -rf "$OUT"
[ "$fail" = 0 ] || exit 1
echo "engine-kernels sabotage: every kernel's break turned its own digests red"
