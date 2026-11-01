#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# delay_instance-perturb.sh — test/fx/delay_instance.test.c sees each moved declaration of the delay face's read taps and each
# defect of its word. Each sabotage is applied to a COPY of the include directory placed ahead of the real
# one, and the oracle built against it must go red in the named arm:
#   pan — each tap's pan is dropped: arm I;
#   factor — each factor read upside down: arm I;
#   unread — the run never reads the taps: arm I;
#   default — tap 1's default factor moved from 1/2 to 1/3: arm I;
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… delay_instance-perturb.sh <scratch-dir>
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
    echo "delay_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/delay_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "delay_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL .* Hz: $arm: " "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "delay_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "delay_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage pan fx/omx_delay_instance.h 's|omx_fxdelay_tap_legs(gk\[k\], pk\[k\], |omx_fxdelay_tap_legs(gk[k], 0.0f * pk[k], |' I &
pids+=($!)
sabotage factor fx/omx_delay_instance.h 's|OMX_DELAY_INSTANCE_TAP_NUMS\[fk\[k\]\], OMX_DELAY_INSTANCE_TAP_DENS\[fk\[k\]\]|OMX_DELAY_INSTANCE_TAP_DENS[fk[k]], OMX_DELAY_INSTANCE_TAP_NUMS[fk[k]]|' I &
pids+=($!)
sabotage unread fx/omx_delay_instance.h 's|\&s->taps, \&s->color|NULL, \&s->color|' I &
pids+=($!)
sabotage default omxdsp_definitions.h 's|^#define OMX_DELAY_TAP1_FACTORS_DEFAULT OMX_DELAY_TAP1_FACTORS_HALF$|#define OMX_DELAY_TAP1_FACTORS_DEFAULT OMX_DELAY_TAP1_FACTORS_THIRD|' I &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || exit 1
echo "delay_instance-perturb: the oracle sees all 4 sabotages"
