#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# mbcomp_instance-perturb.sh — test/fx/mbcomp_instance.test.c sees each moved multiband declaration and
# each defect of the face. Each sabotage is applied to a COPY of the include directory placed ahead of
# the real one, and the oracle built against it must go red in the named arm:
#   minbands  — MBC_MIN_BANDS moved from 2 to 3 in omxdsp_definitions.h: arm F;
#   maxbands  — MBC_MAX_BANDS moved from 5 to 4: the build is refused by omx_mbcomp.h's assertion that
#               the come-up table has one row per band count up to 5, and the log must name it;
#   xovers    — MBC_MAX_XOVERS moved from 4 to 5: arm F;
#   fraction  — MBC_CORNER_MAX_FRACTION moved from 0.45 to 0.4: arms C and F;
#   comeup    — MBC_CROSSOVER_DEFAULTS_3 moved from 200, 2000 to 250, 2000 Hz: arm F;
#   solo      — the muted bands still reach the sum: arm D;
#   pastcount — a solo past the band count is not masked off: arm D;
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
# The sabotages are independent and run side by side; each is waited for and every one must be seen.
pids=()

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
    # the build must stop on the come-up table's own assertion, not on some other breakage of the copy
    grep -q "the come-up table below has one row per band count" "$d/build.log" \
      || { echo "mbcomp_instance-perturb: FAIL — sabotage '$name' failed to build, but not on the come-up table's assertion"; head -5 "$d/build.log"; exit 1; }
    echo "mbcomp_instance-perturb: sabotage '$name' refused at build time by the come-up table's assertion"; return 0
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
sabotage minbands $D 's|^#define OMX_MBC_MIN_BANDS 2$|#define OMX_MBC_MIN_BANDS 3|' F &
pids+=($!)
sabotage maxbands $D 's|^#define OMX_MBC_MAX_BANDS 5$|#define OMX_MBC_MAX_BANDS 4|' refused &
pids+=($!)
sabotage xovers $D 's|^#define OMX_MBC_MAX_XOVERS 4$|#define OMX_MBC_MAX_XOVERS 5|' F &
pids+=($!)
sabotage fraction $D 's|^#define OMX_MBC_CORNER_MAX_FRACTION_DOUBLE 0.45$|#define OMX_MBC_CORNER_MAX_FRACTION_DOUBLE 0.4|' C F &
pids+=($!)
sabotage comeup $D 's|^#define OMX_MBC_CROSSOVER_DEFAULTS_3_INIT { 200, 2000 }$|#define OMX_MBC_CROSSOVER_DEFAULTS_3_INIT { 250, 2000 }|' F &
pids+=($!)
sabotage solo fx/omx_mbcomp.h 's|        if (m->muted >> k \& 1u) continue;|        (void)0;|' D &
pids+=($!)
sabotage pastcount fx/omx_mbcomp_instance.h 's|^  soloed \&= (1u << nb) - 1u;$|  (void)0;|' D &
pids+=($!)
sabotage mix fx/omx_mbcomp_instance.h 's|    d->dry = omx_dyn_dry_share(mix_share);|    d->dry = 0.0f * mix_share;|' E &
pids+=($!)
sabotage rising fx/omx_mbcomp_instance.h 's|    hz\[k\] = k > 0u \&\& !(corner > hz\[k - 1u\]) ? nextafterf(hz\[k - 1u\], INFINITY) : corner;|    hz[k] = corner;|' F &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "mbcomp_instance-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "mbcomp_instance-perturb: the multiband face oracle sees all nine sabotages"
