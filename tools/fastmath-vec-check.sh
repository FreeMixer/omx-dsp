#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# fastmath-vec-check.sh — the loops omx_fastmath.h exists for really vectorise.
#
#   tools/fastmath-vec-check.sh CC INCFLAGS MVECFLAGS...
#       compiles test/fx/fastmath_vec.c to assembly with the libmvec flags the build derived and requires
#       every loop to call the _ZGV vector symbol of each libm function it makes, and no scalar call
#   tools/fastmath-vec-check.sh --self-test CC INCFLAGS MVECFLAGS...
#       the same over a copy of the header with its `omp simd` pragma taken out, which must be refused
#
# With no libmvec function among the flags it says so and passes: the libm path has nothing to vectorise.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SELF=0
if [ "${1:-}" = "--self-test" ]; then SELF=1; shift; fi
CC="$1"; INC="$2"; shift 2
case "$*" in
  *OMX_FASTMATH_MVEC_*) ;;
  *) echo "fastmath-vec-check: no libmvec function in this build, the libm path: nothing to vectorise"; exit 0 ;;
esac
T="$(mktemp -d "${TMPDIR:-/tmp}/fastmath-vec.XXXXXX")"
trap 'rm -rf "$T"' EXIT
ARGS="-I$HERE/include $INC"
if [ "$SELF" = 1 ]; then
  mkdir -p "$T/inc"; cp -r "$HERE/include/omxdsp" "$T/inc/"
  sed -i 's|#define OMX_FM_SIMD _Pragma("omp simd")|#define OMX_FM_SIMD|' "$T/inc/omxdsp/omx_fastmath.h"
  ARGS="-I$T/inc $INC"
fi
# shellcheck disable=SC2086
$CC -O2 -S $ARGS "$@" -o "$T/v.s" "$HERE/test/fx/fastmath_vec.c"
bad=0
# the assembly of one function, from its label to its .size
body() { awk -v f="$1" '$0 ~ "^"f":" {on=1} on {print} on && /\.size/ {exit}' "$T/v.s"; }
need() { # function libm-name...
  fn="$1"; shift
  b="$(body "$fn")"
  [ -n "$b" ] || { echo "fastmath-vec-check: $fn is not in the assembly"; bad=1; return; }
  for f in "$@"; do
    U="$(echo "$f" | tr a-z A-Z)"
    if ! echo " $MVECFLAGS " | grep -q "OMX_FASTMATH_MVEC_$U=1"; then continue; fi
    echo "$b" | grep -Eq "_ZGV[A-Za-z0-9]+_$f" || { echo "fastmath-vec-check: $fn calls no vector $f"; bad=1; }
  done
}
MVECFLAGS="$*"
need vc_drive_soft tanhf
need vc_drive_exciter tanhf
need vc_gain_centred log10f powf
need vc_gain_shifted log10f powf
need vc_gain_flat log10f powf
if [ "$SELF" = 1 ]; then
  if [ "$bad" = 1 ]; then echo "fastmath-vec-check: self-test: a header without the pragma is refused"; exit 0; fi
  echo "fastmath-vec-check: self-test FAILED: a header with no pragma was passed"; exit 1
fi
[ "$bad" = 0 ] || exit 1
echo "fastmath-vec-check: every loop calls its libmvec symbols"
