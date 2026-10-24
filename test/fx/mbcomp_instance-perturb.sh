#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# mbcomp_instance-perturb.sh — test/fx/mbcomp_instance.test.c sees each moved multiband declaration and
# each defect of the face. Each sabotage is applied to a COPY of the include directory placed ahead of
# the real one, and the oracle built against it must go red in the named arm:
#   minbands  — MBC_MIN_BANDS moved from 2 to 3 in omxdsp_definitions.h: arm F;
#   maxbands  — MBC_MAX_BANDS moved from 5 to 4: the build is refused, the come-up table in omx_mbcomp.h
#               having rows up to 5 bands;
#   xovers    — MBC_MAX_XOVERS moved from 4 to 5: arm F;
#   fraction  — MBC_CORNER_MAX_FRACTION moved from 0.45 to 0.4: arms C and F;
#   comeup    — MBC_CROSSOVER_DEFAULTS_3 moved from 200, 2000 to 250, 2000 Hz: arm F;
#   solo      — the muted bands still reach the sum: arm D;
#   mix       — the mix never reaches the bands' dry share: arm E;
#   rising    — corners are not made rising: arm F.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… mbcomp_instance-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"

sabotage() { # <name> <header under include/omxdsp> <sed expression> <arm>...
  local name="$1" hdr="$2" expr="$3" d="$OUT/$1"
  shift 3
  mkdir -p "$d"
  cp -r "$PKG/include/omxdsp" "$d/"
  sed -i "$expr" "$d/omxdsp/$hdr"
  if cmp -s "$PKG/include/omxdsp/$hdr" "$d/omxdsp/$hdr"; then
    echo "mbcomp_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  if [ "$1" = refused ]; then
    # shellcheck disable=SC2086 # CFLAGS is a flag list
    if $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
      -o "$d/oracle" "$PKG/test/fx/mbcomp_instance.test.c" "$PKG"/src/*.c -lm >"$d/build.log" 2>&1; then
      echo "mbcomp_instance-perturb: FAIL — sabotage '$name' built"; exit 1
    fi
    echo "mbcomp_instance-perturb: sabotage '$name' refused at build time"; return 0
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/mbcomp_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "mbcomp_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "mbcomp_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "mbcomp_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

D=omxdsp_definitions.h
sabotage minbands $D 's|^#define OMX_MBC_MIN_BANDS 2$|#define OMX_MBC_MIN_BANDS 3|' F
sabotage maxbands $D 's|^#define OMX_MBC_MAX_BANDS 5$|#define OMX_MBC_MAX_BANDS 4|' refused
sabotage xovers $D 's|^#define OMX_MBC_MAX_XOVERS 4$|#define OMX_MBC_MAX_XOVERS 5|' F
sabotage fraction $D 's|^#define OMX_MBC_CORNER_MAX_FRACTION_DOUBLE 0.45$|#define OMX_MBC_CORNER_MAX_FRACTION_DOUBLE 0.4|' C F
sabotage comeup $D 's|^#define OMX_MBC_CROSSOVER_DEFAULTS_3_INIT { 200, 2000 }$|#define OMX_MBC_CROSSOVER_DEFAULTS_3_INIT { 250, 2000 }|' F
sabotage solo fx/omx_mbcomp.h 's|        if (m->muted >> k \& 1u) continue;|        (void)0;|' D
sabotage mix fx/omx_mbcomp_instance.h 's|    d->dry = omx_dyn_dry_share(mix_share);|    d->dry = 0.0f * mix_share;|' E
sabotage rising fx/omx_mbcomp_instance.h 's|    hz\[k\] = k > 0u \&\& !(corner > hz\[k - 1u\]) ? nextafterf(hz\[k - 1u\], INFINITY) : corner;|    hz[k] = corner;|' F
echo "mbcomp_instance-perturb: the multiband face oracle sees all eight sabotages"
