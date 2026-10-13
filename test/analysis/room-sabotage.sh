#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# room-sabotage.sh <dir> — the room analysis, sweep and alignment-fit oracles are not blind
# (FreeMixer/openmixer#1602). One break per kernel: a SABOTAGED COPY of its compiled unit is linked
# ahead of the archive, and every oracle named for it must fail, the closed-form oracle and the
# parity table against core's TypeScript (room_ts) alike. The tree is never edited, and a sed that
# matches nothing is a failure, not a skip. The real programs, run by make test-analysis, are the
# control.
#
#   xspec     omx_align_fit.c  the accumulator takes R·conj(T) for T·conj(R)
#   fit       omx_align_fit.c  the residual ramp is read with its sign flipped
#   transfer  omx_room.c       the estimate divides by the target's power (H2's denominator), not Pyy
#   impulse   omx_room.c       the reflection floor is read as a power, −30 dB as −60 dB
#   rt60      omx_room.c       the Schroeder integral sums |x| instead of x²
#   modes     omx_room.c       a peak's width is read at −6 dB instead of half power
#   mode-eq   omx_room.c       a cut is bounded at twice the notch depth law's depth
#   sweep     omx_sweep.c      the inverse filter loses its +6 dB/oct envelope
#   convolve  omx_sweep.c      the spectral product's imaginary part takes a minus
#
# CC, CFLAGS and CONTRACT_INC from the environment (make exports CONTRACT_INC).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:?CONTRACT_INC is not set}"
cd "$PKG"
rm -rf "$OUT" && mkdir -p "$OUT"
# The closed-form oracles count allocations through wrapped allocators (room_oracle.h).
WRAP=-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

# sabotage <name> <src file> <sed expression> <oracle>...
sabotage() {
  local name="$1" file="$2" expr="$3" dir="$OUT/$1"
  shift 3
  mkdir -p "$dir"
  local copy="$dir/$(basename "$file")"
  cp "$file" "$copy"
  sed -i "$expr" "$copy"
  if cmp -s "$file" "$copy"; then
    echo "room-sabotage.sh: FAIL — $name: the line the sabotage targets in $file is gone; it would test nothing"
    exit 1
  fi
  local caught=()
  for t in "$@"; do
    local wrap="$WRAP"
    [ "$t" = room_ts ] && wrap=   # the parity table counts no allocations
    $CC $CFLAGS -Iinclude -isystem "$CONTRACT_INC" -Isrc -Itest/analysis -Itest/fx $wrap \
      -o "$dir/$t" "test/analysis/$t.test.c" "$copy" build/libomxdsp.a -lm
    if "$dir/$t" > "$dir/$t.log" 2>&1; then
      echo "room-sabotage.sh: FAIL — $name: $t still passes against a sabotaged $(basename "$file")"
      tail -3 "$dir/$t.log"
      exit 1
    fi
    caught+=("$t ($(grep -c '^FAIL' "$dir/$t.log") failing checks reported)")
  done
  echo "room-sabotage.sh: $name: caught by ${caught[*]}"
}

sabotage xspec src/omx_align_fit.c \
  's|^    a->xi\[k\] += ti \* rr - tr \* ri;$|    a->xi[k] += tr * ri - ti * rr;|' align_fit room room_ts
sabotage fit src/omx_align_fit.c \
  's|^  const double residual_samples = (-step_rad \* n) / (2.0 \* M_PI);$|  const double residual_samples = (step_rad * n) / (2.0 * M_PI);|' \
  align_fit room_ts
sabotage transfer src/omx_room.c \
  's|^    out->re\[k\] = a->xr\[k\] / a->pyy\[k\];$|    out->re[k] = a->xr[k] / a->pxx[k];|; s|^    out->im\[k\] = a->xi\[k\] / a->pyy\[k\];$|    out->im[k] = a->xi[k] / a->pxx[k];|' \
  room room_ts
sabotage impulse src/omx_room.c \
  's|^  const double floor = peak \* pow(10.0, floor_db / 20.0);$|  const double floor = peak * pow(10.0, floor_db / 10.0);|' room room_ts
sabotage rt60 src/omx_room.c \
  's|^    running += scratch\[i\] \* scratch\[i\];$|    running += fabs(scratch[i]);|' room room_ts
sabotage modes src/omx_room.c \
  's|^  const double target = db\[idx\] - 3.0;$|  const double target = db[idx] - 6.0;|' room room_ts
sabotage mode-eq src/omx_room.c \
  's|^    const double gain = requested > -max_cut ? requested : -max_cut;$|    const double gain = requested > -2.0 * max_cut ? requested : -2.0 * max_cut;|' \
  room room_ts
sabotage sweep src/omx_sweep.c \
  's|^    inverse\[i\] = stimulus\[n - 1u - i\] \* exp(t_rev / L);$|    inverse[i] = stimulus[n - 1u - i] * exp(0.0 * t_rev / L);|' \
  sweep room_ts
sabotage convolve src/omx_sweep.c \
  's|^    const double im = ar\[k\] \* bi\[k\] + ai\[k\] \* br\[k\];$|    const double im = ar[k] * bi[k] - ai[k] * br[k];|' sweep room_ts
echo "room-sabotage.sh: every sabotage is caught by every oracle named for it"
