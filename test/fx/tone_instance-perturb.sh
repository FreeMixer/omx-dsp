#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# tone_instance-perturb.sh — test/fx/tone_instance.test.c sees each moved declaration of the tone face
# and each defect of its laws. Each sabotage is applied to a COPY of the include directory placed ahead
# of the real one, and the oracle built against it must go red in the named arm:
#   low       — TONE_FILTER_LOW_HZ moved from 20 to 21 Hz: arm C;
#   high      — TONE_FILTER_HIGH_HZ moved from 20000 to 19000 Hz: arm C;
#   tiltmax   — tiltDb's top moved from 6 to 7 dB: arm E;
#   pivot     — pivotHz's default moved from 1000 to 1100 Hz: arm F;
#   resonance — resonance's default moved from 0.71 to 0.8: arm F;
#   span      — the span read as a slope's a, 10^(tiltDb/20): arm B;
#   map       — the low-pass's corner rising with the word instead of falling: arm C;
#   rest      — the filter run at a filter of 0: arm A.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… tone_instance-perturb.sh <scratch-dir>
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
    echo "tone_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/tone_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "tone_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "tone_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "tone_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage low omxdsp_definitions.h 's|^#define OMX_TONE_FILTER_LOW_HZ 20$|#define OMX_TONE_FILTER_LOW_HZ 21|' C &
pids+=($!)
sabotage high omxdsp_definitions.h 's|^#define OMX_TONE_FILTER_HIGH_HZ 20000$|#define OMX_TONE_FILTER_HIGH_HZ 19000|' C &
pids+=($!)
sabotage tiltmax omxdsp_definitions.h 's|^#define OMX_TONE_TILT_DB_RANGE_MAX 6$|#define OMX_TONE_TILT_DB_RANGE_MAX 7|' E &
pids+=($!)
sabotage pivot omxdsp_definitions.h 's|^#define OMX_TONE_PIVOT_HZ_RANGE_DEFAULT 1000$|#define OMX_TONE_PIVOT_HZ_RANGE_DEFAULT 1100|' F &
pids+=($!)
sabotage resonance omxdsp_definitions.h 's|^#define OMX_TONE_RESONANCE_RANGE_DEFAULT 0.71f$|#define OMX_TONE_RESONANCE_RANGE_DEFAULT 0.8f|' F &
pids+=($!)
sabotage span omx_tilt.h 's|exp(sp \* (M_LN10 / 40.0))|exp(sp * (M_LN10 / 20.0))|' B &
pids+=($!)
sabotage map fx/omx_tone_instance.h 's|OMX_TONE_INSTANCE_HIGH_HZ \* exp(-u \* span)|OMX_TONE_INSTANCE_LOW_HZ * exp(u * span)|' C &
pids+=($!)
sabotage rest fx/omx_tone_instance.h 's|, filter_on = s->filter != 0.0f;|, filter_on = 1;|' A &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "tone_instance-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "tone_instance-perturb: the tone oracle sees all eight sabotages"
