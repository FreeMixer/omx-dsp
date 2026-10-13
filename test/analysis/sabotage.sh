#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# sabotage.sh <dir> — one sabotage per analysis kernel: a SABOTAGED COPY of the kernel is placed
# ahead of the real one, test/analysis/golden.test.c is built against it, and its digests must
# move for that kernel's engine. A golden that still passes would hold nothing. The tree is never
# edited, and a sed that matches nothing is a failure, not a skip.
#
#   fbs     src/omx_fbs_detect.c   a candidate's level reads 0.5 dB high
#   rta     omx_rta.h              the spectrum's Hann gain correction is halved
#   render  omx_hrp_render.h       a window counts as hot at any positive deviation, not past the step
#   live    omx_hrp_lv2.h          a band is placed 1 % above the partial it was measured on
#
# CC, CFLAGS and CONTRACT_INC from the environment (make exports CONTRACT_INC).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:?CONTRACT_INC is not set}"
cd "$PKG"
# Every rate's digest of the sabotaged engine must move: the analysis rates, counted from the golden file.
nrates="$(awk '$1 == "fbs"' test/golden/analysis.sha256 | wc -l)"
rm -rf "$OUT" && mkdir -p "$OUT"

# sabotage <engine> <file> <sed expression>
sabotage() {
  local engine="$1" file="$2" expr="$3" dir="$OUT/$1"
  mkdir -p "$dir/omxdsp/analysis"
  local copy="$dir/omxdsp/analysis/$(basename "$file")"
  [ "${file#src/}" != "$file" ] && copy="$dir/$(basename "$file")"
  cp "$file" "$copy"
  sed -i "$expr" "$copy"
  if cmp -s "$file" "$copy"; then
    echo "sabotage.sh: FAIL — $engine: the line the sabotage targets in $file is gone; it would test nothing"
    exit 1
  fi
  local extra=()
  [ "${file#src/}" != "$file" ] && extra=("$copy")
  $CC $CFLAGS -I"$dir" -Iinclude -isystem "$CONTRACT_INC" -Isrc -Itest/analysis -Itest/fx -Wl,--wrap=log10f \
    -o "$dir/golden" test/analysis/golden.test.c "${extra[@]}" build/libomxdsp.a -lm
  if "$dir/golden" test/golden/analysis.sha256 > "$dir/log" 2>&1; then
    echo "sabotage.sh: FAIL — $engine: a sabotaged $file still gives the golden digests"
    exit 1
  fi
  local moved
  moved="$(grep -c "^FAIL: $engine at .* moved" "$dir/log" || true)"
  if [ "$moved" != "$nrates" ]; then
    echo "sabotage.sh: FAIL — $engine: a sabotaged $file moved the $engine digests at $moved of $nrates rates"
    cat "$dir/log"
    exit 1
  fi
  echo "sabotage.sh: $engine: a sabotaged $(basename "$file") moves its digests at all $moved rates"
}

sabotage fbs src/omx_fbs_detect.c \
  's|^  c.level_db = to_db(m\[bin\]);$|  c.level_db = to_db(m[bin]) + 0.5;|'
sabotage rta include/omxdsp/analysis/omx_rta.h \
  's|^  const float scale = 4.0f / (float)n;$|  const float scale = 2.0f / (float)n;|'
sabotage render include/omxdsp/analysis/omx_hrp_render.h \
  's|^      if (deviation > r->cfg.baseline.step_db) {$|      if (deviation > 0.0f) {|'
sabotage live include/omxdsp/analysis/omx_hrp_lv2.h \
  's|^        cand\[n\].freq_hz = f0 \* (float)(k + 1u);$|        cand[n].freq_hz = f0 * (float)(k + 1u) * 1.01f;|'
