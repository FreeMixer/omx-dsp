#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# geq-true-sabotage.sh <dir> <archive> — the graphic EQ's `true` mode oracle (test/fx/geq_true.test.c)
# is not blind. It is built once per sabotage against a SABOTAGED COPY of omx_geq_instance.h placed
# ahead of the real include directory, and each build must fail the arm named beside it:
#   one-pass     the second pass is skipped: the first pass's gains are the design      (T)
#   no-solve     `true` hands the faders through unsolved                               (T, K)
#   no-ceiling   a gain past OMX_GEQ_DESIGN_GAIN_MAX_DB is not clamped                    (C)
#   silent       the ceiling clamps but the design does not say so                       (C)
#   no-proto     a band whose first-pass gain is 0 keeps a zero column, not the prototype (F)
#   no-apply     apply publishes nothing to the kernel                                   (S, A)
# The builds run in parallel. The tree is never edited; a sed that matches nothing is a failure, not
# a skip. The real oracle, run by `make test-fx`, is the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… geq-true-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
H=omxdsp/fx/omx_geq_instance.h
WRAP="-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free"

run() { # $1 = name, $2 = the arms that must fail ("T K"), $3 = sed expression
  local dir="$OUT/$1"
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$3" "$dir/$H"
  if cmp -s "$PKG/include/$H" "$dir/$H"; then
    echo "geq-true-sabotage: FAIL — the $1 sabotage matched nothing; the line it targets is gone"
    return 1
  fi
  $CC $CFLAGS -ffp-contract=off -I"$dir" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -pthread \
    -I"$PKG/test/fx" -o "$dir/oracle" "$PKG/test/fx/geq_true.test.c" "$LIBC" -lm $WRAP
  local log="$dir/oracle.log"
  if "$dir/oracle" > "$log"; then
    echo "geq-true-sabotage: FAIL — the oracle passed with the $1 sabotage"
    return 1
  fi
  local arm
  for arm in $2; do
    if ! grep -qE "^FAIL \[$arm " "$log"; then
      echo "geq-true-sabotage: FAIL — the $1 sabotage was not caught by arm $arm"
      return 1
    fi
  done
  echo "geq-true-sabotage: $1 — $(grep -c '^FAIL ' "$log") checks fail (arms $2 among them), as they must"
}

pids=()
run one-pass "T" 's|  if (!omx_geq_instance_solve_pass(first, target_db, sr, second)) return 0;|  memcpy(second, first, sizeof second);|' & pids+=($!)
run no-solve "T K" 's|    double g = second\[k\];|    double g = target_db[k];|' & pids+=($!)
run no-ceiling "C" 's|    if (fabs(g) > ceiling) {|    if (0) {|' & pids+=($!)
run silent "C" 's|      over = 1;|      over = 0;|' & pids+=($!)
run no-proto "F" 's|at_db\[k\] == 0.0 ? (double)OMX_GEQ_PROTO_DB : at_db\[k\]|at_db[k]|' & pids+=($!)
run no-apply "S A" 's|  if (d->ok) {|  if (0) {|' & pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
if [ "$failed" -ne 0 ]; then
  echo "geq-true-sabotage: FAIL"
  exit 1
fi
echo "geq-true-sabotage: all six sabotages are caught"
