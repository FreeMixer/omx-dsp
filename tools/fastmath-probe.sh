#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# fastmath-probe.sh — which of tanhf, expf, log10f and powf this toolchain can call through libmvec.
#
#   tools/fastmath-probe.sh [CC]     prints, space separated, the names found (e.g. "tanhf expf log10f powf"),
#                                    or nothing; exit 0 either way
#
# A function counts only when a loop over it, declared `#pragma omp declare simd notinbranch` exactly as
# omx_fastmath.h declares it, compiles to a call of its _ZGV vector symbol AND that program links against
# -lm -lmvec. A libmvec that lacks the symbol (glibc older than 2.35 on x86-64, 2.40 on aarch64 for tanhf
# and powf, none on Debian 12 arm64), a compiler that ignores the pragma, or no libmvec at all each give
# an empty answer for that function, and the build then keeps the libm. OMX_FASTMATH_PROBE_FORCE_NONE=1
# makes it answer nothing (the make test of the plain path); the Makefile reads FASTMATH=off the same way.
set -u
CC="${1:-${CC:-cc}}"
[ "${OMX_FASTMATH_PROBE_FORCE_NONE:-0}" = 1 ] && exit 0
T="$(mktemp -d "${TMPDIR:-/tmp}/fastmath-probe.XXXXXX")" || exit 0
trap 'rm -rf "$T"' EXIT
found=""
for f in tanhf expf log10f powf; do
  case "$f" in
    powf) proto="float powf(float, float)"; call="$f(x[i], 10.0f)" ;;
    *)    proto="float $f(float)";          call="$f(x[i])" ;;
  esac
  cat > "$T/p.c" <<C
#include <math.h>
#pragma omp declare simd notinbranch
extern $proto;
void omx_probe(float *x, int n) {
#pragma omp simd
  for (int i = 0; i < n; i++) x[i] = $call;
}
int main(int argc, char **argv) { float v[8] = {1, 2, 3, 4, 5, 6, 7, 8}; omx_probe(v, 8); return (int)v[argc & 7] == 99; }
C
  "$CC" -O2 -fopenmp-simd -fno-math-errno -S -o "$T/p.s" "$T/p.c" >/dev/null 2>&1 || continue
  grep -q "_ZGV[A-Za-z0-9]*_$f" "$T/p.s" || continue
  "$CC" -O2 -fopenmp-simd -fno-math-errno -o "$T/p" "$T/p.c" -lm -lmvec >/dev/null 2>&1 || continue
  found="$found $f"
done
echo $found
