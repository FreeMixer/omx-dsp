#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# gate_key-perturb.sh — test/fx/gate_key.test.c sees the moved pivot and each defect of the gate's key
# tilt and listen. Each sabotage is applied to a COPY of the include directory placed ahead of the real
# one, and the oracle built against it must go red in the named arm:
#   pivot     — KEY_TILT_PIVOT_HZ moved from 1000 to 1200 Hz: arm A;
#   slope     — the tilt's numerator term b1 with its sign turned: arms A and B;
#   detector  — the tilt is not on the key the detector hears: arm C;
#   listen    — the listen puts the gate's legs on the outputs, not the key: arm B;
#   leak      — the listen reaches the detector (the self key replaces the self detection): arm C.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… gate_key-perturb.sh <scratch-dir>
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
    echo "gate_key-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/gate_key.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "gate_key-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "gate_key-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "gate_key-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage pivot omxdsp_definitions.h 's|^#define OMX_KEY_TILT_PIVOT_HZ 1000$|#define OMX_KEY_TILT_PIVOT_HZ 1200|' A &
pids+=($!)
sabotage slope omx_tilt.h 's|  t->c\[1\] = (w - a) / n;|  t->c[1] = (a - w) / n;|' A B &
pids+=($!)
sabotage detector omx_gate.h 's|^  const int filtered = omx_gate_key_active(\&g->key) \|\| tilted;$|  const int filtered = omx_gate_key_active(\&g->key);|' C &
pids+=($!)
sabotage listen omx_gate.h 's|    memcpy(out_l + off, g->listen \&\& p->enabled ? g->sk : g->sl, m \* sizeof(float));|    memcpy(out_l + off, g->sl, m * sizeof(float));|' B &
pids+=($!)
sabotage leak omx_gate.h 's|    omx_dynamics_keyed(g->sl, g->sr, keyed \|\| filtered ? g->sk : NULL, m, p, \&g->st);|    omx_dynamics_keyed(g->sl, g->sr, keyed \|\| built ? g->sk : NULL, m, p, \&g->st);|' C &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "gate_key-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "gate_key-perturb: the gate key oracle sees all five sabotages"
