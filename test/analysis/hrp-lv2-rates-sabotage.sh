#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# hrp-lv2-rates-sabotage.sh <dir> — one sabotage per arm of test/analysis/hrp_lv2_rates.test.c: a
# SABOTAGED COPY of omx_hrp_lv2.h is placed ahead of the real one, the oracle is built against it,
# and it must fail, at every rate, on the check that arm makes. The tree is never edited, and a sed
# that matches nothing is a failure, not a skip.
#
#   due     the worker is asked for one frame before a whole hop is unconsumed
#   idle    an idle shell scales its output by one ulp below 1
#   cursor  a pass consumes one frame less than the hop it read
#   band    run() skips the cascade, so an adopted band does not reach the audio
#   walk    the ramp moves half a step per pass
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
HDR=include/omxdsp/analysis/omx_hrp_lv2.h
nrates="$(printf '#include <omxcontract/omx_contract_limits.h>\nOMX_RME_RATES_COUNT\n' |
  $CC -E -P -isystem "$CONTRACT_INC" - | tail -1 | tr -d '() uU')"
rm -rf "$OUT" && mkdir -p "$OUT"

# sabotage <arm> <check text the arm must fail> <sed expression>
sabotage() {
  local arm="$1" want="$2" expr="$3" dir="$OUT/$1"
  mkdir -p "$dir/omxdsp/analysis"
  local copy="$dir/omxdsp/analysis/omx_hrp_lv2.h"
  cp "$HDR" "$copy"
  sed -i "$expr" "$copy"
  if cmp -s "$HDR" "$copy"; then
    echo "hrp-lv2-rates-sabotage.sh: FAIL — $arm: the line the sabotage targets in $HDR is gone; it would test nothing"
    exit 1
  fi
  $CC $CFLAGS -I"$dir" -Iinclude -isystem "$CONTRACT_INC" -Isrc -Itest/analysis -Itest/fx \
    -o "$dir/oracle" test/analysis/hrp_lv2_rates.test.c -lm
  if "$dir/oracle" > "$dir/log" 2>&1; then
    echo "hrp-lv2-rates-sabotage.sh: FAIL — $arm: the oracle passes over a sabotaged $HDR"
    exit 1
  fi
  local hit
  hit="$(grep -F "$want" "$dir/log" | sed 's/: .*//' | sort -u | wc -l)"
  if [ "$hit" != "$nrates" ]; then
    echo "hrp-lv2-rates-sabotage.sh: FAIL — $arm: '$want' failed at $hit of $nrates rates"
    cat "$dir/log"
    exit 1
  fi
  echo "hrp-lv2-rates-sabotage.sh: $arm: '$want' fails at all $hit rates"
}

sabotage due "hop - 1 frames: not due" \
  's|^  if (written - consumed < (uint64_t)s->render.cfg.hop) return 0;$|  if (written - consumed + 1u < (uint64_t)s->render.cfg.hop) return 0;|'
sabotage idle "idle shell: one second out byte for byte" \
  's|^  omx_hrp_cascade_apply(&s->live, in, out, n);$|  omx_hrp_cascade_apply(\&s->live, in, out, n); if (!omx_hrp_cascade_live(\&s->live)) for (uint32_t i = 0; i < n; i++) out[i] *= 0.99999994f;|'
sabotage cursor "k passes over 3 hops consume k hops" \
  's|^  atomic_store_explicit(&s->consumed, consumed + (uint64_t)hop, memory_order_release);$|  atomic_store_explicit(\&s->consumed, consumed + (uint64_t)hop - 1u, memory_order_release);|'
sabotage band "a sine at the centre comes out at |H|" \
  's|^  omx_hrp_cascade_apply(&s->live, in, out, n);$|  if (out != in) memcpy(out, in, (size_t)n * sizeof(float));|'
sabotage walk "the walk moves one step per pass" \
  's|^  const float step = OMX_HRP_GAIN_STEP_DB;$|  const float step = 0.5f * OMX_HRP_GAIN_STEP_DB;|'
