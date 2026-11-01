#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# glue_instance-perturb.sh — test/fx/glue_instance.test.c sees each moved declaration of the glue face and each
# defect of its word. Each sabotage is applied to a COPY of the include directory placed ahead of the real
# one, and the oracle built against it must go red in the named arm:
#   knee — GLUE_KNEE_DB moved from 3 to 4: arm C;
#   ratio — GLUE_RATIO_VALUES' second moved from 4 to 3: arm B C;
#   attack — GLUE_ATTACK_MS' first moved from 0.1 to 0.2: arm B;
#   release — GLUE_RELEASE_MS' first moved from 100 to 120: arm B;
#   auto — the last detent is not the program release: arm B;
#   hp — the corner never reaches the detector: arm B D;
#   mix — the mix never reaches the slot: arm A B;
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… glue_instance-perturb.sh <scratch-dir>
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
    echo "glue_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/glue_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "glue_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "glue_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "glue_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage knee omxdsp_definitions.h 's|^#define OMX_GLUE_KNEE_DB 3$|#define OMX_GLUE_KNEE_DB 4|' C &
pids+=($!)
sabotage ratio omxdsp_definitions.h 's|^#define OMX_GLUE_RATIO_VALUES_INIT { 2, 4, 10 }|#define OMX_GLUE_RATIO_VALUES_INIT { 2, 3, 10 }|' B C &
pids+=($!)
sabotage attack omxdsp_definitions.h 's|^#define OMX_GLUE_ATTACK_MS_INIT { 0.1f,|#define OMX_GLUE_ATTACK_MS_INIT { 0.2f,|' B &
pids+=($!)
sabotage release omxdsp_definitions.h 's|^#define OMX_GLUE_RELEASE_MS_INIT { 100,|#define OMX_GLUE_RELEASE_MS_INIT { 120,|' B &
pids+=($!)
sabotage auto fx/omx_glue_instance.h 's/is_auto ? (int)OMX_COMP_RELEASE_MODES_AUTO : /0 ? (int)OMX_COMP_RELEASE_MODES_AUTO : /' B &
pids+=($!)
sabotage hp fx/omx_glue_instance.h 's/(int)OMX_COMP_KEY_SOURCES_SELF, hp, 0.0f/(int)OMX_COMP_KEY_SOURCES_SELF, 0.0f * hp, 0.0f/' B D &
pids+=($!)
sabotage mix fx/omx_glue_instance.h 's/rel_ms, mk, mx, (int)/rel_ms, mk, 100.0f + 0.0f * mx, (int)/' A B &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || exit 1
echo "glue_instance-perturb: the oracle sees all 7 sabotages"
