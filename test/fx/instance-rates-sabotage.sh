#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# instance-rates-sabotage.sh <dir> <archive> — the instance oracles at the nine RME rates are not blind
# to the declared-only gate (FreeMixer/omx-dsp#179). Each sabotage builds one face's oracle against a
# SABOTAGED COPY of the headers placed ahead of the real include directory, with the face's init put
# back to omx_rate_is_declared(sr); the build must go red on the init arm at 32 kHz (the first of the
# three rates outside the declaration), never on a compiler error. The deesser has a second sabotage:
# its detector band's Nyquist cap removed, which must go red on the stable-band arm at 32 kHz. The tree
# is never edited; a sed that matches nothing is a failure, not a skip. The real oracles, run by
# `make test-fx`, are the control.
# Usage: CC=… CFLAGS=… CONTRACT_INC=… instance-rates-sabotage.sh <scratch-dir> <libomxdsp-contracts.a>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="${1:?scratch dir}"
LIBC="${2:?contracts archive}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
WRAP='-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free'

run() { # $1 = name, $2 = header, $3 = oracle, $4 = link flags, $5 = sed expression, $6… = FAIL lines (EREs)
  local name=$1 h=$2 oracle=$3 ldflags=$4 expr=$5 dir="$OUT/$1"
  shift 5
  rm -rf "$dir" && mkdir -p "$dir"
  cp -r "$PKG/include/omxdsp" "$dir/"
  sed -i "$expr" "$dir/$h"
  if cmp -s "$PKG/include/$h" "$dir/$h"; then
    echo "instance-rates-sabotage: FAIL — the $name sabotage matched nothing; the line it targets is gone"
    return 1
  fi
  # shellcheck disable=SC2086 # CFLAGS and ldflags are word lists
  $CC $CFLAGS -ffp-contract=off -I"$dir" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS -pthread \
    -I"$PKG/test/fx" -o "$dir/oracle" "$PKG/test/fx/$oracle.test.c" "$LIBC" -lm $ldflags
  local log="$dir/oracle.log"
  if "$dir/oracle" > "$log"; then
    echo "instance-rates-sabotage: FAIL — $oracle passed with the $name sabotage"
    return 1
  fi
  local want
  for want in "$@"; do
    if ! grep -qE "^FAIL $want" "$log"; then
      echo "instance-rates-sabotage: FAIL — the $name sabotage was not caught as '$want'"
      grep '^FAIL ' "$log" | head -3
      return 1
    fi
  done
  echo "instance-rates-sabotage: $name — $(grep -c '^FAIL ' "$log") checks of $oracle fail, as they must"
}

GATE='s|  if (!omx_rate_is_oracle_full(sr)) return 0;|  if (!omx_rate_is_declared(sr)) return 0;|'
pids=()
for k in drive rotary reverb chorus deesser delay flanger limiter; do
  ld=$WRAP; [ "$k" = rotary ] && ld=''
  run "$k-declared" "omxdsp/fx/omx_${k}_instance.h" "${k}_instance" "$ld" "$GATE" \
    '32000 Hz: A: the rate is accepted' '64000 Hz: A: the rate is accepted' \
    '128000 Hz: A: the rate is accepted' & pids+=($!)
done
run eq-declared omxdsp/fx/omx_eq_instance.h eq_instance "$WRAP" "$GATE" \
  '\[E instance face @ 32000 Hz\] init at each of the nine rates' \
  '\[E instance face @ 64000 Hz\] init at each of the nine rates' \
  '\[E instance face @ 128000 Hz\] init at each of the nine rates' & pids+=($!)
run deesser-nyquist omxdsp/fx/omx_deesser_instance.h deesser_instance "$WRAP" \
  's|OMX_DEESSER_INSTANCE_NYQUIST_FRACTION \* 0.5 \* (double)s->sr|1.0e9|' \
  '32000 Hz: C: no contract broken' '32000 Hz: P: the top of the travel at width' & pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
if [ "$failed" -ne 0 ]; then
  echo "instance-rates-sabotage: FAIL"
  exit 1
fi
echo "instance-rates-sabotage: the nine faces' declared-only gate and the deesser's cap are caught"
