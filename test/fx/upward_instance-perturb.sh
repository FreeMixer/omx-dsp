#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# upward_instance-perturb.sh — test/fx/upward_instance.test.c sees each moved declaration of the upward face and each
# defect of its word. Each sabotage is applied to a COPY of the include directory placed ahead of the real
# one, and the oracle built against it must go red in the named arm:
#   knee — UPWARD_KNEE_DB moved from 6 to 8: arm B;
#   mode — the slot runs the downward law: arm B;
#   rms — the detector reads peak: arm C;
#   rel — the release runs at twice its time: arm D;
#   range — the cap is twice the range: arm B;
#   idle — a range of 0 or a ratio of 1 still runs the slot: arm A;
#   readout — the readout reads half: arm E;
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… upward_instance-perturb.sh <scratch-dir>
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
    echo "upward_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/upward_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "upward_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "upward_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "upward_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage knee omxdsp_definitions.h 's|^#define OMX_UPWARD_KNEE_DB 6$|#define OMX_UPWARD_KNEE_DB 8|' B &
pids+=($!)
sabotage mode fx/omx_upward_instance.h 's|  d->gc.mode = OMX_DYN_UPWARD;|  d->gc.mode = OMX_DYN_ABOVE;|' B &
pids+=($!)
sabotage rms fx/omx_upward_instance.h 's|  d->detect = OMX_DETECT_RMS;|  d->detect = OMX_DETECT_PEAK;|' C &
pids+=($!)
sabotage rel fx/omx_upward_instance.h 's|  d->release_coeff = omx_pole_from_time_ms(rl, s->sr);|  d->release_coeff = omx_pole_from_time_ms(rl * 2.0f, s->sr);|' D &
pids+=($!)
sabotage range fx/omx_upward_instance.h 's|  d->gc.range_db = rg;|  d->gc.range_db = 2.0f * rg;|' B &
pids+=($!)
sabotage idle fx/omx_upward_instance.h 's|  s->enabled = !bypass \&\& ra > 1.0f \&\& rg > 0.0f;|  s->enabled = !bypass \&\& rg > -1.0f;|' A &
pids+=($!)
sabotage readout fx/omx_upward_instance.h 's|omx_lin_to_db(after / before) : 0.0f|0.5f * omx_lin_to_db(after / before) : 0.0f|' E &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || exit 1
echo "upward_instance-perturb: the oracle sees all 7 sabotages"
