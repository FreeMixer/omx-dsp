#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# suboct_instance-perturb.sh — test/fx/suboct_instance.test.c sees each moved declaration of the sub-octaver and each defect of its laws. Each sabotage is applied to a COPY
# of the include directory placed ahead of the real one, and the oracle built against it must go red
# in the named arm:
#   lo      — SUBOCT_RANGE_LO_HZ's mid row moved from 70 to 75 Hz: arm I;
#   post1   — SUBOCT_POST1_HZ's high row moved from 360 to 330 Hz: arm I;
#   post2   — SUBOCT_POST2_HZ's low row moved from 45 to 50 Hz: arm I;
#   hi      — SUBOCT_RANGE_HI_HZ's low row moved from 140 to 150 Hz: arm I;
#   hyst    — SUBOCT_HYSTERESIS moved from 0.25 to 0.3: arm I;
#   envatt  — SUBOCT_ENV_ATTACK_MS moved from 1 to 2 ms: arm I;
#   envrel  — SUBOCT_ENV_RELEASE_MS moved from 100 to 120 ms: arm I;
#   ramp    — SUBOCT_GATE_RAMP_MS moved from 5 to 8 ms: arms G, I;
#   quarter — the second octave a quarter division of the first divider gating the tracked band: arm B;
#   gate    — the gate never closing: arm G;
#   dflt    — trackGateDb's default moved from -50 to -55 dBFS: arm I.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… suboct_instance-perturb.sh <scratch-dir>
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
    echo "suboct_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/suboct_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "suboct_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "suboct_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "suboct_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage lo omxdsp_definitions.h 's|^#define OMX_SUBOCT_RANGE_LO_HZ_INIT { 35, 70, 140 }$|#define OMX_SUBOCT_RANGE_LO_HZ_INIT { 35, 75, 140 }|' I &
pids+=($!)
sabotage post1 omxdsp_definitions.h 's|^#define OMX_SUBOCT_POST1_HZ_INIT { 90, 180, 360 }$|#define OMX_SUBOCT_POST1_HZ_INIT { 90, 180, 330 }|' I &
pids+=($!)
sabotage post2 omxdsp_definitions.h 's|^#define OMX_SUBOCT_POST2_HZ_INIT { 45, 90, 180 }$|#define OMX_SUBOCT_POST2_HZ_INIT { 50, 90, 180 }|' I &
pids+=($!)
sabotage hi omxdsp_definitions.h 's|^#define OMX_SUBOCT_RANGE_HI_HZ_INIT { 140, 280, 560 }$|#define OMX_SUBOCT_RANGE_HI_HZ_INIT { 150, 280, 560 }|' I &
pids+=($!)
sabotage hyst omxdsp_definitions.h 's|^#define OMX_SUBOCT_HYSTERESIS 0.25f$|#define OMX_SUBOCT_HYSTERESIS 0.3f|' I &
pids+=($!)
sabotage envatt omxdsp_definitions.h 's|^#define OMX_SUBOCT_ENV_ATTACK_MS 1$|#define OMX_SUBOCT_ENV_ATTACK_MS 2|' I &
pids+=($!)
sabotage envrel omxdsp_definitions.h 's|^#define OMX_SUBOCT_ENV_RELEASE_MS 100$|#define OMX_SUBOCT_ENV_RELEASE_MS 120|' I &
pids+=($!)
sabotage ramp omxdsp_definitions.h 's|^#define OMX_SUBOCT_GATE_RAMP_MS 5$|#define OMX_SUBOCT_GATE_RAMP_MS 8|' G I &
pids+=($!)
sabotage quarter fx/omx_suboct_instance.h 's|    float s2 = omx_divider_step(\&s->d2, s1, omx_suboct_arm_level(e1));|    float s2 = (s->d2.q = (s->d1.toggles \& 2u) ? -1.0f : 1.0f) * xt; (void)e1;|' B &
pids+=($!)
sabotage gate fx/omx_suboct_instance.h 's|    const float target = e >= s->gate_lin ? 1.0f : 0.0f;|    const float target = 1.0f;|' G &
pids+=($!)
sabotage dflt omxdsp_definitions.h 's|^#define OMX_SUBOCT_TRACK_GATE_DB_RANGE_DEFAULT -50$|#define OMX_SUBOCT_TRACK_GATE_DB_RANGE_DEFAULT -55|' I &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "suboct_instance-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "suboct_instance-perturb: the oracle sees all eleven sabotages"
