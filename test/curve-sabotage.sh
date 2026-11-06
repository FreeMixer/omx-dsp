#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# curve-sabotage.sh <dir> <libomxdsp-contracts.a> — the curve arm (test/kernels/curve.c) is not blind. The suite
# is built against a SABOTAGED COPY of omx_curve.h, placed ahead of the real include directory, once per sabotage,
# and each build must fail the curve arm:
#   khz       a kHz term read as 1024 Hz, not 1000;
#   band-on   a band's switch ignored, so a band switched off is drawn;
#   makeup    the make-up left out of a transfer curve;
#   knee      the gate's knee range dropped for the centred knee (omx_gaincomp_db);
#   range     a gate with no range term drawn with no floor, not refused;
#   anchors   an EQ's room counted without its anchors;
#   fbs-live-mark  the show pool's origin dropped from the marks;
#   hrp-mark  the HRP's origin dropped from the marks;
#   default-q a band with no Q drawn at 0.5, not the declared default;
#   limiter   a limiter's missing ratio taken as 4, not infinity;
#   slope-enum a bare choice value taken as a slope;
#   user-only the ring-out notches counted in the user-only curve.
# The tree is never edited; a sed that matches nothing is a failure, not a skip. The real suite, run by
# `make suite`, is the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… curve-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
bash "$PKG/tools/kernels-gen.sh" "$OUT/gen" >/dev/null
H=omxdsp/omx_curve.h

run() { # $1 = name, $2 = sed expression
  local dir="$OUT/$1"
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$2" "$dir/$H"
  if cmp -s "$PKG/include/$H" "$dir/$H"; then
    echo "curve-sabotage: FAIL — the $1 sabotage matched nothing; the line it targets is gone"
    exit 1
  fi
  $CC $CFLAGS -ffp-contract=off -I"$dir" -Iinclude -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -I"$OUT/gen" \
    -o "$dir/suite" "$PKG/test/omxdsp_suite.c" "$LIBC" -lm
  local log="$dir/suite.log"
  if "$dir/suite" > "$log"; then
    echo "curve-sabotage: FAIL — the suite passed with the $1 sabotage"
    exit 1
  fi
  if ! grep -qE '^FAIL \[curve\]' "$log"; then
    echo "curve-sabotage: FAIL — the $1 sabotage was not caught by the curve arm"
    exit 1
  fi
  echo "curve-sabotage: $1 — $(grep -cE '^FAIL \[curve\]' "$log") curve checks fail, as they must"
}

run khz 's|  case OMX_CURVE_UNITS_KHZ: return t->value \* 1000.0;|  case OMX_CURVE_UNITS_KHZ: return t->value * 1024.0;|'
run band-on 's|    bands\[used\].on = on ? omx_curve_term_value(on) != 0.0 : 1;|    bands[used].on = 1;|'
run makeup 's|    y\[i\] = lv + (double)g + mk;|    y[i] = lv + (double)g + 0.0 * mk;|'
run knee 's|    const float g = below ? omx_gaincomp_db_shifted(&d.gc, d.knee_shift_db, (float)lv) : omx_gaincomp_db(&d.gc, (float)lv);|    const float g = omx_gaincomp_db(\&d.gc, (float)lv);|'
run range 's/(below \&\& !range) || //; s|const float rg = (float)omx_curve_term_value(range);|const float rg = range ? (float)omx_curve_term_value(range) : 0.0f;|'
run anchors 's|  if (room < OMX_EQ_CURVE_CAP(rq->points, nd)) return|  if (room < rq->points) return|'
run fbs-live-mark 's|    if (!bands\[b\].on \|\| !ex->marks) continue;|    if (!bands[b].on \|\| !ex->marks \|\| origin[b] == OMX_CURVE_ORIGINS_FBS_LIVE) continue;|'
run hrp-mark 's|    if (!bands\[b\].on \|\| !ex->marks) continue;|    if (!bands[b].on \|\| !ex->marks \|\| origin[b] == OMX_CURVE_ORIGINS_HRP) continue;|'
run user-only 's|    if (origin\[b\] == OMX_CURVE_ORIGINS_USER) {|    if (origin[b] != OMX_CURVE_ORIGINS_FBS_LIVE) {|'
run default-q 's|                      : (double)omx_eq_lv2_clamp_q(kind == OMX_EQ_NOTCH ? OMX_EQ_LV2_NOTCH : OMX_EQ_LV2_BELL,|                      : 0.5 + 0.0 * omx_eq_lv2_clamp_q(kind == OMX_EQ_NOTCH ? OMX_EQ_LV2_NOTCH : OMX_EQ_LV2_BELL,|'
run limiter 's|  const float r = ratio ? (float)omx_curve_term_value(ratio) : INFINITY, k|  const float r = ratio ? (float)omx_curve_term_value(ratio) : 4.0f, k|'
run slope-enum 's|  if (!f \|\| !s \|\| s->unit != OMX_CURVE_UNITS_DB) return OMX_CURVE_TERM_MISSING;|  if (!f \|\| !s) return OMX_CURVE_TERM_MISSING;|'
echo "curve-sabotage: all twelve sabotages are caught"
