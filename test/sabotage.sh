#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# sabotage.sh <dir> — a SABOTAGED COPY of the library headers for the negative suite's POST arm
# (docs/design/specs/2026-09-26-dsp-primitives.md §8b item 2): the fractional read's Lagrange
# kernel is broken so it no longer sums to one, and the copy is placed ahead of the real include
# directory when omxdsp_negative.c is compiled. The tree is never edited. A sed that matches
# nothing is a failure, not a skip.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
rm -rf "$OUT" && mkdir -p "$OUT/omxdsp"
cp "$PKG"/include/omxdsp/*.h "$OUT/omxdsp/"
sed -i 's|    c\[k\] = (float)(num / den);|    c[k] = (float)(num / den) + (k == 0 ? 0.25f : 0.0f);|' "$OUT/omxdsp/omx_fdelay.h"
if cmp -s "$PKG/include/omxdsp/omx_fdelay.h" "$OUT/omxdsp/omx_fdelay.h"; then
  echo "sabotage.sh: FAIL — the kernel line the sabotage targets is gone; the negative POST arm would test nothing"
  exit 1
fi
echo "sabotage.sh: $OUT/omxdsp/omx_fdelay.h carries a kernel that does not sum to one"
