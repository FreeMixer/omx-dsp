#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reverb_return-perturb.sh — test/fx/reverb_return.test.c sees each moved declaration of the reverb's
# return and each defect of it. Each sabotage is applied to a COPY of the include directory placed ahead
# of the real one, and the oracle built against it must go red in the named arm:
#   tapms     — REVERB_EARLY_TAP_MS's fourth tap moved from 10.7 to 11.7 ms: arm B;
#   tapgain   — REVERB_EARLY_TAP_GAINS' first gain moved from 0.84 to 0.8: arm B;
#   sizemin   — REVERB_EARLY_SIZE_MIN moved from 0.5 to 0.6: arm B;
#   sizemax   — REVERB_EARLY_SIZE_MAX moved from 1.5 to 1.4: arm B;
#   early     — the early taps never reach the wet: arm B;
#   legs      — the odd taps land on the left: arm B;
#   tail      — the tail's level is never applied: arms B and C;
#   duck      — the wet is never ducked: arm D;
#   deess     — the wet de-esser never runs: arm E.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… reverb_return-perturb.sh <scratch-dir>
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
    echo "reverb_return-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/reverb_return.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "reverb_return-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "reverb_return-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "reverb_return-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage tapms omxdsp_definitions.h 's|^#define OMX_REVERB_EARLY_TAP_MS_INIT { 3.1f, 5.3f, 7.9f, 10.7f,|#define OMX_REVERB_EARLY_TAP_MS_INIT { 3.1f, 5.3f, 7.9f, 11.7f,|' B &
pids+=($!)
sabotage tapgain omxdsp_definitions.h 's|^#define OMX_REVERB_EARLY_TAP_GAINS_INIT { 0.84f,|#define OMX_REVERB_EARLY_TAP_GAINS_INIT { 0.8f,|' B &
pids+=($!)
sabotage sizemin omxdsp_definitions.h 's|^#define OMX_REVERB_EARLY_SIZE_MIN 0.5f$|#define OMX_REVERB_EARLY_SIZE_MIN 0.6f|' B &
pids+=($!)
sabotage sizemax omxdsp_definitions.h 's|^#define OMX_REVERB_EARLY_SIZE_MAX 1.5f$|#define OMX_REVERB_EARLY_SIZE_MAX 1.4f|' B &
pids+=($!)
sabotage early fx/omx_reverb.h 's|^        wetL += el\[i\];$|        wetL += 0.0f * el[i];|' B &
pids+=($!)
sabotage legs fx/omx_reverb.h 's|for (uint32_t t = 1; t < OMX_REVERB_EARLY_TAPS; t += 2u) b += |for (uint32_t t = 1; t < OMX_REVERB_EARLY_TAPS; t += 2u) a += |' B &
pids+=($!)
sabotage tail fx/omx_reverb.h 's|  const int tail_scaled = ret \&\& x->tail_lin != 1.0f;|  const int tail_scaled = 0;|' B C &
pids+=($!)
sabotage duck fx/omx_reverb.h 's|  const int ducked = ret \&\& x->duck.depth_db < 0.0f;|  const int ducked = 0;|' D &
pids+=($!)
sabotage deess fx/omx_reverb.h 's|  const int deessed = ret \&\& x->deess != NULL \&\& x->deess->enabled;|  const int deessed = 0;|' E &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "reverb_return-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "reverb_return-perturb: the reverb return oracle sees all nine sabotages"
