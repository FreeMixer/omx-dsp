#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# delay_color-perturb.sh — test/fx/delay_color.test.c sees each moved declaration of the delay's sync,
# colour and duck, and each defect of them. Each sabotage is applied to a COPY of the include directory
# placed ahead of the real one, and the oracle built against it must go red in the named arm:
#   tapelp    — DELAY_TAPE_LP_HZ moved from 6000 to 7000 Hz: arm C;
#   analoglp  — DELAY_ANALOG_LP_HZ moved from 3000 to 3500 Hz: arm E;
#   wowhz     — DELAY_TAPE_WOW_HZ moved from 0.4 to 0.5 Hz: arm D;
#   wowms     — DELAY_TAPE_WOW_MS moved from 0.3 to 0.4 ms: arm D;
#   fallback  — DELAY_TEMPO_FALLBACK_BPM moved from 120 to 100: arm B;
#   beats     — DELAY_DIVISION_BEATS' dotted eighth moved from 0.75 to 0.8 beats: arm B;
#   attack    — FX_DUCK_ATTACK_MS moved from 5 to 6 ms: arm F;
#   loop      — tape's loop low pass never runs: arm C;
#   wow       — the wow never moves the head: arm D;
#   soft      — analog's saturation is the identity: arm E;
#   duck      — the wet is never ducked: arm F.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… delay_color-perturb.sh <scratch-dir>
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
    echo "delay_color-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/delay_color.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "delay_color-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "delay_color-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "delay_color-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage tapelp omxdsp_definitions.h 's|^#define OMX_DELAY_TAPE_LP_HZ 6000$|#define OMX_DELAY_TAPE_LP_HZ 7000|' C &
pids+=($!)
sabotage analoglp omxdsp_definitions.h 's|^#define OMX_DELAY_ANALOG_LP_HZ 3000$|#define OMX_DELAY_ANALOG_LP_HZ 3500|' E &
pids+=($!)
sabotage wowhz omxdsp_definitions.h 's|^#define OMX_DELAY_TAPE_WOW_HZ 0.4f$|#define OMX_DELAY_TAPE_WOW_HZ 0.5f|' D &
pids+=($!)
sabotage wowms omxdsp_definitions.h 's|^#define OMX_DELAY_TAPE_WOW_MS 0.3f$|#define OMX_DELAY_TAPE_WOW_MS 0.4f|' D &
pids+=($!)
sabotage fallback omxdsp_definitions.h 's|^#define OMX_DELAY_TEMPO_FALLBACK_BPM 120$|#define OMX_DELAY_TEMPO_FALLBACK_BPM 100|' B &
pids+=($!)
sabotage beats omxdsp_definitions.h 's|0.5f, 0.75f, 0.3333333333333333f|0.5f, 0.8f, 0.3333333333333333f|' B &
pids+=($!)
sabotage attack omxdsp_definitions.h 's|^#define OMX_FX_DUCK_ATTACK_MS 5$|#define OMX_FX_DUCK_ATTACK_MS 6|' F &
pids+=($!)
sabotage loop fx/omx_delay.h 's|  const float lp = (tape \|\| analog) ? c->loop_pole : 0.0f;|  const float lp = analog ? c->loop_pole : 0.0f;|' C &
pids+=($!)
sabotage wow fx/omx_delay.h 's|  const int wow = tape \&\& c->wow_depth > 0.0f;|  const int wow = 0;|' D &
pids+=($!)
sabotage soft fx/omx_delay.h 's|static inline float omx_fxdelay_soft(float x) { return x / (1.0f + fabsf(x)); }|static inline float omx_fxdelay_soft(float x) { return x; }|' E &
pids+=($!)
sabotage duck fx/omx_delay.h 's|  const int ducked = col \&\& c->duck.depth_db < 0.0f;|  const int ducked = 0;|' F &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "delay_color-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "delay_color-perturb: the delay colour oracle sees all eleven sabotages"
