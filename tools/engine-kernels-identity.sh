#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# engine-kernels-identity.sh — the sample-rate converter, the generators and the analyser's
# readings in this library give the same bits as the copies OpenMixer's engine still carries in
# its source directory (<engine-src>, the one holding mix_src.h and mix_dsp.h).
#
#   tools/engine-kernels-identity.sh <engine-src>             the check
#   tools/engine-kernels-identity.sh --write <engine-src>     print the
#                                                    digests the engine's copies give (how
#                                                    test/golden/engine_kernels.sha256 was written)
#   tools/engine-kernels-identity.sh --self-test <engine-src> each
#                                                    sabotage of an engine copy must go red
#
# test/engine_kernels/golden.test.c is compiled with -DOMX_GOLDEN_ENGINE against a copy of include/
# in which the three moved headers forward to the engine's copies:
#
#   omxdsp/omx_src.h -> mix_src.h            the converter
#   omxdsp/omx_gen.h -> mix_dsp.h            the generators
#   omxdsp/analysis/omx_rta.h -> mix_dsp.h   the spectrum readings and the half-band decimator
#
# and against the installed omx-contract inside this tree's range (tools/contract-include.sh), or the
# include directory ENGINE_IDENTITY_CONTRACT_INC names, whose header the engine's generated
# omx_console_limits.h completes. Nothing is downloaded. Every other primitive comes from this
# tree's include/ (ENGINE_IDENTITY_INCLUDE=<dir> takes them from another omx-dsp include directory,
# such as the one the engine builds against). The program must reach both engine headers, then match the
# committed digests at every rate.
set -euo pipefail
cd "$(dirname "$0")/.."
PKG="$PWD"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
INCLUDE="${ENGINE_IDENTITY_INCLUDE:-$PKG/include}"

# engine_contract: the include directory of the omx-contract the engine's copies compile against: the
# installed package inside this tree's range, or ENGINE_IDENTITY_CONTRACT_INC
engine_contract() {
  if [ -n "${ENGINE_IDENTITY_CONTRACT_INC:-}" ]; then printf '%s\n' "$ENGINE_IDENTITY_CONTRACT_INC"; return 0; fi
  sh "$PKG/tools/contract-include.sh" || { echo "engine-kernels-identity: FAIL — no installed omx-contract in range (or set ENGINE_IDENTITY_CONTRACT_INC)" >&2; return 1; }
}

# build <engine src dir> <scratch dir>: the golden program through the engine's copies
build() {
  local src out shim eci
  src="$(cd "$1" && pwd)"
  out="$2"
  for h in mix_src.h mix_dsp.h; do
    [ -f "$src/$h" ] || { echo "engine-kernels-identity: FAIL — $src has no $h"; return 1; }
  done
  eci="$(engine_contract "$src")" || return 1
  rm -rf "$out" && mkdir -p "$out"
  shim="$out/include"
  cp -r "$INCLUDE" "$shim"
  printf '#include "mix_src.h"\n' > "$shim/omxdsp/omx_src.h"
  printf '#include "mix_dsp.h"\n' > "$shim/omxdsp/omx_gen.h"
  printf '#include "mix_dsp.h"\n' > "$shim/omxdsp/analysis/omx_rta.h"
  # shellcheck disable=SC2086
  if ! $CC $CFLAGS -ffp-contract=off -DOMX_GOLDEN_ENGINE -H -I"$shim" -isystem "$eci" -I"$src" \
      -o "$out/golden" "$PKG/test/engine_kernels/golden.test.c" -lm 2>"$out/trace"; then
    echo "engine-kernels-identity: FAIL — the golden program does not compile against the engine's copies:"
    grep -E 'error' "$out/trace" | head -5
    return 1
  fi
  for h in mix_src.h mix_dsp.h; do
    grep -qE "^\.+ $src/$h$" "$out/trace" || { echo "engine-kernels-identity: FAIL — the program never reached $src/$h, so it compared nothing"; return 1; }
  done
}

run_check() { # $1 = engine src dir, $2 = scratch dir
  build "$1" "$2" || return 1
  if ! "$2/golden" "$PKG/test/golden/engine_kernels.sha256" >"$2/out" 2>&1; then
    echo "engine-kernels-identity: FAIL — through the engine's copies:"
    sed 's/^/  /' "$2/out"
    return 1
  fi
  echo "engine-kernels-identity: through the engine's copies in $(cd "$1" && pwd): $(cat "$2/out")"
}

if [ "${1:-}" = --write ]; then
  build "${2:?usage: engine-kernels-identity.sh --write <engine-src>}" "$PKG/build/engine-kernels-identity"
  "$PKG/build/engine-kernels-identity/golden" --write
  exit 0
fi

if [ "${1:-}" = --self-test ]; then
  src="$(cd "${2:?usage: engine-kernels-identity.sh --self-test <engine-src>}" && pwd)"
  scratch="$PKG/build/engine-kernels-identity-selftest"
  rm -rf "$scratch" && mkdir -p "$scratch"
  fail=0
  # name | file | sed expression: each one moves the arithmetic of one kernel's engine copy
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
    else
      echo "SABOTAGE $name: red — $(grep -m1 -E 'FAIL' "$scratch/$name.log")"
    fi
  done <<'EOF'
src|mix_src.h|s/  for (uint32_t k = 0; k < s->taps; k++) acc += c\[k\] \* h\[k\];/  for (uint32_t k = 0; k + 1u < s->taps; k++) acc += c[k] * h[k];/
gen|mix_dsp.h|s/  b\[6\] = w \* 0.115926f;/  b[6] = w * 0.115927f;/
rta|mix_dsp.h|s/    if (hi > last) hi = last;/    if (hi > last) hi = last - 1;/
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
  echo "engine-kernels-identity self-test: every sabotage red"
  exit 0
fi

run_check "${1:?usage: engine-kernels-identity.sh <engine-src>}" "$PKG/build/engine-kernels-identity"
