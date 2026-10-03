#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Debian bookworm's GCC 12.2 on arm64 dies with an internal compiler error in
# vect_transform_reduction on a `double` running maximum or minimum kept with fmax/fmin over values
# widened from float (BUILDING.md, "GCC 12.2 on arm64"). Only an arm64 bookworm build sees the
# crash, so this scan refuses the shape on any host: `x = fmax(x, ... (double) ...)`. Write it as
# `if (!(e <= x)) x = e;` instead.
set -euo pipefail
cd "$(dirname "$0")/.."
found=0
while IFS= read -r hit; do
  echo "reduction-check: $hit" >&2
  found=1
done < <(git ls-files -z -- '*.c' '*.h' | xargs -0 grep -nHE '\b([A-Za-z_][A-Za-z0-9_]*) = fm(ax|in)\(\1, [^;]*\(double\)' || true)
if [[ $found == 0 ]]; then
  echo "reduction-check: no fmax/fmin running reduction over widened floats"
else
  echo "reduction-check: write these as comparisons, see BUILDING.md \"GCC 12.2 on arm64\"" >&2
fi
exit "$found"
