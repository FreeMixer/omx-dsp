#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# fdelay-sabotage.sh <dir> — the fractional delay line's suite arm, proven able to SEE its own
# defects (fractional delay line design, moved from the engine's
# fractional-delay sabotage harness). Each sabotage is applied
# to a COPY of omx_fdelay.h placed ahead of the real include directory, the suite is built against
# it and must go red on its fdelay arm:
#   exact    — §1's exact-copy branch made to flush its result (arm A feeds words below the flush
#              floor on purpose, so bit-exactness breaks);
#   order    — the kernel computes order 5 as order 3 (arm B's table stops matching the closed
#              form, which is built by a different algorithm and is not sabotaged with it);
#   zerotap  — §1's "a zero coefficient is SKIPPED, never multiplied" removed (arm D poisons the
#              slot the kernel must not touch with a non-finite word, so 0 x inf surfaces).
# The tree is never edited. A sed that matches nothing is a failure, not a skip, and a red that
# printed no FAIL line of the fdelay arm (a compile error, a crash) is not a red arm.
# Usage: CC=… CFLAGS=… fdelay-sabotage.sh <scratch-dir>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"
# the arm list into the scratch dir, never build/: a parallel make builds the suite from build/'s
bash "$PKG/tools/kernels-gen.sh" "$OUT" >/dev/null

sabotage() { # name sed-expression
  local name=$1 expr=$2 d="$OUT/$1" rc=0
  mkdir -p "$d/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$d/omxdsp/"
  sed -i "$expr" "$d/omxdsp/omx_fdelay.h"
  if cmp -s "$PKG/include/omxdsp/omx_fdelay.h" "$d/omxdsp/omx_fdelay.h"; then
    echo "fdelay-sabotage.sh: FAIL — sabotage '$name' changed nothing; the kernel line it targets is gone"
    return 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a word list
  $CC $CFLAGS -ffp-contract=off -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS \
    -I"$OUT" -o "$d/suite" "$PKG/test/omxdsp_suite.c" "$PKG"/src/*.c -lm
  "$d/suite" > "$d/out" 2>&1 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "fdelay-sabotage.sh: FAIL — sabotage '$name' left the suite GREEN; the arm cannot see the defect"
    return 1
  fi
  local n
  n=$(grep -c '^FAIL \[fdelay[] ]' "$d/out" || true)
  if [ "$n" -eq 0 ]; then
    echo "fdelay-sabotage.sh: FAIL — sabotage '$name' went red (rc=$rc) with no FAIL line of the fdelay arm"
    tail -5 "$d/out"
    return 1
  fi
  echo "fdelay-sabotage.sh: sabotage '$name': the fdelay arm went red on $n checks"
}

# §1: "The exact branch above is deliberately NOT flushed." Flush it.
sabotage exact 's|    return l->ring\[idx\];|    return omx_flush(l->ring[idx]);|'
# §1: the order is a parameter; the runtime instantiation is where order 5 is chosen.
sabotage order 's|  else omx_fdelay_lagrange_body(order, f, c);|  else omx_fdelay_lagrange_body(order == 5 ? 3 : order, f, c);|'
# §1: "A ZERO TAP IS SKIPPED, NEVER MULTIPLIED: 0 x inf is NaN." Multiply it.
sabotage zerotap 's|    if (c\[k\] != 0.0f) acc += c\[k\] \* l->ring\[t\];|    acc += c[k] * l->ring[t];|'
# #57: tick_pair's second read is the right line's, not the left's again.
sabotage pair 's|  omx_fdelay_read_pair(a, b, delay, ya, yb);|  omx_fdelay_read_pair(a, a, delay, ya, yb);|'
echo "fdelay-sabotage.sh: the fdelay arm sees all four sabotages"
