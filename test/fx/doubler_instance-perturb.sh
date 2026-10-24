#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# doubler_instance-perturb.sh — test/fx/doubler_instance.test.c sees each moved declaration of the
# doubler and each defect of its face. Each sabotage is applied to a COPY of the include directory placed
# ahead of the real one, and the oracle built against it must go red in the named arm:
#   detunes   — DOUBLER_VOICE_DETUNES' first share moved from 1 to 0.9: arm B;
#   delays    — DOUBLER_VOICE_DELAYS' first share moved from 1 to 1.05: arm C;
#   pans      — DOUBLER_VOICE_PANS' first place moved from -1 to -0.9: arm E;
#   window    — DOUBLER_WINDOW_MS moved from 20 to 18 ms: arm B;
#   fade      — DOUBLER_FADE moved from 0.25 to 0.3: arm E;
#   drift     — every voice drifts in step, no voice a further share of a turn along: arm E;
#   count     — the voices' sum is not divided by their count: arm E.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… doubler_instance-perturb.sh <scratch-dir>
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
    echo "doubler_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/doubler_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "doubler_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "doubler_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "doubler_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage detunes omxdsp_definitions.h 's|^#define OMX_DOUBLER_VOICE_DETUNES_INIT { 1, -1,|#define OMX_DOUBLER_VOICE_DETUNES_INIT { 0.9f, -1,|' B &
pids+=($!)
sabotage delays omxdsp_definitions.h 's|^#define OMX_DOUBLER_VOICE_DELAYS_INIT { 1, 1.31f,|#define OMX_DOUBLER_VOICE_DELAYS_INIT { 1.05f, 1.31f,|' C &
pids+=($!)
sabotage pans omxdsp_definitions.h 's|^#define OMX_DOUBLER_VOICE_PANS_INIT { -1, 1,|#define OMX_DOUBLER_VOICE_PANS_INIT { -0.9f, 1,|' E &
pids+=($!)
sabotage window omxdsp_definitions.h 's|^#define OMX_DOUBLER_WINDOW_MS 20$|#define OMX_DOUBLER_WINDOW_MS 18|' B &
pids+=($!)
sabotage fade omxdsp_definitions.h 's|^#define OMX_DOUBLER_FADE 0.25f$|#define OMX_DOUBLER_FADE 0.3f|' E &
pids+=($!)
sabotage drift fx/omx_doubler_instance.h 's|omx_lfo_at(\&s->lfo, (float)k / (float)s->nv)|omx_lfo_at(\&s->lfo, 0.0f * (float)k)|' E &
pids+=($!)
sabotage count fx/omx_doubler_instance.h 's|    s->gl\[k\] = bl / (float)s->nv;|    s->gl[k] = bl;|' E &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "doubler_instance-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "doubler_instance-perturb: the doubler oracle sees all seven sabotages"
