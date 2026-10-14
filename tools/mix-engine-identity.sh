#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# mix-engine-identity.sh — the mix module (include/omxdsp/mix/) gives the same bits as the copies
# OpenMixer's engine still carries in packages/pipewire-native/src (FreeMixer/omx-dsp#98): the pan
# law, the LCR pan, the strip and fold coefficients, a bus's out stage, the block and leg peaks and
# the clip run.
#
#   tools/mix-engine-identity.sh <openmixer>/packages/pipewire-native/src             the check
#   tools/mix-engine-identity.sh --write <openmixer>/packages/pipewire-native/src     print the
#                                                    digests the engine's copies give (how
#                                                    test/golden/mix.sha256 was written)
#   tools/mix-engine-identity.sh --self-test <openmixer>/packages/pipewire-native/src each
#                                                    sabotage of an engine copy must move its
#                                                    kernel's digests
#
# test/mix/golden.test.c is compiled with -DOMX_GOLDEN_ENGINE, which calls the engine's mix_dsp.h,
# mix_lcr.h and mix_bus.h instead of the library's mix headers, against the omx-contract release
# the engine pins (packages/core/package.json), whose header the engine's generated
# omx_console_limits.h completes. Every primitive those headers include comes from this tree's
# include/ (ENGINE_IDENTITY_INCLUDE=<dir> takes them from another omx-dsp include directory). The
# program must reach the three engine headers, then match the committed digests at every rate.
set -euo pipefail
cd "$(dirname "$0")/.."
PKG="$PWD"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
INCLUDE="${ENGINE_IDENTITY_INCLUDE:-$PKG/include}"
ENGINE_HEADERS="mix_dsp.h mix_lcr.h mix_bus.h"

# engine_contract <src>: the include directory of the omx-contract release the engine pins
engine_contract() {
  local core v dir
  core="$1/../../core/package.json"
  v="$(sed -n 's/.*"@openmixer\/omx-contract": "\([0-9][0-9.]*\)".*/\1/p' "$core" 2>/dev/null | head -1)"
  [ -n "$v" ] || { echo "mix-engine-identity: FAIL — no @openmixer/omx-contract pin in $core" >&2; return 1; }
  dir="$PKG/build/omx-contract/$v"
  if [ ! -f "$dir/package/include/omxcontract/omx_contract_limits.h" ]; then
    rm -rf "$dir"; mkdir -p "$dir"
    curl -fsSL "https://github.com/FreeMixer/omx-contract/releases/download/v$v/openmixer-omx-contract-$v.tgz" -o "$dir/release.tgz" \
      || { echo "mix-engine-identity: FAIL — omx-contract v$v could not be fetched" >&2; rm -rf "$dir"; return 1; }
    tar xzf "$dir/release.tgz" -C "$dir"
  fi
  printf '%s\n' "$dir/package/include"
}

# build <engine src dir> <scratch dir>: the golden program through the engine's copies
build() {
  local src out eci
  src="$(cd "$1" && pwd)"
  out="$2"
  for h in $ENGINE_HEADERS; do
    [ -f "$src/$h" ] || { echo "mix-engine-identity: FAIL — $src has no $h"; return 1; }
  done
  eci="$(engine_contract "$src")" || return 1
  rm -rf "$out" && mkdir -p "$out"
  # shellcheck disable=SC2086
  if ! $CC $CFLAGS -ffp-contract=off -DOMX_GOLDEN_ENGINE -H -I"$INCLUDE" -isystem "$eci" -I"$src" \
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
  build "${2:?usage: mix-engine-identity.sh --write <openmixer>/packages/pipewire-native/src}" "$PKG/build/mix-engine-identity"
  "$PKG/build/mix-engine-identity/golden" --write
  exit 0
fi

if [ "${1:-}" = --self-test ]; then
  src="$(cd "${2:?usage: mix-engine-identity.sh --self-test <openmixer>/packages/pipewire-native/src}" && pwd)"
  scratch="$PKG/build/mix-engine-identity-selftest"
  rm -rf "$scratch" && mkdir -p "$scratch"
  fail=0
  # kernel | file | sed expression: each one moves the arithmetic of one kernel's engine copy, and
  # that kernel's digests must move
  while IFS='|' read -r name file expr; do
    # the copy keeps the checkout's shape, so its ../../core/package.json names the same contract
    rm -rf "$scratch/x" && mkdir -p "$scratch/x/y" "$scratch/x/core"
    cp -r "$src" "$scratch/x/y/src"
    cp "$src/../../core/package.json" "$scratch/x/core/"
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

run_check "${1:?usage: mix-engine-identity.sh <openmixer>/packages/pipewire-native/src}" "$PKG/build/mix-engine-identity"
