#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# width_instance-perturb.sh — test/fx/width_instance.test.c sees each moved declaration of the width face and each defect of its word. Each sabotage is applied to a COPY
# of the include directory placed ahead of the real one, and the oracle built against it must go red
# in the named arm:
#   gain        — WIDTH_COMB_GAIN moved from 0.5 to 0.6: arm C;
#   sign        — the comb's side added with the wrong sign: arm C;
#   round       — the delay truncated, not rounded: arms C, D;
#   order       — bass mono split by an LR2: arm E;
#   scale       — the width read as half: arm B;
#   haasleg     — Haas delaying the left leg's signal onto the right: arm D;
#   active      — the word run at its come-up values: arm A;
#   bassdefault — bassMonoHz's default moved from 120 to 130 Hz: arm F;
#   mstop       — pseudoMs' top moved from 30 to 25 ms: arm F;
#   ringkeep    — the ring kept across a pseudo-mode or delay change: arm G;
#   engagekeep  — the state kept across the engage edge: arm G.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… width_instance-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"
# The sabotages are independent and run side by side; each is waited for and every one must be seen.
pids=()

sabotage() { # <name> <header under include/omxdsp> <sed expression> <arm>...
  local name="$1" hdr="$2" expr="$3" d="$OUT/$1"
  shift 3
  mkdir -p "$d"
  cp -r "$PKG/include/omxdsp" "$d/"
  sed -i "$expr" "$d/omxdsp/$hdr"
  if cmp -s "$PKG/include/omxdsp/$hdr" "$d/omxdsp/$hdr"; then
    echo "width_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/width_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "width_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "width_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "width_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage gain omxdsp_definitions.h 's|^#define OMX_WIDTH_COMB_GAIN 0.5f$|#define OMX_WIDTH_COMB_GAIN 0.6f|' C &
pids+=($!)
sabotage sign omx_width.h 's|      s += OMX_WIDTH_COMB_GAIN \* st->ring|      s -= OMX_WIDTH_COMB_GAIN * st->ring|' C &
pids+=($!)
sabotage round omx_width.h 's|  const uint32_t d = (uint32_t)lrintf(ms \* 0.001f \* sr);|  const uint32_t d = (uint32_t)(ms * 0.001f * sr);|' C D &
pids+=($!)
sabotage order omx_width.h 's|  if (p->bass_mono) omx_xover_design(\&p->x, 4u, bass_hz, sr);|  if (p->bass_mono) omx_xover_design(\&p->x, 2u, bass_hz, sr);|' E &
pids+=($!)
sabotage scale fx/omx_width_instance.h 's|s->wpct \* 0.01f|s->wpct * 0.005f|' B &
pids+=($!)
sabotage haasleg omx_width.h 's|      st->ring\[st->pos\] = r\[i\];|      st->ring[st->pos] = l[i];|' D &
pids+=($!)
sabotage active omx_width.h 's|  p->active = w != 1.0f \|\| p->pseudo != OMX_WIDTH_OFF \|\| p->bass_mono;|  p->active = 1;|' A &
pids+=($!)
sabotage bassdefault omxdsp_definitions.h 's|^#define OMX_WIDTH_BASS_MONO_HZ_RANGE_DEFAULT 120$|#define OMX_WIDTH_BASS_MONO_HZ_RANGE_DEFAULT 130|' F &
pids+=($!)
sabotage mstop omxdsp_definitions.h 's|^#define OMX_WIDTH_PSEUDO_MS_RANGE_MAX 30$|#define OMX_WIDTH_PSEUDO_MS_RANGE_MAX 25|' F &
pids+=($!)
sabotage ringkeep fx/omx_width_instance.h 's|    if (s->p.pseudo != was_pseudo [|][|] s->p.d != was_d) {|    if (0 \&\& (s->p.pseudo != was_pseudo \|\| s->p.d != was_d)) {|' G &
pids+=($!)
sabotage engagekeep fx/omx_width_instance.h 's|  if (s->enabled [\&][\&] s->p.active [\&][\&] !was_live) {|  if (0 \&\& !was_live) {|' G &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "width_instance-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "width_instance-perturb: the oracle sees all eleven sabotages"
