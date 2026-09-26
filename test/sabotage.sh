#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# sabotage.sh <scratch dir> kernel|ledger — a COPY of include/omxdsp with ONE line broken, for a
# build that puts the copy ahead of the real headers on the include path; the tree is never
# edited (docs/design/specs/2026-09-26-dsp-primitives.md §8b item 2).
#   kernel  omx_fdelay.h: every Lagrange tap scaled by 0.99, so the kernel sums to 0.99 and
#           `fdelay/kernel post the-kernel-is-unity-at-dc` is the one contract that records
#   ledger  omx_contract.h: `checks` advanced by a load and a store instead of fetch_add, so N
#           threads lose increments and the thread arm goes RED against the copy (§4 F1)
# A sed that matches nothing is a failure, never a silent green.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
WHICH="${2:?kernel|ledger}"

case "$WHICH" in
  kernel)
    file=omx_fdelay.h
    expr='s|    c\[k\] = (float)(num / den);|    c[k] = (float)(num / den) * 0.99f;|' ;;
  ledger)
    file=omx_contract.h
    expr='s|  atomic_fetch_add_explicit(&omx_contract_log.checks, 1u, memory_order_relaxed);|  omx_contract_log.checks = omx_contract_log.checks + 1u;|' ;;
  *) echo "sabotage.sh: unknown sabotage '$WHICH' (kernel|ledger)" >&2; exit 2 ;;
esac

rm -rf "$OUT"
mkdir -p "$OUT/omxdsp"
cp "$PKG"/include/omxdsp/*.h "$OUT/omxdsp/"
sed -i "$expr" "$OUT/omxdsp/$file"
if cmp -s "$OUT/omxdsp/$file" "$PKG/include/omxdsp/$file"; then
  echo "sabotage.sh: FAIL '$WHICH' changed nothing in $file — the expression matched no line" >&2
  exit 1
fi
echo "sabotage.sh: $WHICH — $file broken in $OUT/omxdsp (the tree untouched)"
