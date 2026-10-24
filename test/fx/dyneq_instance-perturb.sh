#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# dyneq_instance-perturb.sh — test/fx/dyneq_instance.test.c sees each defect of the dynamic EQ face
# and each moved declaration. Each sabotage is applied to a COPY of the include directory placed ahead
# of the real one, and the oracle built against it must go red in the named arm:
#   bands     — DYNEQ_BANDS moved from 6 to 5 in omxdsp_definitions.h: arm A;
#   knee      — BAND_DYN_KNEE_DB moved from 6 to 3 dB: the static law, arm B;
#   shelf     — a shelf's B designed as the bell's bandpass: the shelf closed forms, arm B;
#   order     — the dynamics run before the band's static section: arm B;
#   listen    — a listening band taps the signal before its static section: arm D;
#   mix       — the blend weights swapped: arm F.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… dyneq_instance-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"

sabotage() { # <name> <header under include/omxdsp> <sed expression> <arm>
  local name="$1" hdr="$2" expr="$3" arm="$4" d="$OUT/$1"
  mkdir -p "$d"
  cp -r "$PKG/include/omxdsp" "$d/"
  sed -i "$expr" "$d/omxdsp/$hdr"
  if cmp -s "$PKG/include/omxdsp/$hdr" "$d/omxdsp/$hdr"; then
    echo "dyneq_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/dyneq_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "dyneq_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local n
  n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
  [ "$n" -gt 0 ] || { echo "dyneq_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
  echo "dyneq_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
}

sabotage bands omxdsp_definitions.h 's|^#define OMX_DYNEQ_BANDS 6$|#define OMX_DYNEQ_BANDS 5|' A
sabotage knee omxdsp_definitions.h 's|^#define OMX_BAND_DYN_KNEE_DB 6$|#define OMX_BAND_DYN_KNEE_DB 3|' B
sabotage shelf omx_band_dyn.h 's|  if (kind != OMX_EQ_LOWSHELF \&\& kind != OMX_EQ_HIGHSHELF) {|  if (1) {|' B
sabotage order fx/omx_dyneq_instance.h 's|^    omx_band_dyn_process(l, r, m, \&s->band\[i\], \&s->st\[i\]);$|    (void)0;|; s|^    const uint32_t k = s->entry\[i\];$|    const uint32_t k = s->entry[i]; omx_band_dyn_process(l, r, m, \&s->band[i], \&s->st[i]);|' B
sabotage listen fx/omx_dyneq_instance.h 's|^    if (s->listen\[i\]) {$|    if (0) {|; s|^    const uint32_t k = s->entry\[i\];$|    const uint32_t k = s->entry[i]; if (s->listen[i]) for (uint32_t j = 0; j < m; j++) { s->sum_l[j] += omx_biquad(l[j], s->band[i].b_c, s->listen_l[i]); s->sum_r[j] += omx_biquad(r[j], s->band[i].b_c, s->listen_r[i]); }|' D
sabotage mix fx/omx_dyneq_instance.h 's|omx_wetdry_mix(\&l\[j\], \&r\[j\], s->dry_l\[j\], s->dry_r\[j\], l\[j\], r\[j\], dry, s->mix);|omx_wetdry_mix(\&l[j], \&r[j], l[j], r[j], s->dry_l[j], s->dry_r[j], dry, s->mix);|' F
echo "dyneq_instance-perturb: the dynamic EQ oracle sees all six sabotages"
