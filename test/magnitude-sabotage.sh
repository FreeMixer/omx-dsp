#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# magnitude-sabotage.sh <dir> — the magnitude evaluator's oracles are not blind (FreeMixer/omx-dsp#69).
# The suite is built twice against a SABOTAGED COPY of the headers, placed ahead of the real include
# directory, and each build must fail its magnitude arms:
#   term   the numerator's imaginary part loses its z^-2 term, so the evaluator is no longer |H|;
#   floor  a full null answers 0 dB instead of the floor (the flat line a Q-116 notch once drew).
# The tree is never edited; a sed that matches nothing is a failure, not a skip. The real suite,
# run by `make suite`, is the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… magnitude-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
bash "$PKG/tools/kernels-gen.sh" "$OUT/gen" >/dev/null
H=omxdsp/omx_magnitude.h

run() { # $1 = name, $2 = sed expression
  local dir="$OUT/$1"
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$2" "$dir/$H"
  if cmp -s "$PKG/include/$H" "$dir/$H"; then
    echo "magnitude-sabotage: FAIL — the $1 sabotage matched nothing; the line it targets is gone"
    exit 1
  fi
  $CC $CFLAGS -ffp-contract=off -I"$dir" -Iinclude -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -I"$OUT/gen" \
    -o "$dir/suite" "$PKG/test/omxdsp_suite.c" "$LIBC" -lm
  local log="$dir/suite.log"
  if "$dir/suite" > "$log"; then
    echo "magnitude-sabotage: FAIL — the suite passed with the $1 sabotage"
    exit 1
  fi
  local arms
  arms="$(grep -oE '^FAIL \[magnitude(-ts)?\]' "$log" | sort -u | tr '\n' ' ')"
  if [ "$arms" != "FAIL [magnitude-ts] FAIL [magnitude] " ]; then
    echo "magnitude-sabotage: FAIL — the $1 sabotage was not caught by both magnitude arms (caught by: ${arms:-none})"
    exit 1
  fi
  echo "magnitude-sabotage: $1 — $(grep -cE '^FAIL \[magnitude' "$log") magnitude checks fail, as they must"
}

run term 's|  const double num_im = -(c\[1\] \* sw + c\[2\] \* sw2);|  const double num_im = -(c[1] * sw);|'
run floor 's|  return db < floor_db ? floor_db : db;|  return db < floor_db ? 0.0 : db;|'
echo "magnitude-sabotage: both sabotages are caught"
