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
# and the calls a plugin face drives the shell through:
#
#   block-size       the analysis block is sized one half-window short
#   block-partition  init_block lays the hop buffer over the residual
#   ask-default      a non-finite family word reads as the last family, not the declared default
#   ask-bypass       a bypass word of exactly one half bypasses
#   install          install_ask never re-points the family
#   asked            mark_asked counts two passes for one
#   in-flight        "in flight" compares the counters by order, so the 2^32 wrap breaks it
#   clear            clear leaves the cascade running
#   restart-slots    restart leaves the band slots
#   restart-cursors  restart leaves the consumed cursor
#   wants            wants ignores the snapshot's bypass
#   answer           answer analyses without installing the snapshot
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
  hit="$({ grep -F "$want" "$dir/log" || true; } | sed 's/: .*//' | sort -u | wc -l)"
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
sabotage block-size "block_floats = five windows" \
  's|^  return 4u \* fft + 2u \* (fft / 2u);$|  return 4u * fft + (fft / 2u);|'
sabotage block-partition "the hop buffer is the block's last window" \
  's|^\( *block + 3u \* fft + half, block + 3u \* fft\) + 2u \* half);$|\1 + half);|'
sabotage ask-default "ask_of: a NaN family is no family" \
  's|^\( *\)OMX_HRP_FAMILY_RANGE_DEFAULT);$|\1OMX_HRP_FAMILY_RANGE_MAX);|'
sabotage ask-bypass "bypass 0.5 is not bypassed" \
  's|^  a.bypass = bypass > 0.5f ? 1 : 0;$|  a.bypass = bypass >= 0.5f ? 1 : 0;|'
sabotage install "install: the family and its search are installed" \
  's|^  if (a->family != s->family) omx_hrp_lv2_set_family(s, a->family);$|  if (0) omx_hrp_lv2_set_family(s, a->family);|'
sabotage asked "answered: not in flight" \
  's|^  if (s) atomic_fetch_add_explicit(&s->asked, 1u, memory_order_release);$|  if (s) atomic_fetch_add_explicit(\&s->asked, 2u, memory_order_release);|'
sabotage in-flight "wrapped counters: in flight" \
  's|^  return atomic_load_explicit(&s->asked, memory_order_acquire) !=$|  return atomic_load_explicit(\&s->asked, memory_order_acquire) >|'
sabotage clear "cleared: the cascade is empty" \
  '/^static inline void omx_hrp_lv2_clear(OmxHrpLv2 \*s) {$/,/^}$/ s|^  omx_hrp_cascade_init(&s->live);$|  (void)s;|'
sabotage restart-slots "restart: every slot free" \
  's|^  memset(s->slot, 0, sizeof(s->slot));$|  (void)0;|'
sabotage restart-cursors "restart: both cursors at zero" \
  's|^  atomic_store_explicit(&s->consumed, 0u, memory_order_relaxed);$|  (void)0;|'
sabotage wants "wants follows the snapshot's bypass" \
  's|^  return a ? omx_hrp_lv2_due(s, a->bypass) : 0;$|  return a ? omx_hrp_lv2_due(s, 0) : 0;|'
sabotage answer "answer installs the snapshot" \
  's|^  omx_hrp_lv2_install_ask(s, a);$|  (void)0;|'
