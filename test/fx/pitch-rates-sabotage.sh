#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# pitch-rates-sabotage.sh <dir> <archive> — the pitch oracles at the nine oracle rates are not blind to
# the two things that make the pitch faces work at 32, 64 and 128 kHz (FreeMixer/omx-dsp#91). Each
# sabotage builds the named oracle against a SABOTAGED COPY of the headers placed ahead of the real
# include directory, and each build must go red on the arm and rate named beside it, never on a
# compiler error:
#   declared   the instance face's rate gate put back to the declared rates only: 32, 64 and
#              128 kHz are refused at init                       (pitch_instance, arm A, 32 kHz)
#   nyquist    the pre-filter's Nyquist term removed: at 32 kHz the corner lands on Nyquist, the
#              filter rings, and a tap's impulse cluster no longer sits at d_min + W·u
#                                                                (pitch_math, arm D, 32 kHz)
# The tree is never edited; a sed that matches nothing is a failure, not a skip. The real oracles,
# run by `make test-fx`, are the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… pitch-rates-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it

run() { # $1 = name, $2 = header, $3 = oracle, $4 = link flags, $5 = sed expression, $6… = FAIL lines (EREs)
  local name=$1 h=$2 oracle=$3 ldflags=$4 expr=$5 dir="$OUT/$1"
  shift 5
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$expr" "$dir/$h"
  if cmp -s "$PKG/include/$h" "$dir/$h"; then
    echo "pitch-rates-sabotage: FAIL — the $name sabotage matched nothing; the line it targets is gone"
    return 1
  fi
  # shellcheck disable=SC2086 # CFLAGS and ldflags are word lists
  $CC $CFLAGS -ffp-contract=off -I"$dir" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -pthread \
    -I"$PKG/test/fx" -o "$dir/oracle" "$PKG/test/fx/$oracle.test.c" "$LIBC" -lm $ldflags
  local log="$dir/oracle.log"
  if "$dir/oracle" > "$log"; then
    echo "pitch-rates-sabotage: FAIL — $oracle passed with the $name sabotage"
    return 1
  fi
  local want
  for want in "$@"; do
    if ! grep -qE "^FAIL $want" "$log"; then
      echo "pitch-rates-sabotage: FAIL — the $name sabotage was not caught as '$want'"
      grep '^FAIL ' "$log" | head -3
      return 1
    fi
  done
  echo "pitch-rates-sabotage: $name — $(grep -c '^FAIL ' "$log") checks of $oracle fail, as they must"
}

pids=()
run declared omxdsp/fx/omx_pitch_instance.h pitch_instance \
  '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free' \
  's|  if (!omx_pitch_instance_rate_ok(sr)) return 0;|  if (!omx_rate_is_declared(sr)) return 0;|' \
  '32000 Hz: A: the rate is accepted' '64000 Hz: A: the rate is accepted' \
  '128000 Hz: A: the rate is accepted' & pids+=($!)
run nyquist omxdsp/fx/omx_pitch.h pitch_math '' \
  's|                         OMX_PITCH_PREFILTER_NYQUIST_FRACTION_DOUBLE \* 0.5 \* (double)sr);|                         1.0 * 0.5 * (double)sr);|' \
  "D a tap's centroid is d_min \+ W·u \(samples\) @ 32000 Hz" & pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
if [ "$failed" -ne 0 ]; then
  echo "pitch-rates-sabotage: FAIL"
  exit 1
fi
echo "pitch-rates-sabotage: both sabotages are caught"
