#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# balancer_instance-perturb.sh — test/fx/balancer_instance.test.c sees each moved declaration of the spectral balancer and each
# defect of its word. Each sabotage is applied to a COPY of the include directory placed ahead of the real
# one, and the oracle built against it must go red in the named arm:
#   band — BALANCER_BAND_HZ's first moved from 125 to 150: arm B;
#   q — BALANCER_Q moved from 0.7 to 1: arm B;
#   slope — the target at half the slope: arm B C;
#   amount — the amount is ignored: arm B;
#   range — the downward range is not held: arm D;
#   speed — the walk at twice its speed: arm D;
#   gate — the gate far under silence: arm E;
#   settle — the gains move before the detector has settled: arm E;
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… balancer_instance-perturb.sh <scratch-dir>
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
    echo "balancer_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/balancer_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "balancer_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "balancer_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "balancer_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage band omxdsp_definitions.h 's|^#define OMX_BALANCER_BAND_HZ_INIT { 125,|#define OMX_BALANCER_BAND_HZ_INIT { 150,|' B &
pids+=($!)
sabotage q omxdsp_definitions.h 's|^#define OMX_BALANCER_Q_DOUBLE 0.7$|#define OMX_BALANCER_Q_DOUBLE 1|' B &
pids+=($!)
sabotage slope fx/omx_balancer_instance.h 's|(s->slope + (float)OMX_BALANCER_BAND_DB_OCT)|(0.5f * s->slope + (float)OMX_BALANCER_BAND_DB_OCT)|' B C &
pids+=($!)
sabotage amount fx/omx_balancer_instance.h 's|  s->amount = am \* 0.01f;|  s->amount = 1.0f + 0.0f * am;|' B &
pids+=($!)
sabotage range fx/omx_balancer_instance.h 's|    want = want > s->range ? s->range : want < -s->range ? -s->range : want;|    want = want > s->range ? s->range : want;|' D &
pids+=($!)
sabotage speed fx/omx_balancer_instance.h 's|  s->step_db = sp / (float)OMX_BALANCER_INSTANCE_TICKS_PER_S;|  s->step_db = 2.0f * sp / (float)OMX_BALANCER_INSTANCE_TICKS_PER_S;|' D &
pids+=($!)
sabotage gate omxdsp_definitions.h 's|^#define OMX_BALANCER_GATE_DBFS -60$|#define OMX_BALANCER_GATE_DBFS -400|' E &
pids+=($!)
sabotage settle fx/omx_balancer_instance.h 's/  if (s->settled < s->settle || mean <=/  if (mean <=/' E &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || exit 1
echo "balancer_instance-perturb: the oracle sees all 8 sabotages"
