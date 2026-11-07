#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# meter-view-sabotage.sh <dir> <libomxdsp-contracts.a> — the meter view arm (test/kernels/mix_meter_view.c) is not
# blind. The suite is built against a SABOTAGED COPY of the headers, placed ahead of the real include directory, once
# per sabotage, and each build must fail the mix_meter_view arm:
#   per-change  the marker falls only when the live fill changes, not by elapsed time (on silence it hangs);
#   gr-hold     the gain-reduction reading's hold removed (omx_meter.h);
#   fall        the declared fall one dB per second off, 19 for 18 (omxdsp_definitions.h);
#   hysteresis  the ladder's hysteresis removed;
#   clip        the clip word not sticky: it shows this tick's clip only;
#   floor       the declared floor changed, -54 for -60 dBFS (omxdsp_definitions.h);
#   decimate    a gain reduction's window keeps the peak, not the deepest.
# The tree is never edited; a sed that matches nothing is a failure, not a skip. The real suite, run by
# `make suite`, is the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… meter-view-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
bash "$PKG/tools/kernels-gen.sh" "$OUT/gen" >/dev/null

run() { # $1 = name, $2 = header under include/, $3 = sed expression
  local dir="$OUT/$1" h="$2"
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$3" "$dir/$h"
  if cmp -s "$PKG/include/$h" "$dir/$h"; then
    echo "meter-view-sabotage: FAIL — the $1 sabotage matched nothing; the line it targets is gone"
    exit 1
  fi
  $CC $CFLAGS -ffp-contract=off -I"$dir" -Iinclude -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -I"$OUT/gen" \
    -DOMX_METER_VIEW_VECTORS="\"$PKG/vectors/meter-view.json\"" -o "$dir/suite" "$PKG/test/omxdsp_suite.c" "$LIBC" -lm
  local log="$dir/suite.log"
  if "$dir/suite" > "$log"; then
    echo "meter-view-sabotage: FAIL — the suite passed with the $1 sabotage"
    exit 1
  fi
  if ! grep -qE '^FAIL \[mix_meter_view\]' "$log"; then
    echo "meter-view-sabotage: FAIL — the $1 sabotage was not caught by the mix_meter_view arm"
    exit 1
  fi
  echo "meter-view-sabotage: $1 — $(grep -cE '^FAIL \[mix_meter_view\]' "$log") meter view checks fail, as they must"
}

V=omxdsp/mix/omx_meter_view.h
D=omxdsp/omxdsp_definitions.h
run per-change "$V" 's|  v->elapsed_s += dt_s > 0.0 ? dt_s : 0.0;|  v->elapsed_s += (dt_s > 0.0 \&\& f != v->live) ? dt_s : 0.0;|'
run gr-hold omxdsp/mix/omx_meter.h 's|    m->hold_left_s = OMX_METER_GR_HOLD_MS \* 0.001f;|    m->hold_left_s = 0.0f;|'
run fall "$D" 's|^#define OMX_METER_VIEW_FALL_DB_PER_SECOND 18$|#define OMX_METER_VIEW_FALL_DB_PER_SECOND 19|'
run hysteresis "$V" 's|  if (prev >= 0 \&\& fabs(raw - (double)prev) <= 0.5 + hysteresis) return prev;|  if (prev >= 0 \&\& fabs(raw - (double)prev) <= 0.5 + 0.0 * hysteresis) return prev;|'
run clip "$V" 's|  v->clip = v->clip \|\| clip;|  v->clip = clip;|'
run floor "$D" 's|^#define OMX_METER_VIEW_FLOOR_DB -60$|#define OMX_METER_VIEW_FLOOR_DB -54|'
run decimate "$V" 's|  if (keep == OMX_METER_DECIMATION_DEEPEST) return value < kept ? value : kept;|  if (keep == OMX_METER_DECIMATION_DEEPEST) return value > kept ? value : kept;|'
echo "meter-view-sabotage: all seven sabotages are caught"
