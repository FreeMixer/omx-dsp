#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# mix-engine-identity.sh — the mix module (include/omxdsp/mix/) gives the same bits as the copies
# OpenMixer's engine still carries in its native sources (FreeMixer/omx-dsp#98): the pan
# law, the LCR pan, the strip and fold coefficients, a bus's out stage, the block and leg peaks, the
# clip run, the peak hold, the gain-reduction reading, one analyser tap's reading, one source's
# contribution to a summing bus and the real-time load meter.
#
#   tools/mix-engine-identity.sh <engine-src>             the check
#   tools/mix-engine-identity.sh --write <engine-src>     print the
#                                                    digests the engine's copies give (how
#                                                    test/golden/mix.sha256 was written)
#   tools/mix-engine-identity.sh --self-test <engine-src> each
#                                                    sabotage of an engine copy must move its
#                                                    kernel's digests
#
# test/mix/golden.test.c is compiled with -DOMX_GOLDEN_ENGINE, which calls the engine's mix_dsp.h,
# mix_lcr.h, mix_bus.h and mix_gr_meter.h instead of the library's mix headers, against the
# installed omx-contract inside this tree's range (tools/contract-include.sh), or the include
# directory ENGINE_IDENTITY_CONTRACT_INC names, whose header the engine's generated
# omx_console_limits.h completes. Nothing is downloaded. Every primitive those headers include comes from a copy of this
# tree's include/ (ENGINE_IDENTITY_INCLUDE=<dir> takes them from another omx-dsp include directory)
# in which omxdsp/omx_gen.h forwards to mix_dsp.h while the engine still carries its own generators,
# so the two copies of their names never meet. The program must reach the four engine headers,
# then match the committed digests at every rate.
set -euo pipefail
cd "$(dirname "$0")/.."
PKG="$PWD"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
INCLUDE="${ENGINE_IDENTITY_INCLUDE:-$PKG/include}"
ENGINE_HEADERS="mix_dsp.h mix_lcr.h mix_bus.h mix_gr_meter.h"

# engine_contract: the include directory of the omx-contract the engine's copies compile against: the
# installed package inside this tree's range, or ENGINE_IDENTITY_CONTRACT_INC
engine_contract() {
  if [ -n "${ENGINE_IDENTITY_CONTRACT_INC:-}" ]; then printf '%s\n' "$ENGINE_IDENTITY_CONTRACT_INC"; return 0; fi
  sh "$PKG/tools/contract-include.sh" || { echo "mix-engine-identity: FAIL — no installed omx-contract in range (or set ENGINE_IDENTITY_CONTRACT_INC)" >&2; return 1; }
}

# build <engine src dir> <scratch dir>: the golden program through the engine's copies
build() {
  local src out eci shim
  src="$(cd "$1" && pwd)"
  out="$2"
  for h in $ENGINE_HEADERS; do
    [ -f "$src/$h" ] || { echo "mix-engine-identity: FAIL — $src has no $h"; return 1; }
  done
  eci="$(engine_contract)" || return 1
  rm -rf "$out" && mkdir -p "$out"
  shim="$out/include"
  cp -r "$INCLUDE" "$shim"
  if grep -qE '^#define OMX_GEN_OSC ' "$src/mix_dsp.h"; then
    printf '#include "mix_dsp.h"\n' > "$shim/omxdsp/omx_gen.h"
  fi
  # shellcheck disable=SC2086
  if ! $CC $CFLAGS -ffp-contract=off -DOMX_GOLDEN_ENGINE -H -I"$shim" -isystem "$eci" -I"$src" \
      -o "$out/golden" "$PKG/test/mix/golden.test.c" -lm 2>"$out/trace"; then
    echo "mix-engine-identity: FAIL — the golden program does not compile against the engine's copies:"
    grep -E 'error' "$out/trace" | head -5
    return 1
  fi
  for h in $ENGINE_HEADERS; do
    grep -qE "^\.+ $src/$h$" "$out/trace" || { echo "mix-engine-identity: FAIL — the program never reached $src/$h, so it compared nothing"; return 1; }
  done
  if grep -qE "^\.+ .*/omxdsp/mix/" "$out/trace"; then
    echo "mix-engine-identity: FAIL — the engine build reached the library's mix headers, so it compared the library with itself"
    return 1
  fi
}

run_check() { # $1 = engine src dir, $2 = scratch dir
  build "$1" "$2" || return 1
  if ! "$2/golden" "$PKG/test/golden/mix.sha256" >"$2/out" 2>&1; then
    echo "mix-engine-identity: FAIL — through the engine's copies:"
    sed 's/^/  /' "$2/out"
    return 1
  fi
  echo "mix-engine-identity: through the engine's copies in $(cd "$1" && pwd): $(cat "$2/out")"
}

