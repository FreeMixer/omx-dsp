#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# transient_readouts-perturb.sh — test/fx/transient_readouts.test.c sees each defect of the transient designer's readouts and its rates. Each sabotage is applied to a COPY
# of the include directory placed ahead of the real one, and the oracle built against it must go red
# in the named arm:
#   swap  — attackGainDb reading the decay share: arm B;
#   stale — sustainGainDb never written: arms B, C;
#   range — attackGainDb's declared floor moved from -24 to -20 dB: arm C;
#   rates — the face back on the six declared rates: arm B.
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… transient_readouts-perturb.sh <scratch-dir>
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
    echo "transient_readouts-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/transient_readouts.test.c" "$PKG"/src/*.c -lm
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "transient_readouts-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "transient_readouts-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "transient_readouts-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage swap fx/omx_transient.h 's|    st->attack_gain_db = ga;|    st->attack_gain_db = gs;|' B &
pids+=($!)
sabotage stale fx/omx_transient.h 's|    st->sustain_gain_db = gs;|    (void)gs;|' B C &
pids+=($!)
sabotage range omxdsp_definitions.h 's|^#define OMX_TRANSIENT_READOUT_ATTACK_GAIN_DB_MIN -24.0f$|#define OMX_TRANSIENT_READOUT_ATTACK_GAIN_DB_MIN -20.0f|' C &
pids+=($!)
sabotage rates fx/omx_transient_instance.h 's|  if (!omx_rate_is_oracle_full(sr)) return 0;|  if (!OMX_RATE_IS_DECLARED(sr)) return 0;|' B &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "transient_readouts-perturb: FAIL — a sabotage was not seen"; exit 1; }
echo "transient_readouts-perturb: the oracle sees all four sabotages"
