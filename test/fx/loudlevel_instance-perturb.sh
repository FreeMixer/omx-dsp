#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# loudlevel_instance-perturb.sh — test/fx/loudlevel_instance.test.c sees each moved declaration of the leveler to a loudness target and each
# defect of its word. Each sabotage is applied to a COPY of the include directory placed ahead of the real
# one, and the oracle built against it must go red in the named arm:
#   target — the target off by half a dB: arm B;
#   range — the downward range is not held: arm C;
#   speed — the walk runs at twice its speed: arm D;
#   gate — the gate is gone: arm E;
#   fill — the gain moves before the window has filled: arm E;
#   ramp — the gain steps at each tick: arm F;
#   kweight — the loudness offset moved by a dB: arm B;
#   window — the short window is ignored: arm E;
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… loudlevel_instance-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"
pids=()

sabotage() { # <name> <header under include/omxdsp> <sed expression> <arm>...
  local name="$1" hdr="$2" expr="$3" d="$OUT/$1"
  shift 3
  mkdir -p "$d"
  cp -r "$PKG/include/omxdsp" "$d/"
  sed -i "$expr" "$d/omxdsp/$hdr"
  if cmp -s "$PKG/include/omxdsp/$hdr" "$d/omxdsp/$hdr"; then
    echo "loudlevel_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/loudlevel_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "loudlevel_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "loudlevel_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "loudlevel_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage target fx/omx_loudlevel_instance.h 's|    want = s->target - s->lufs;|    want = s->target - s->lufs + 0.5f;|' B &
pids+=($!)
sabotage range fx/omx_loudlevel_instance.h 's|    want = want > s->range ? s->range : want < -s->range ? -s->range : want;|    want = want > s->range ? s->range : want;|' C &
pids+=($!)
sabotage speed fx/omx_loudlevel_instance.h 's|  s->step_db = sp / (float)OMX_LOUDNESS_SUBBLOCKS_PER_S;|  s->step_db = 2.0f * sp / (float)OMX_LOUDNESS_SUBBLOCKS_PER_S;|' D &
pids+=($!)
sabotage gate fx/omx_loudlevel_instance.h 's/  if (s->ticks >= full \&\& s->lufs > s->gate) {/  if (s->ticks >= full) {/' E &
pids+=($!)
sabotage fill fx/omx_loudlevel_instance.h 's/  if (s->ticks >= full \&\& s->lufs > s->gate) {/  if (s->lufs > s->gate) {/' E &
pids+=($!)
sabotage ramp fx/omx_loudlevel_instance.h 's|      const float g = s->gain_lin + s->ramp_step \* (float)(done + i);|      const float g = s->ramp_end + 0.0f * (float)done;|' F &
pids+=($!)
sabotage kweight omx_loudness.h 's|^#define OMX_LOUDNESS_OFFSET_LU OMX_BS1770_LOUDNESS_OFFSET_LU_DOUBLE$|#define OMX_LOUDNESS_OFFSET_LU (OMX_BS1770_LOUDNESS_OFFSET_LU_DOUBLE - 1.0)|' B &
pids+=($!)
sabotage window fx/omx_loudlevel_instance.h 's/const double ms = s->win == (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY ? /const double ms = 1 ? /' E &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || exit 1
echo "loudlevel_instance-perturb: the oracle sees all 8 sabotages"
