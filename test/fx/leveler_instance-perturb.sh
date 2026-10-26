#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# leveler_instance-perturb.sh — test/fx/leveler_instance.test.c sees each moved row of the leveler's table and each defect of its face. Each sabotage is applied to a COPY
# of the include directory placed ahead of the real one, and the oracle built against it must go red
# in the named arm:
#   points    — LEVELER_AMOUNT_POINTS' second point moved from 25 to 30: arms B, C;
#   downt     — LEVELER_DOWN_THRESHOLD_DB's second row moved from -10 to -11 dB: arms B, C;
#   downr     — LEVELER_DOWN_RATIO's last row moved from 4 to 4.5: arms B, C;
#   downm     — LEVELER_DOWN_MAKEUP_DB's last row moved from 6 to 6.5 dB: arms B, C;
#   upt       — LEVELER_UP_THRESHOLD_DB's last row moved from -50 to -51 dB: arms B, C;
#   upr       — LEVELER_UP_RATIO's last row moved from 3 to 3.5: arms B, C;
#   uprange   — LEVELER_UP_RANGE_DB's last row moved from 12 to 13 dB: arms B, C;
#   attack    — the medium attack moved from 10 to 12 ms: arm D;
#   release   — the fast release moved from 100 to 120 ms: arm D;
#   knee      — LEVELER_KNEE_DB moved from 6 to 8 dB: arms B, C;
#   direction — the downward slot run when the direction is up: arm C;
#   engage    — the upward slot's state kept across its engage edge: arm G.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… leveler_instance-perturb.sh <scratch-dir>
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
    echo "leveler_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/leveler_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "leveler_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "leveler_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "leveler_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage points omxdsp_definitions.h 's|^#define OMX_LEVELER_AMOUNT_POINTS_INIT { 0, 25, 50, 75, 100 }$|#define OMX_LEVELER_AMOUNT_POINTS_INIT { 0, 30, 50, 75, 100 }|' B C &
pids+=($!)
sabotage downt omxdsp_definitions.h 's|^#define OMX_LEVELER_DOWN_THRESHOLD_DB_INIT { 0, -10, |#define OMX_LEVELER_DOWN_THRESHOLD_DB_INIT { 0, -11, |' B C &
pids+=($!)
sabotage downr omxdsp_definitions.h 's|^#define OMX_LEVELER_DOWN_RATIO_INIT { 1, 1.5f, 2, 3, 4 }$|#define OMX_LEVELER_DOWN_RATIO_INIT { 1, 1.5f, 2, 3, 4.5f }|' B C &
pids+=($!)
sabotage downm omxdsp_definitions.h 's|^#define OMX_LEVELER_DOWN_MAKEUP_DB_INIT { 0, 1, 2.5f, 4, 6 }$|#define OMX_LEVELER_DOWN_MAKEUP_DB_INIT { 0, 1, 2.5f, 4, 6.5f }|' B C &
pids+=($!)
sabotage upt omxdsp_definitions.h 's|^#define OMX_LEVELER_UP_THRESHOLD_DB_INIT { -40, -42, -45, -48, -50 }$|#define OMX_LEVELER_UP_THRESHOLD_DB_INIT { -40, -42, -45, -48, -51 }|' B C &
pids+=($!)
sabotage upr omxdsp_definitions.h 's|^#define OMX_LEVELER_UP_RATIO_INIT { 1, 1.5f, 2, 2.5f, 3 }$|#define OMX_LEVELER_UP_RATIO_INIT { 1, 1.5f, 2, 2.5f, 3.5f }|' B C &
pids+=($!)
sabotage uprange omxdsp_definitions.h 's|^#define OMX_LEVELER_UP_RANGE_DB_INIT { 0, 3, 6, 9, 12 }$|#define OMX_LEVELER_UP_RANGE_DB_INIT { 0, 3, 6, 9, 13 }|' B C &
pids+=($!)
sabotage attack omxdsp_definitions.h 's|^#define OMX_LEVELER_SPEED_ATTACK_MS_INIT { 30, 10, 3 }$|#define OMX_LEVELER_SPEED_ATTACK_MS_INIT { 30, 12, 3 }|' D &
pids+=($!)
sabotage release omxdsp_definitions.h 's|^#define OMX_LEVELER_SPEED_RELEASE_MS_INIT { 800, 300, 100 }$|#define OMX_LEVELER_SPEED_RELEASE_MS_INIT { 800, 300, 120 }|' D &
pids+=($!)
sabotage knee omxdsp_definitions.h 's|^#define OMX_LEVELER_KNEE_DB 6$|#define OMX_LEVELER_KNEE_DB 8|' B C &
pids+=($!)
sabotage direction fx/omx_leveler_instance.h 's|s->down.enabled = s->dir != OMX_LEVELER_DIRECTIONS_UP;|s->down.enabled = 1;|' C &
pids+=($!)
sabotage engage fx/omx_leveler_instance.h 's|  if (s->enabled [\&][\&] s->up.enabled [\&][\&] !up_was) omx_dyn_state_init|  if (0 \&\& !up_was) omx_dyn_state_init|' G &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "leveler_instance-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "leveler_instance-perturb: the oracle sees all twelve sabotages"
