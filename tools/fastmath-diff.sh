#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# fastmath-diff.sh — what the libmvec build does to a kernel's audio, in dB.
#
#   tools/fastmath-diff.sh CC CFLAGS_PLAIN LIB LIMIT_DBFS MVECFLAGS...
#
# Every kernel that routes a per-sample tanhf, log10f or powf through omx_fastmath.h is rendered twice at every
# declared rate: once on the libm (the build the golden digests are made with, log10f correctly rounded), once
# with libmvec, from the same source. The output is not bit-identical; this prints, per kernel, the largest
# sample difference in dB relative to full scale, the reference's peak, and the difference relative to that
# peak, and fails when any kernel is above LIMIT_DBFS. With no libmvec function among MVECFLAGS there is
# nothing to compare and it says so.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CC="$1"; PLAIN="$2"; LIB="$3"; LIMIT="$4"; shift 4
case "$*" in *OMX_FASTMATH_MVEC_*) ;; *) echo "fastmath-diff: no libmvec function in this build: nothing to compare"; exit 0 ;; esac
T="$(mktemp -d "${TMPDIR:-/tmp}/fastmath-diff.XXXXXX")"
trap 'rm -rf "$T"' EXIT
MV="$*"
INC="-I$HERE/include -isystem ${CONTRACT_INC:-/usr/include} -I$HERE/test/fx"
# shellcheck disable=SC2086
$CC $PLAIN $INC -o "$T/cmp" "$HERE/test/fx/fastmath_cmp.c" -lm
bad=0
run() { # name source
  name="$1"; src="$2"
  # shellcheck disable=SC2086
  $CC $PLAIN $INC -include "$HERE/test/support/log10f_subst.h" -o "$T/old_$name" "$src" $LIB -lm 2>"$T/e" || { cat "$T/e"; bad=1; return; }
  # shellcheck disable=SC2086
  $CC $PLAIN $MV -DOMX_FX_GOLDEN_DUMP_ONLY=1 $INC -o "$T/new_$name" "$src" $LIB -lm 2>"$T/e" || { cat "$T/e"; bad=1; return; }
  (cd "$HERE" && "$T/old_$name" --dump "$T/old_$name.raw" && "$T/new_$name" --dump "$T/new_$name.raw") || { echo "fastmath-diff: $name did not render"; bad=1; return; }
  r="$("$T/cmp" "$T/old_$name.raw" "$T/new_$name.raw" "$LIMIT")" && ok=ok || ok=ABOVE-LIMIT
  printf 'fastmath-diff: %-22s max diff %s dBFS / reference peak / diff re peak (dB) [%s]\n' "$name" "$r" "$ok"
  [ "$ok" = ok ] || bad=1
}
for k in dynamics dynamics_keyed gate gate_knee gate_key gate_keyfilter comp_key mbcomp band_dyn dyn_controls; do
  [ -f "$HERE/test/fx/${k}_golden.test.c" ] || continue
  run "$k" "$HERE/test/fx/${k}_golden.test.c"
done
# the drive curves: the old side is the same source on the libm, with no substitution of log10f
# shellcheck disable=SC2086
$CC $PLAIN -DOMX_FX_GOLDEN_DUMP_ONLY=1 $INC -o "$T/old_drive" "$HERE/test/fx/fastmath_drive_render.c" $LIB -lm
# shellcheck disable=SC2086
$CC $PLAIN $MV $INC -o "$T/new_drive" "$HERE/test/fx/fastmath_drive_render.c" $LIB -lm
"$T/old_drive" --dump "$T/old_drive.raw"; "$T/new_drive" --dump "$T/new_drive.raw"
# four curves, two factors, per rate: report the worst curve of each
r="$("$T/cmp" "$T/old_drive.raw" "$T/new_drive.raw" "$LIMIT")" && ok=ok || ok=ABOVE-LIMIT
printf 'fastmath-diff: %-22s max diff %s dBFS / reference peak / diff re peak (dB) [%s]\n' "drive (4 curves, 1x 4x)" "$r" "$ok"
[ "$ok" = ok ] || bad=1
[ "$bad" = 0 ] || exit 1
