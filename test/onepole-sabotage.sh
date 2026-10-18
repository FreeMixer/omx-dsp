#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# onepole-sabotage.sh <dir> — the onepole arm's omx_pole_rescale checks, proven able to SEE their own
# defects (FreeMixer/omx-dsp#133). Each sabotage is applied to a COPY of the include tree placed ahead
# of the real one, the suite is built against it and must go red on its onepole arm:
#   exponent  — the law inverted, p^(sr/ref): the time-constant and corner checks at every rate but the
#               reference;
#   zerorate  — the fixed point of a rate not above 0 dropped: powf(p, ref/0) is 0 and
#               powf(p, ref/NaN) is NaN, the fixed-point check;
#   refrate   — the fixed point of a reference rate not above 0 dropped: powf(p, NaN/sr) is NaN and
#               powf(p, -ref/sr) is above 1, the reference-rate check (FreeMixer/omx-dsp#148);
#   tone      — the delay's tone pole hands the knob on unrescaled: the fold check;
#   damp      — the reverb's damping pole spells the law with the exponent inverted: the fold check.
# The tree is never edited. A sed that matches nothing is a failure, not a skip, and a red that
# printed no FAIL line of the onepole arm (a compile error, a crash) is not a red arm.
# Usage: CC=… CFLAGS=… onepole-sabotage.sh <scratch-dir>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"
# the arm list into the scratch dir, never build/: a parallel make builds the suite from build/'s
bash "$PKG/tools/kernels-gen.sh" "$OUT" >/dev/null

sabotage() { # name header (relative to include/omxdsp) sed-expression
  local name=$1 hdr=$2 expr=$3 d="$OUT/$1" rc=0
  mkdir -p "$d"
  cp -r "$PKG/include/omxdsp" "$d/"
  sed -i "$expr" "$d/omxdsp/$hdr"
  if cmp -s "$PKG/include/omxdsp/$hdr" "$d/omxdsp/$hdr"; then
    echo "onepole-sabotage.sh: FAIL — sabotage '$name' changed nothing; the line it targets is gone"
    return 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a word list
  $CC $CFLAGS -ffp-contract=off -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS \
    -I"$OUT" -o "$d/suite" "$PKG/test/omxdsp_suite.c" "$PKG"/src/*.c -lm
  "$d/suite" > "$d/out" 2>&1 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "onepole-sabotage.sh: FAIL — sabotage '$name' left the suite GREEN; the arm cannot see the defect"
    return 1
  fi
  local n
  n=$(grep -c '^FAIL \[onepole[] ]' "$d/out" || true)
  if [ "$n" -eq 0 ]; then
    echo "onepole-sabotage.sh: FAIL — sabotage '$name' went red (rc=$rc) with no FAIL line of the onepole arm"
    tail -5 "$d/out"
    return 1
  fi
  echo "onepole-sabotage.sh: sabotage '$name': the onepole arm went red on $n checks"
}

sabotage exponent omx_onepole.h 's|? p : powf(p, ref / sr);|? p : powf(p, sr / ref);|'
sabotage zerorate omx_onepole.h 's# || !(sr > 0.0f) || sr == ref)# || sr == ref)#'
sabotage refrate omx_onepole.h 's#p >= 1.0f || !(ref > 0.0f) || #p >= 1.0f || #'
sabotage tone fx/omx_delay.h 's|  return omx_pole_rescale(p, OMX_FXDELAY_TONE_REFERENCE_RATE, sr);|  return (void)sr, p;|'
sabotage damp fx/omx_reverb.h 's|  return omx_pole_rescale(p, OMX_REVERB_DAMP_REFERENCE_RATE, sr);|  return omx_pole_rescale(p, sr, OMX_REVERB_DAMP_REFERENCE_RATE);|'
echo "onepole-sabotage.sh: the onepole arm sees all five sabotages"
