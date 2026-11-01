#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# spring_instance-perturb.sh — test/fx/spring_instance.test.c sees each moved declaration of the spring and each
# defect of its word. Each sabotage is applied to a COPY of the include directory placed ahead of the real
# one, and the oracle built against it must go red in the named arm:
#   ratio — SPRING_RIGHT_RATIO moved from 1.13 to 1.05: arm B;
#   sections — SPRING_SECTIONS moved from 40 to 30: arm C;
#   chirp — SPRING_CHIRP_HZ's first moved from 1200 to 1500: arm C;
#   decay — the tail falls at half its rate: arm D;
#   length — the left spring is a tenth long: arm B;
#   clear — the tank is never cleared: arm F;
#   bypass — a mix of 0 still runs the tank: arm F;
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… spring_instance-perturb.sh <scratch-dir>
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
    echo "spring_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/spring_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "spring_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "spring_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "spring_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage ratio omxdsp_definitions.h 's|^#define OMX_SPRING_RIGHT_RATIO_DOUBLE 1.13$|#define OMX_SPRING_RIGHT_RATIO_DOUBLE 1.05|' B &
pids+=($!)
sabotage sections omxdsp_definitions.h 's|^#define OMX_SPRING_SECTIONS 40$|#define OMX_SPRING_SECTIONS 30|' C &
pids+=($!)
sabotage chirp omxdsp_definitions.h 's|^#define OMX_SPRING_CHIRP_HZ_INIT { 1200,|#define OMX_SPRING_CHIRP_HZ_INIT { 1500,|' C &
pids+=($!)
sabotage decay fx/omx_spring_instance.h 's|pow(10.0, -3.0 \* (((double)l->d + chain)|pow(10.0, -1.5 * (((double)l->d + chain)|' D &
pids+=($!)
sabotage length fx/omx_spring_instance.h 's|  const uint32_t d0 = (uint32_t)lrint((double)ln \* 0.001|  const uint32_t d0 = (uint32_t)lrint((double)ln * 0.0011|' B &
pids+=($!)
sabotage clear fx/omx_spring_instance.h 's/  if (s->enabled \&\& (!was || moved)) {/  if (0 \&\& s->enabled \&\& (!was || moved)) {/' F &
pids+=($!)
sabotage bypass fx/omx_spring_instance.h 's|  s->enabled = !bypass \&\& mx > 0.0f;|  s->enabled = !bypass;|' F &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || exit 1
echo "spring_instance-perturb: the oracle sees all 7 sabotages"
