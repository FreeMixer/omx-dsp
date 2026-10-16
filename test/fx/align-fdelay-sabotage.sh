#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# align-fdelay-sabotage.sh <dir> <archive> — the strip alignment stage's oracle
# (test/fx/align_fdelay.test.c, the nine RME rates) is not blind. It is built once per sabotage
# against a SABOTAGED COPY of omx_align.h placed ahead of the real include directory, and each build
# must go red on the arm named beside it, never on a compiler error:
#   integer      the target rounded at every rate: 88.2 kHz and up settle on a whole sample   (A)
#   jump         the slew removed: a re-point lands in one sample, with a step beyond the
#                slewed tone's steepest                                                        (C)
#   rategate     the 88.2 kHz edge removed: 32 to 64 kHz settle fractional                     (A)
#   zipper       the MOVING read point rounded too: the tone's measured delay is no longer
#                the fractional target                                                         (B)
#   wholekernel  the exact-copy branch removed: a whole-sample delay goes through the kernel
#                at a fraction of 1, which the kernel's precondition refuses (the ledger)      (B)
# These are the five guards the console's own alignment sabotage broke while the stage lived in the
# console; they are broken here, where the stage's source is. The builds run in parallel. The tree is
# never edited; a sed that matches nothing is a failure, not a skip. The real oracle, run by
# `make test-fx`, is the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… align-fdelay-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
H=omxdsp/omx_align.h

run() { # $1 = name, $2 = the arm, $3 = sed expression, $4… = the FAIL lines the arm must print (EREs)
  local name=$1 arm=$2 expr=$3 dir="$OUT/$1"
  shift 3
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$expr" "$dir/$H"
  if cmp -s "$PKG/include/$H" "$dir/$H"; then
    echo "align-fdelay-sabotage: FAIL — the $name sabotage matched nothing; the line it targets is gone"
    return 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a word list
  $CC $CFLAGS -ffp-contract=off -I"$dir" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -pthread \
    -I"$PKG/test/fx" -o "$dir/oracle" "$PKG/test/fx/align_fdelay.test.c" "$LIBC" -lm
  local log="$dir/oracle.log"
  if "$dir/oracle" > "$log"; then
    echo "align-fdelay-sabotage: FAIL — the oracle passed with the $name sabotage"
    return 1
  fi
  local want
  for want in "$@"; do
    if ! grep -qE "^FAIL $want" "$log"; then
      echo "align-fdelay-sabotage: FAIL — the $name sabotage was not caught by arm $arm ('$want')"
      grep '^FAIL ' "$log" | head -3
      return 1
    fi
  done
  echo "align-fdelay-sabotage: $name — $(grep -c '^FAIL ' "$log") checks fail (arm $arm among them), as they must"
}

pids=()
run integer A \
  's|  if (d != floorf(d) \&\& (sr < OMX_ALIGN_FRACTION_MIN_RATE \|\| d < omx_fdelay_min_delay(OMX_ALIGN_ORDER)))|  if (d != floorf(d))|' \
  '[0-9]+ Hz: 0\.0703 ms settles at [0-9.]+ samples, unrounded' & pids+=($!)
run jump C 's|^#define OMX_ALIGN_SLEW (1.0f / 32.0f)|#define OMX_ALIGN_SLEW (1.0e9f)|' \
  'a 10 -> 40 move lands exactly' "[0-9]+ Hz: the move's worst step" & pids+=($!)
run rategate A 's|(sr < OMX_ALIGN_FRACTION_MIN_RATE \|\| d < omx_fdelay_min_delay|(d < omx_fdelay_min_delay|' \
  '[0-9]+ Hz: 0\.0703 ms settles at the nearest whole sample' & pids+=($!)
run zipper B \
  's|      (d < omx_fdelay_min_delay(OMX_ALIGN_ORDER) \&\& d != floorf(d)) ? floorf(d + 0.5f) : d;|      (d != floorf(d)) ? floorf(d + 0.5f) : d;|' \
  '[0-9]+ Hz: [0-9.]+ ms settles at [0-9.]+ samples; the tone is delayed' & pids+=($!)
run wholekernel B 's|  if (omx_fdelay_split(\&a->line_l, dd, id, \&kf)) return NULL;|  (void)omx_fdelay_split(\&a->line_l, dd, id, \&kf);|' \
  'arm B: [0-9]+ contract violations' & pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
if [ "$failed" -ne 0 ]; then
  echo "align-fdelay-sabotage: FAIL"
  exit 1
fi
echo "align-fdelay-sabotage: all five sabotages are caught"
