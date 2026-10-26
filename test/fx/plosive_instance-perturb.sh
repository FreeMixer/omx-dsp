#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# plosive_instance-perturb.sh — test/fx/plosive_instance.test.c sees each moved declaration of the plosive tamer and each defect of its laws. Each sabotage is applied to a COPY
# of the include directory placed ahead of the real one, and the oracle built against it must go red
# in the named arm:
#   sensitivity — the consonant sensitivity ignored: arm D;
#   level       — the consonant detector reading the band's level, not its contrast: arms D, F;
#   split       — the pop split's halves no longer complementary: arm A;
#   sign        — the consonant cut turned into a boost: arms B, G;
#   edge        — PLOSIVE_BAND_EDGE_FRACTION moved from 0.45 to 0.4: arm J;
#   consdefault — consonantDb's default moved from 6 to 7 dB: arm I;
#   popdefault  — popHz's default moved from 150 to 160 Hz: arm I;
#   poptop      — popRecoveryMs' top moved from 200 to 180 ms: arm I.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… plosive_instance-perturb.sh <scratch-dir>
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
    echo "plosive_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/plosive_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "plosive_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "plosive_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "plosive_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage sensitivity fx/omx_plosive_instance.h 's|OMX_TRANSIENT_FLOOR_LIN), s->cons_s);|OMX_TRANSIENT_FLOOR_LIN), 0.0f);|' D &
pids+=($!)
sabotage level fx/omx_plosive_instance.h 's|omx_plosive_gain(s->cons_db, omx_envdiff_step(\&s->cd.slow, \&s->fe, dc, fc, \&s->cp, OMX_TRANSIENT_FLOOR_LIN), s->cons_s)|omx_plosive_gain(s->cons_db, omx_lin_to_db(fc) + 45.0f, s->cons_s)|' D F &
pids+=($!)
sabotage split fx/omx_plosive_instance.h 's|  return 0.5f \* (x + omx_allpass1_tick(ap, x, a));|  return 0.6f * (x + omx_allpass1_tick(ap, x, a));|' A &
pids+=($!)
sabotage sign fx/omx_plosive_instance.h 's|return g != 1.0f ? x1 + (g - 1.0f) \* b : x1;|return g != 1.0f ? x1 + (1.0f - g) * b : x1;|' B G &
pids+=($!)
sabotage edge omxdsp_definitions.h 's|^#define OMX_PLOSIVE_BAND_EDGE_FRACTION_DOUBLE 0.45$|#define OMX_PLOSIVE_BAND_EDGE_FRACTION_DOUBLE 0.4|' J &
pids+=($!)
sabotage consdefault omxdsp_definitions.h 's|^#define OMX_PLOSIVE_CONSONANT_DB_RANGE_DEFAULT 6$|#define OMX_PLOSIVE_CONSONANT_DB_RANGE_DEFAULT 7|' I &
pids+=($!)
sabotage popdefault omxdsp_definitions.h 's|^#define OMX_PLOSIVE_POP_HZ_RANGE_DEFAULT 150$|#define OMX_PLOSIVE_POP_HZ_RANGE_DEFAULT 160|' I &
pids+=($!)
sabotage poptop omxdsp_definitions.h 's|^#define OMX_PLOSIVE_POP_RECOVERY_MS_RANGE_MAX 200$|#define OMX_PLOSIVE_POP_RECOVERY_MS_RANGE_MAX 180|' I &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "plosive_instance-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "plosive_instance-perturb: the oracle sees all eight sabotages"
