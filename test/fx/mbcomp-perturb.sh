#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# mbcomp-perturb.sh — test/fx/mbcomp.test.c sees each defect of the multiband stage. Each sabotage
# is applied to a COPY of fx/omx_mbcomp.h placed ahead of the real include directory, and the oracle
# built against it must go red in the named arm:
#   allpass  — no allpass compensation: a band below a later corner skips that corner's allpass, so
#              the bands no longer share one phase and the sum at rest is not flat (O1, 3 bands up);
#   tap      — each band keeps its own oversampling choice instead of the stage's one factor, so
#              under 4x the bands take different taps (O8);
#   clamp    — the corner line is gone: a 20 kHz corner is designed at 20 kHz where 0.45·sr is
#              lower (44.1 kHz), or refused above Nyquist (32 kHz) (O7);
#   offband  — a band that is off runs its own atom instead of the unity compressor (O1);
#   alloc    — process allocates and frees a block on every call (RT).
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… mbcomp-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
H=omxdsp/fx/omx_mbcomp.h
rm -rf "$OUT" && mkdir -p "$OUT"

sabotage() { # name sed-expression arm...
  local name=$1 expr=$2 d="$OUT/$1" n
  shift 2
  mkdir -p "$d"
  cp -r "$PKG/include/omxdsp" "$d/"
  sed -i "$expr" "$d/$H"
  if cmp -s "$PKG/include/$H" "$d/$H"; then
    echo "mbcomp-perturb: FAIL — sabotage '$name' matched nothing in $H"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/mbcomp.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "mbcomp-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm\]" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "mbcomp-perturb: FAIL — sabotage '$name' did not turn the $arm arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "mbcomp-perturb: sabotage '$name' turned the $arm arm red ($n checks)"
  done
}

sabotage allpass 's|    for (uint32_t j = k + 1u; j < nx; j++) omx_xover_allpass(band\[k\], n, &m->xo\[j\], &leg->ap\[k\]\[j\]);|    (void)leg;|' O1
sabotage tap 's|  d.ovs_mode = factor == 4u ? OMX_DYN_OVS_X4 : OMX_DYN_OVS_OFF;|  (void)factor;|' O8
sabotage clamp 's|    const double f = (double)hz\[k\] < top ? (double)hz\[k\] : top;|    const double f = (double)hz[k]; (void)top;|' O7
sabotage offband 's|  if (!d.enabled) {|  if (0) {|' O1
sabotage alloc 's|  const uint32_t factor = omx_mbcomp_factor(m);|  { void *volatile p = malloc(16); free(p); } const uint32_t factor = omx_mbcomp_factor(m);|' RT
echo "mbcomp-perturb: the multiband oracle sees all five sabotages"
