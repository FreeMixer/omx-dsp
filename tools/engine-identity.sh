#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# engine-identity.sh — the dynamics, balance, rotor and limiter kernels in this library give the
# same bits as the copies OpenMixer's engine still carries in packages/pipewire-native/src.
#
#   tools/engine-identity.sh <openmixer>/packages/pipewire-native/src             the check
#   tools/engine-identity.sh --self-test <openmixer>/packages/pipewire-native/src each sabotage of
#                                                                                  an engine copy
#                                                                                  must go red
#
# The golden-digest programs (test/fx/*_golden.test.c) are compiled a second time, against a copy
# of include/ in which four headers forward to the engine's copies instead of holding the code:
#
#   omxdsp/omx_dyn.h          -> mix_dsp.h     omx_dynamics, omx_dynamics_keyed and their atoms
#   omxdsp/omx_balance_law.h  -> mix_dsp.h     omx_clamp_pan, omx_balance_law
#   omxdsp/fx/omx_rotor.h     -> mix_rotor.h   the rotor word
#   omxdsp/fx/omx_limiter.h   -> mix_limiter.h the precision limiter
#
# (the umbrella omxdsp.h leaves the balance law out of the copy: mix_dsp.h includes primitives that
# include the umbrella, and the engine's header would be read half-way through one of them). Every
# program whose include trace reaches one of those engine headers is run against the committed
# test/golden/<kernel>.sha256, at every rate that file holds (nine for the four kernels): one digest
# that moves is a FAIL. The limiter, dynamics, dynamics_keyed, rotor and rotary programs must all
# reach the engine, or the check did not compare what it names. Every other primitive comes from
# this tree's include/ (ENGINE_IDENTITY_INCLUDE=<dir> takes them from another omx-dsp include
# directory instead, such as the one the engine is pinned to).
set -euo pipefail
cd "$(dirname "$0")/.."
PKG="$PWD"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
INCLUDE="${ENGINE_IDENTITY_INCLUDE:-$PKG/include}"
REQUIRED="limiter dynamics dynamics_keyed rotor rotary"

run_check() { # $1 = engine src dir, $2 = scratch dir
  local src out shim reached moved rates
  src="$(cd "$1" && pwd)"
  out="$2"
  for h in mix_dsp.h mix_rotor.h mix_limiter.h; do
    [ -f "$src/$h" ] || { echo "engine-identity: FAIL — $src has no $h"; return 1; }
  done
  rm -rf "$out" && mkdir -p "$out"
  shim="$out/include"
  cp -r "$INCLUDE" "$shim"
  printf '#include "mix_dsp.h"\n' > "$shim/omxdsp/omx_dyn.h"
  printf '#include "mix_dsp.h"\n' > "$shim/omxdsp/omx_balance_law.h"
  printf '#include "mix_rotor.h"\n' > "$shim/omxdsp/fx/omx_rotor.h"
  printf '#include "mix_limiter.h"\n' > "$shim/omxdsp/fx/omx_limiter.h"
  sed -i '/#include "omx_balance_law.h"/d' "$shim/omxdsp/omxdsp.h"
  make -s -C "$PKG" build/libomxdsp.a >/dev/null
  local fail=0 programs=0
  reached=""
  rates=0
  for t in "$PKG"/test/fx/*_golden.test.c; do
    k="$(basename "$t" _golden.test.c)"
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS -ffp-contract=off -H -I"$shim" -I"$src" -I"$PKG/test/fx" -o "$out/$k" "$t" \
        "$PKG/build/libomxdsp.a" -lm 2>"$out/$k.trace"; then
      echo "engine-identity: FAIL — $k does not compile against the engine's copies:"
      grep -E 'error' "$out/$k.trace" | head -5
      fail=1
      continue
    fi
    grep -qE "^\.+ $src/mix_(dsp|rotor|limiter)\.h$" "$out/$k.trace" || continue
    programs=$((programs + 1))
    reached="$reached $k"
    if "$out/$k" "$PKG/test/golden/$k.sha256" >"$out/$k.out" 2>&1; then
      rates=$((rates + $(sed -n 's/^fx\/[a-z_]*_golden: \([0-9]*\) rates.*/\1/p' "$out/$k.out")))
    else
      echo "engine-identity: FAIL — $k through the engine's copies:"
      sed 's/^/  /' "$out/$k.out"
      fail=1
    fi
  done
  for k in $REQUIRED; do
    case " $reached " in
      *" $k "*) ;;
      *) echo "engine-identity: FAIL — $k never reached the engine's copy, so it compared nothing"; fail=1 ;;
    esac
  done
  [ "$fail" = 0 ] || return 1
  echo "engine-identity: $programs programs through the engine's copies in $src, $rates rate digests, none moved:$reached"
}

if [ "${1:-}" = --self-test ]; then
  src="$(cd "${2:?usage: engine-identity.sh --self-test <openmixer>/packages/pipewire-native/src}" && pwd)"
  scratch="$PKG/build/engine-identity-selftest"
  rm -rf "$scratch" && mkdir -p "$scratch"
  fail=0
  # name | file | sed expression: each one moves the arithmetic of one kernel's engine copy
  while IFS='|' read -r name file expr; do
    rm -rf "$scratch/src" && cp -r "$src" "$scratch/src"
    sed -i "$expr" "$scratch/src/$file"
    if cmp -s "$src/$file" "$scratch/src/$file"; then
      echo "SABOTAGE $name: the edit changed nothing in $file"; fail=1; continue
    fi
    if run_check "$scratch/src" "$scratch/out" >"$scratch/$name.log" 2>&1; then
      echo "SABOTAGE $name: STAYED GREEN"; fail=1
    else
      echo "SABOTAGE $name: red — $(grep -m1 -E 'FAIL' "$scratch/$name.log")"
    fi
  done <<'EOF'
limiter|mix_limiter.h|s/const float c = omx_db_to_lin(p->ceiling_db);/const float c = 0.999f * omx_db_to_lin(p->ceiling_db);/
dynamics|mix_dsp.h|s/for (uint32_t i = 0; i < m; i++) gain\[i\] = omx_gaincomp_gain(&p->gc, lev\[i\]);/for (uint32_t i = 0; i < m; i++) gain[i] = 0.999f * omx_gaincomp_gain(\&p->gc, lev[i]);/
balance|mix_dsp.h|s/\*bL = p <= 0.0f ? 1.0f : 1.0f - p;/*bL = p <= 0.0f ? 1.0f : 0.999f - p;/
rotor|mix_rotor.h|s/#define OMX_ROTOR_AM_OFFSET 0.25f/#define OMX_ROTOR_AM_OFFSET 0.26f/
EOF
  # and a tree without the engine's headers compares nothing, so it must not pass either
  mkdir -p "$scratch/empty"
  if run_check "$scratch/empty" "$scratch/out" >"$scratch/empty.log" 2>&1; then
    echo "SABOTAGE no engine headers: STAYED GREEN"; fail=1
  else
    echo "SABOTAGE no engine headers: red — $(grep -m1 -E 'FAIL' "$scratch/empty.log")"
  fi
  rm -rf "$scratch"
  [ "$fail" = 0 ] || exit 1
  echo "engine-identity self-test: every sabotage red"
  exit 0
fi

run_check "${1:?usage: engine-identity.sh <openmixer>/packages/pipewire-native/src}" "$PKG/build/engine-identity"
