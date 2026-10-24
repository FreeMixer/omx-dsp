#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# gate_keyfilter-perturb.sh — test/fx/gate_keyfilter.test.c sees each defect of the gate's key
# filter. Each sabotage is applied to a COPY of omx_gate.h placed ahead of the real include
# directory, and the oracle built against it must go red in the named arm:
#   q        — the high-pass designed at Q 1, not the Butterworth's 1/sqrt 2: the closed-form arm;
#   lp       — the low-pass never runs: the closed-form and live arms;
#   self     — the self key is never filtered, only an external one: the live arm;
#   history  — an edge that comes in keeps the history it had before it went out: the history arm.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… gate_keyfilter-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"

sabotage() { # name sed-expression arm...
  local name=$1 expr=$2 d="$OUT/$1"
  shift 2
  mkdir -p "$d/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$d/omxdsp/"
  sed -i "$expr" "$d/omxdsp/omx_gate.h"
  if cmp -s "$PKG/include/omxdsp/omx_gate.h" "$d/omxdsp/omx_gate.h"; then
    echo "gate_keyfilter-perturb: FAIL — sabotage '$name' matched nothing in omx_gate.h"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/gate_keyfilter.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "gate_keyfilter-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm
  for arm in "$@"; do
    grep -q "^FAIL \[$arm\]" "$d/oracle.log" \
      || { echo "gate_keyfilter-perturb: FAIL — sabotage '$name' did not turn the $arm arm red"; head -5 "$d/oracle.log"; exit 1; }
  done
  echo "gate_keyfilter-perturb: sabotage '$name' turned the $* arm(s) red"
}

sabotage q 's|omx_eq_design(OMX_EQ_HIGHPASS, hp_hz, M_SQRT1_2, 0.0, rate, k->hp_c);|omx_eq_design(OMX_EQ_HIGHPASS, hp_hz, 1.0, 0.0, rate, k->hp_c);|' 'B closed form'
sabotage lp 's|^  if (k->lp_hz > 0.0f)$|  if (0)|' 'B closed form' 'C live'
sabotage self 's|    if (built \&\& p->enabled) {|    if (keyed \&\& built \&\& p->enabled) {|' 'C live'
sabotage history 's|    if (!(hp_hz > 0.0f) \|\| !(k->hp_hz > 0.0f)) memset(k->hp_s, 0, sizeof k->hp_s);|    if (!(k->hp_hz > 0.0f) \&\& 0) memset(k->hp_s, 0, sizeof k->hp_s);|' 'D history'
echo "gate_keyfilter-perturb: the key filter oracle sees all four sabotages"
