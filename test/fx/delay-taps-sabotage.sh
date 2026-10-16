#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# delay-taps-sabotage.sh <dir> <archive> — the FX delay's read-tap oracle (test/fx/delay_taps.test.c)
# is not blind. It is built once per sabotage against a SABOTAGED COPY of omx_delay.h placed ahead of
# the real include directory, and each build must fail the arm named beside it:
#   wet-plus-zero  the wet sum starts at +0, so a -0 tap reads back +0                       (A)
#   tap-at-zero    every tap k >= 1 reads tap 0's delay                                       (B)
#   no-pan         a tap's per-leg gain ignores its pan                                       (B)
#   fb-tap-zero    the feedback regenerates from tap 0, not the longest tap                   (C)
#   no-refusal     the precondition on a tap past the ring is gone                            (D)
#   no-clamp       a tap past the ring wraps instead of reading cap-1                          (D)
# The builds run in parallel. The tree is never edited; a sed that matches nothing is a failure, not
# a skip. The real oracle, run by `make test-fx`, is the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… delay-taps-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
H=omxdsp/fx/omx_delay.h

run() { # $1 = name, $2 = the arms that must fail ("A B"), $3 = sed expression
  local dir="$OUT/$1"
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$3" "$dir/$H"
  if cmp -s "$PKG/include/$H" "$dir/$H"; then
    echo "delay-taps-sabotage: FAIL — the $1 sabotage matched nothing; the line it targets is gone"
    return 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a word list
  $CC $CFLAGS -ffp-contract=off -I"$dir" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -pthread \
    -I"$PKG/test/fx" -o "$dir/oracle" "$PKG/test/fx/delay_taps.test.c" "$LIBC" -lm
  local log="$dir/oracle.log"
  if "$dir/oracle" > "$log"; then
    echo "delay-taps-sabotage: FAIL — the oracle passed with the $1 sabotage"
    return 1
  fi
  local arm
  for arm in $2; do
    if ! grep -qE "^FAIL \[$arm\] " "$log"; then
      echo "delay-taps-sabotage: FAIL — the $1 sabotage was not caught by arm $arm"
      return 1
    fi
  done
  echo "delay-taps-sabotage: $1 — $(grep -c '^FAIL ' "$log") checks fail (arms $2 among them), as they must"
}

pids=()
run wet-plus-zero "A" 's|    float wetL = gl\[0\] \* tapL, wetR = gr\[0\] \* tapR;|    float wetL = 0.0f + gl[0] * tapL, wetR = 0.0f + gr[0] * tapR;|' & pids+=($!)
run tap-at-zero "B" 's|      const uint32_t rk = omx_lookahead_back(w, td\[k\], cap);|      const uint32_t rk = omx_lookahead_back(w, dl, cap);|' & pids+=($!)
run no-pan "B" 's|  \*gl = g \* bl;|  *gl = g;|' & pids+=($!)
run fb-tap-zero "C" 's|      if (k == kfl) srcL = tl;|      (void)kfl;|' & pids+=($!)
run no-refusal "D" 's|    OMX_PRE(p->tap_d\[k\] < s->cap, "taps-inside-the-ring");|    (void)0;|' & pids+=($!)
run no-clamp "D" 's|    td\[k\] = omx_fxdelay_clamp(p->tap_d\[k\], cap);|    td[k] = p->tap_d[k] % cap;|' & pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
if [ "$failed" -ne 0 ]; then
  echo "delay-taps-sabotage: FAIL"
  exit 1
fi
echo "delay-taps-sabotage: all six sabotages are caught"
