#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# comp_release-perturb.sh — test/fx/comp_release.test.c sees each moved release profile and each defect of the release modes and the effective attack. Each sabotage is applied to a COPY
# of the include directory placed ahead of the real one, and the oracle built against it must go red
# in the named arm:
#   autofast  — PROGRAM_RELEASE_PROFILES.auto.fastMs moved from 100 to 130 ms: arm B;
#   autoslow  — PROGRAM_RELEASE_PROFILES.auto.slowMinMs moved from 300 to 400 ms: arm B;
#   automax   — PROGRAM_RELEASE_PROFILES.auto.slowMaxMs moved from 1200 to 1600 ms: arm B;
#   autoshare — PROGRAM_RELEASE_PROFILES.auto.share moved from 0.5 to 0.65: arm B;
#   optoshare — PROGRAM_RELEASE_PROFILES.opto.share moved from 0.5 to 0.65: arm B;
#   symmetric — the detector left on its own release in opto and auto: arms A, C;
#   rate4x    — the 4x control path's follower made at the base rate: arm B;
#   profile   — auto run on the opto profile: arms A, B;
#   fixed     — the kernel ignoring the release mode: arms A, B;
#   attack    — the effective attack's time constant read through log10: arm D.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… comp_release-perturb.sh <scratch-dir>
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
    echo "comp_release-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/comp_release.test.c" "$PKG"/src/*.c -lm
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "comp_release-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "comp_release-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "comp_release-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage autofast omxdsp_definitions.h 's|^#define OMX_PROGRAM_RELEASE_PROFILES_AUTO_FAST_MS 100$|#define OMX_PROGRAM_RELEASE_PROFILES_AUTO_FAST_MS 130|' B &
pids+=($!)
sabotage autoslow omxdsp_definitions.h 's|^#define OMX_PROGRAM_RELEASE_PROFILES_AUTO_SLOW_MIN_MS 300$|#define OMX_PROGRAM_RELEASE_PROFILES_AUTO_SLOW_MIN_MS 400|' B &
pids+=($!)
sabotage automax omxdsp_definitions.h 's|^#define OMX_PROGRAM_RELEASE_PROFILES_AUTO_SLOW_MAX_MS 1200$|#define OMX_PROGRAM_RELEASE_PROFILES_AUTO_SLOW_MAX_MS 1600|' B &
pids+=($!)
sabotage autoshare omxdsp_definitions.h 's|^#define OMX_PROGRAM_RELEASE_PROFILES_AUTO_SHARE 0.5f$|#define OMX_PROGRAM_RELEASE_PROFILES_AUTO_SHARE 0.65f|' B &
pids+=($!)
sabotage optoshare omxdsp_definitions.h 's|^#define OMX_PROGRAM_RELEASE_PROFILES_OPTO_SHARE 0.5f$|#define OMX_PROGRAM_RELEASE_PROFILES_OPTO_SHARE 0.65f|' B &
pids+=($!)
sabotage symmetric fx/omx_comp_instance.h 's|    p->release_coeff = p->attack_coeff;|    (void)0;|' A C &
pids+=($!)
sabotage rate4x fx/omx_comp_instance.h 's|s->sr, k ? 4u : 1u);|s->sr, 1u);|g' B &
pids+=($!)
sabotage profile fx/omx_comp_instance.h 's|      if (rel_mode == OMX_COMP_RELEASE_MODES_OPTO) omx_env_program_release_opto|      if (1) omx_env_program_release_opto|' A B &
pids+=($!)
sabotage fixed omx_dyn.h 's|  const int prog = p->gc.mode == OMX_DYN_ABOVE \&\& p->release_mode != 0;|  const int prog = 0;|' A B &
pids+=($!)
sabotage attack fx/omx_comp_instance.h 's|-1000.0f / (s->sr \* logf(pole))|-1000.0f / (s->sr * log10f(pole))|' D &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "comp_release-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "comp_release-perturb: the oracle sees all ten sabotages"