if [ "${1:-}" = --write ]; then
  build "${2:?usage: mix-engine-identity.sh --write <engine-src>}" "$PKG/build/mix-engine-identity"
  "$PKG/build/mix-engine-identity/golden" --write
  exit 0
fi

if [ "${1:-}" = --self-test ]; then
  src="$(cd "${2:?usage: mix-engine-identity.sh --self-test <engine-src>}" && pwd)"
  scratch="$PKG/build/mix-engine-identity-selftest"
  rm -rf "$scratch" && mkdir -p "$scratch"
  fail=0
  # kernel | file | sed expression: each one moves the arithmetic of one kernel's engine copy, and
  # that kernel's digests must move
  while IFS='|' read -r name file expr; do
    rm -rf "$scratch/x" && mkdir -p "$scratch/x/y"
    cp -r "$src" "$scratch/x/y/src"
    sed -i "$expr" "$scratch/x/y/src/$file"
    if cmp -s "$src/$file" "$scratch/x/y/src/$file"; then
      echo "SABOTAGE $name: the edit changed nothing in $file"; fail=1; continue
    fi
    if run_check "$scratch/x/y/src" "$scratch/out" >"$scratch/$name.log" 2>&1; then
      echo "SABOTAGE $name: STAYED GREEN"; fail=1
    elif ! grep -qE "FAIL: $name [0-9]+ Hz output moved" "$scratch/$name.log"; then
      echo "SABOTAGE $name: red, but not on the $name digests — $(grep -m1 -E 'FAIL' "$scratch/$name.log")"; fail=1
    else
      echo "SABOTAGE $name: red — $(grep -m1 -E "FAIL: $name " "$scratch/$name.log" | sed 's/^ *//')"
    fi
  done <<'EOF'
pan|mix_dsp.h|s/  \*gR = sinf(theta);/  *gR = sinf(theta) * 0.9999f;/
lcr|mix_lcr.h|s/  float phantom = sqrtf(phantom_sq);/  float phantom = sqrtf(phantom_sq) * 0.9999f;/
strip|mix_dsp.h|s/  return gain \* fader \* (mute ? 0.0f : 1.0f);/  return gain * fader * (mute ? 0.0f : 1.0001f);/
fold|mix_bus.h|s/    float t = tgt \* fold_coef;/    float t = tgt * fold_coef * 1.0001f;/
bus_out|mix_dsp.h|s/    busL\[i\] \*= omx_ramp_at(rl, i);/    busL[i] *= omx_ramp_at(rl, i) * 0.9999f;/
peak|mix_dsp.h|s/      if (y > peak) peak = y;/      if (y > peak) peak = x;/
clip|mix_dsp.h|s/      if (run_b >= need) return true;/      if (run_b > need) return true;/
hold|mix_dsp.h|s/  if (l > atomic_load_explicit(slot_l, memory_order_relaxed))/  if (l < atomic_load_explicit(slot_l, memory_order_relaxed))/
gr|mix_gr_meter.h|s/    m->hold_left_s = OMX_GR_HOLD_MS \* 0.001f;/    m->hold_left_s = OMX_GR_HOLD_MS * 0.0011f;/
rta|mix_dsp.h|s/    snap->block\[i\] = 0.5f \* (snap->block\[i\] + leg_scratch\[i\]);/    snap->block[i] = 0.5001f * (snap->block[i] + leg_scratch[i]);/
rta|mix_dsp.h|s/  omx_decimate_halfband(scr->stage2, OMX_RTA_LOW_WINDOW \/ 4, scr->low_block);/  omx_decimate_halfband(scr->stage1, OMX_RTA_LOW_WINDOW \/ 4, scr->low_block);/
bus_sum|mix_bus.h|s/    for (uint32_t i = 0; i < n; i++) busL\[i\] += (inL\[i\] + inR\[i\]) \* (c + step \* (float)i);/    for (uint32_t i = 0; i < n; i++) busL[i] += (inL[i] + inL[i]) * (c + step * (float)i);/
rt_load|mix_dsp.h|s/#define OMX_RT_LOAD_EMA_COEFF 0.9f/#define OMX_RT_LOAD_EMA_COEFF 0.8f/
EOF
  # and a tree without the engine's headers compares nothing, so it must not pass either
  mkdir -p "$scratch/x/y/empty"
  if run_check "$scratch/x/y/empty" "$scratch/out" >"$scratch/empty.log" 2>&1; then
    echo "SABOTAGE no engine headers: STAYED GREEN"; fail=1
  else
    echo "SABOTAGE no engine headers: red — $(grep -m1 -E 'FAIL' "$scratch/empty.log")"
  fi
  rm -rf "$scratch"
  [ "$fail" = 0 ] || exit 1
  echo "mix-engine-identity self-test: every sabotage red"
  exit 0
fi

run_check "${1:?usage: mix-engine-identity.sh <engine-src>}" "$PKG/build/mix-engine-identity"
