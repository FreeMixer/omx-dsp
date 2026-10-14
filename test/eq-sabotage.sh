#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# eq-sabotage.sh <dir> <archive> — the EQ bank's, head filters' and drawn curve's oracles are not
# blind (openmixer #1601). The suite is built once per sabotage against a SABOTAGED COPY of the
# headers, placed ahead of the real include directory, and each build must fail both the closed-form
# arm (eq-curve) and the arm against the TypeScript (eq-design-ts):
#   complement  the complement's b1 is b1 − a1 instead of a1 − b1;
#   identity    a cut of any depth reads as identity, so it is parked;
#   bank        a band that is off stays in the bank;
#   slope       a 24 dB/oct filter is designed as one 12 dB/oct section;
#   octaves     the width law's floor is 0.1 octave instead of 0.05;
#   nyquist     a band is evaluated at Nyquist itself instead of reading 0 dB there;
#   anchors     an anchor on the axis's edge is added to the grid;
#   hold        a curve point is not held under 0.999 Nyquist;
#   eq-off      an EQ that is off still draws its bands.
# The builds run in parallel. The tree is never edited; a sed that matches nothing is a failure, not
# a skip. The real suite, run by `make suite`, is the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… eq-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
bash "$PKG/tools/kernels-gen.sh" "$OUT/gen" >/dev/null
D=omxdsp/omx_eq_design.h
M=omxdsp/omx_magnitude.h

run() { # $1 = name, $2 = header, $3 = sed expression
  local dir="$OUT/$1"
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$3" "$dir/$2"
  if cmp -s "$PKG/include/$2" "$dir/$2"; then
    echo "eq-sabotage: FAIL — the $1 sabotage matched nothing; the line it targets is gone"
    return 1
  fi
  $CC $CFLAGS -ffp-contract=off -I"$dir" -Iinclude -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -I"$OUT/gen" \
    -o "$dir/suite" "$PKG/test/omxdsp_suite.c" "$LIBC" -lm
  local log="$dir/suite.log"
  if "$dir/suite" > "$log"; then
    echo "eq-sabotage: FAIL — the suite passed with the $1 sabotage"
    return 1
  fi
  local arms
  arms="$(grep -oE '^FAIL \[eq-(curve|design-ts)\]' "$log" | sort -u | tr '\n' ' ')"
  if [ "$arms" != "FAIL [eq-curve] FAIL [eq-design-ts] " ]; then
    echo "eq-sabotage: FAIL — the $1 sabotage was not caught by both EQ arms (caught by: ${arms:-none})"
    return 1
  fi
  echo "eq-sabotage: $1 — $(grep -cE '^FAIL \[eq-(curve|design-ts)\]' "$log") EQ checks fail, as they must"
}

pids=()
run complement "$D" 's|  out\[1\] = a1 - b1;|  out[1] = b1 - a1;|' & pids+=($!)
run identity "$D" 's|         gain_db == 0.0;|         gain_db <= 0.0;|' & pids+=($!)
run bank "$D" 's|    if (!b->on) continue;|    if (0) continue;|' & pids+=($!)
run slope "$D" 's|    const uint32_t ns = omx_eq_butterworth_qs(pf\[k\]->slope_24, qs);|    const uint32_t ns = omx_eq_butterworth_qs(0, qs);|' & pids+=($!)
run octaves "$D" 's|  const double bw = bw_oct > 0.05 ? bw_oct : 0.05;|  const double bw = bw_oct > 0.1 ? bw_oct : 0.1;|' & pids+=($!)
run nyquist "$M" 's/|| freq_hz >= sample_rate \/ 2.0) return 0.0;/|| freq_hz > sample_rate \/ 2.0) return 0.0;/' & pids+=($!)
run anchors "$M" 's|      if (a\[j\] > f_min \&\& a\[j\] < f_max) f_out\[n++\] = a\[j\];|      if (a[j] >= f_min \&\& a[j] <= f_max) f_out[n++] = a[j];|' & pids+=($!)
run hold "$M" 's|    db_out\[i\] = omx_eq_magnitude_sum_db(bands, nbands, f_out\[i\] < hi ? f_out\[i\] : hi,|    db_out[i] = omx_eq_magnitude_sum_db(bands, nbands, f_out[i] < hi ? f_out[i] : f_out[i],|' & pids+=($!)
run eq-off "$M" 's|^  if (eq_on)$|  if (eq_on \|\| 1)|' & pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
if [ "$failed" -ne 0 ]; then
  echo "eq-sabotage: FAIL"
  exit 1
fi
echo "eq-sabotage: all nine sabotages are caught"
