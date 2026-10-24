#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# comp_key-perturb.sh — test/fx/comp_key.test.c sees each defect of the compressor's key path, hold and
# ducker, and the moved pivot. Each sabotage is applied to a COPY of the include directory placed ahead of
# the real one, and the oracle built against it must go red in the named arm:
#   pivot     — KEY_TILT_PIVOT_HZ moved from 1000 to 1200 Hz in omxdsp_definitions.h: arm C;
#   tilt      — the tilt's numerator term b1 with its sign turned: arms C and D;
#   source    — the sidechain never reaches the detector: arm B;
#   filter    — the key filter's edges never run: arm C;
#   fold      — mid folds as side: arm F;
#   linked    — linked folds as the mean of the legs, not the louder: arm A;
#   listen    — listen puts the unfiltered key on the output: arm D;
#   hold      — the hold never freezes the cascade: arm E;
#   floor     — the ducker's floor is never applied: arm G.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… comp_key-perturb.sh <scratch-dir>
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
    echo "comp_key-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/comp_key.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "comp_key-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "comp_key-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "comp_key-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage pivot omxdsp_definitions.h 's|^#define OMX_KEY_TILT_PIVOT_HZ 1000$|#define OMX_KEY_TILT_PIVOT_HZ 1200|' C &
pids+=($!)
sabotage tilt omx_tilt.h 's|  t->c\[1\] = (w - a) / n;|  t->c[1] = (a - w) / n;|' C D &
pids+=($!)
sabotage source fx/omx_comp_instance.h 's|  const int ext = s->src \&\& key_l;|  const int ext = 0;|' B &
pids+=($!)
sabotage filter fx/omx_comp_instance.h 's|^  omx_gate_key_run(\&s->filt\[0\], s->ka, m);$|  (void)0;|' C &
pids+=($!)
sabotage fold fx/omx_comp_instance.h 's|      case OMX_COMP_DETECT_LINKS_MID: s->kd\[i\] = mid; break;|      case OMX_COMP_DETECT_LINKS_MID: s->kd[i] = side; break;|' F &
pids+=($!)
sabotage linked fx/omx_comp_instance.h 's|      default: s->kd\[i\] = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b); break;|      default: s->kd[i] = 0.5f * (fabsf(a) + fabsf(b)); break;|' A &
pids+=($!)
sabotage listen fx/omx_comp_instance.h 's|        out_l\[o + i\] = !ms ? a :|        out_l[o + i] = !ms ? in_l[o + i] :|' D &
pids+=($!)
sabotage hold omx_dyn.h 's|    else if (st->hold_left > 0u) {|    else if (0) {|' E &
pids+=($!)
sabotage floor omx_dyn.h 's|    if (floored) omx_dyn_floor_gain(gain, m, floor_lin);|    (void)floored; (void)floor_lin;|' G &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "comp_key-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "comp_key-perturb: the key path oracle sees all nine sabotages"
