#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# mix-engine-identity.sh — the mix module (include/omxdsp/mix/) gives the same bits as the copies
# OpenMixer's engine carries, or carried, in its native sources. Three sets of kernels, each with
# its own engine build of test/mix/golden.test.c, because the engine dropped each set's copies
# when it switched to this library for them (five sets):
#   first  (-DOMX_GOLDEN_ENGINE, the engine's mix_dsp.h, mix_lcr.h, mix_bus.h and mix_gr_meter.h)
#          the pan law, the LCR pan, the strip and fold coefficients, a bus's out stage, the block
#          and leg peaks, the clip run, the peak hold, the gain-reduction reading, one analyser
#          tap's reading, one source's contribution to a summing bus and the real-time load meter
#   rest   (-DOMX_GOLDEN_ENGINE_REST, the engine's mix_dsp.h and mix_sum.h) the factored strip
#          coefficient, the route gain stages (fill, fold, fold-fill), the strip's post-fader
#          reading, the settled-silence test and the LCR fold's centre level
#   strip  (-DOMX_GOLDEN_ENGINE_STRIP, the engine's mix_dsp.h) the bus stages of mix/omx_bus_stage.h:
#          one source summed into a stereo bus, a bus's out gain and its whole out stage, which keep
#          the engine's names, so the build must never reach the library's omx_bus_stage.h
#   key    (-DOMX_GOLDEN_ENGINE_KEY, the engine's mix_lane.h) a dynamics slot's key of
#          mix/omx_key.h: struct omx_key and omx_key_build
#   pb     (-DOMX_GOLDEN_ENGINE_PB, the engine's mix_pb.h) the source switch of mix/omx_src_blend.h:
#          omx_src_blend
#          These two keep the engine's names too. The rest of mix_lane.h needs the engine's plugin
#          host and mix_pb.h its recorder, so the build does not include those headers: it cuts the
#          engine's text out of them (the struct to its closing brace, each function from its
#          contract stage's #define to the #undef after it) into one header of the build's own,
#          which it includes in place of the library's, and must never reach the library's.
# A set is compared when the engine tree carries its copies; a tree that carries no set compares
# nothing and fails.
#
#   tools/mix-engine-identity.sh <engine-src>             the check
#   tools/mix-engine-identity.sh --write <engine-src>     print the
#                                                    digests the engine's copies give (how
#                                                    test/golden/mix.sha256 was written)
#   tools/mix-engine-identity.sh --self-test <engine-src> each
#                                                    sabotage of an engine copy must move its
#                                                    kernel's digests
#
# Each build calls the engine's headers instead of the library's for its own kernels, against the
# installed omx-contract inside this tree's range (tools/contract-include.sh), or the include
# directory ENGINE_IDENTITY_CONTRACT_INC names, whose header the engine's generated
# omx_console_limits.h completes. Nothing is downloaded. Every primitive those headers include comes from a copy of this
# tree's include/ (ENGINE_IDENTITY_INCLUDE=<dir> takes them from another omx-dsp include directory)
# in which omxdsp/omx_gen.h forwards to mix_dsp.h while the engine still carries its own generators,
# so the two copies of their names never meet. The first set's build must never reach the library's
# mix headers; the rest's (whose engine headers include the library's mix module for everything
# else) must never reach omxdsp/mix/omx_route_gain.h, and must find each engine copy it calls.
set -euo pipefail
cd "$(dirname "$0")/.."
PKG="$PWD"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
INCLUDE="${ENGINE_IDENTITY_INCLUDE:-$PKG/include}"

# set_headers <set>: the engine headers a set's build reads; set_define <set>: its build's define
set_headers() { case "$1" in first) echo "mix_dsp.h mix_lcr.h mix_bus.h mix_gr_meter.h" ;; rest) echo "mix_dsp.h mix_sum.h" ;; strip) echo "mix_dsp.h" ;; key) echo "mix_lane.h" ;; pb) echo "mix_pb.h" ;; esac; }
set_define() { case "$1" in first) echo "-DOMX_GOLDEN_ENGINE" ;; rest) echo "-DOMX_GOLDEN_ENGINE_REST" ;; strip) echo "-DOMX_GOLDEN_ENGINE_STRIP" ;; key) echo "-DOMX_GOLDEN_ENGINE_KEY" ;; pb) echo "-DOMX_GOLDEN_ENGINE_PB" ;; esac; }
SETS='first rest strip key pb'
# The rest's engine copies, one defining line each, in an engine whose mix_dsp.h takes the rest of
# the mix module from this library (an older engine's own copies of it collide with the library's
# in one build): file|extended regex
REST_COPIES='mix_dsp.h|^#include <omxdsp/mix/omx_mix_coef.h>
mix_sum.h|^static inline void omx_sum_main_split\(
mix_dsp.h|^static inline void omx_fill_gain\(
mix_dsp.h|^static inline void omx_fold_gain\(
mix_dsp.h|^static inline void omx_fold_fill\(
mix_dsp.h|^static inline float omx_strip_meter\(
mix_dsp.h|^static inline bool omx_strip_silent\(
mix_dsp.h|^#define OMX_CUE_FOLD_CLEV '
# The bus stages' engine copies, in an engine whose mix_dsp.h takes the rest of the mix module from
# this library: file|extended regex
STRIP_COPIES='mix_dsp.h|^#include <omxdsp/mix/omx_route_gain.h>
mix_dsp.h|^static inline void omx_mix_strip\(
mix_dsp.h|^static inline void omx_bus_out_gain\(
mix_dsp.h|^static inline void omx_bus_out_stage\('
# The key's and the source switch's engine copies: file|extended regex
KEY_COPIES='mix_lane.h|^struct omx_key \{
mix_lane.h|^#define OMX_CONTRACT_STAGE "lane/key-build"
mix_lane.h|^static inline const float \*omx_key_build\('
PB_COPIES='mix_pb.h|^#define OMX_CONTRACT_STAGE "pb/src-blend"
mix_pb.h|^static inline void omx_src_blend\('
# the header each cut-out set's build includes in place of the library's, and the library header it
# must never reach
cut_header() { case "$1" in key) echo "mix_key_engine.h" ;; pb) echo "mix_src_blend_engine.h" ;; esac; }
lib_header() { case "$1" in key) echo "omx_key.h" ;; pb) echo "omx_src_blend.h" ;; esac; }

# cut <set> <engine src dir>: the engine's text of the set's copies, verbatim, on stdout
cut() {
  case "$1" in
    key) awk '/^struct omx_key \{/ { on = 1 } on { print } on && /^\};/ { on = 0 }' "$2/mix_lane.h"
         awk '/^#define OMX_CONTRACT_STAGE "lane\/key-build"/ { on = 1 } on { print } on && /^#undef OMX_CONTRACT_STAGE/ { on = 0 }' "$2/mix_lane.h" ;;
    pb)  awk '/^#define OMX_CONTRACT_STAGE "pb\/src-blend"/ { on = 1 } on { print } on && /^#undef OMX_CONTRACT_STAGE/ { on = 0 }' "$2/mix_pb.h" ;;
  esac
}

# carries <set> <engine src dir>: whether the engine tree carries the set's copies
carries() {
  local h f re
  for h in $(set_headers "$1"); do [ -f "$2/$h" ] || return 1; done
  if [ "$1" = rest ]; then
    while IFS='|' read -r f re; do grep -qE "$re" "$2/$f" || return 1; done <<<"$REST_COPIES"
  fi
  if [ "$1" = strip ]; then
    while IFS='|' read -r f re; do grep -qE "$re" "$2/$f" || return 1; done <<<"$STRIP_COPIES"
  fi
  if [ "$1" = key ]; then
    while IFS='|' read -r f re; do grep -qE "$re" "$2/$f" || return 1; done <<<"$KEY_COPIES"
  fi
  if [ "$1" = pb ]; then
    while IFS='|' read -r f re; do grep -qE "$re" "$2/$f" || return 1; done <<<"$PB_COPIES"
  fi
  return 0
}

# engine_contract: the include directory of the omx-contract the engine's copies compile against: the
# installed package inside this tree's range, or ENGINE_IDENTITY_CONTRACT_INC
engine_contract() {
  if [ -n "${ENGINE_IDENTITY_CONTRACT_INC:-}" ]; then printf '%s\n' "$ENGINE_IDENTITY_CONTRACT_INC"; return 0; fi
  sh "$PKG/tools/contract-include.sh" || { echo "mix-engine-identity: FAIL — no installed omx-contract in range (or set ENGINE_IDENTITY_CONTRACT_INC)" >&2; return 1; }
}

# build <set> <engine src dir> <scratch dir>: the golden program through the engine's copies
build() {
  local set src out eci shim h
  set="$1"
  src="$(cd "$2" && pwd)"
  out="$3"
  for h in $(set_headers "$set"); do
    [ -f "$src/$h" ] || { echo "mix-engine-identity: FAIL — $src has no $h"; return 1; }
  done
  eci="$(engine_contract)" || return 1
  rm -rf "$out" && mkdir -p "$out"
  shim="$out/include"
  cp -r "$INCLUDE" "$shim"
  if [ -f "$src/mix_dsp.h" ] && grep -qE '^#define OMX_GEN_OSC ' "$src/mix_dsp.h"; then
    printf '#include "mix_dsp.h"\n' > "$shim/omxdsp/omx_gen.h"
  fi
  local engine_inc="$src"
  if [ "$set" = key ] || [ "$set" = pb ]; then
    # the engine's text, cut out of its header, behind the library primitives it calls
    engine_inc="$out/engine"
    mkdir -p "$engine_inc"
    {
      echo "/* GENERATED by tools/mix-engine-identity.sh: the $set set's copies, cut verbatim out of the engine's $(set_headers "$set"). */"
      echo "#include <stddef.h>"
      echo "#include <stdint.h>"
      echo "#include <string.h>"
      echo "#include <omxdsp/omx_biquad.h>"
      echo "#include <omxdsp/omx_contract.h>"
      echo "#include <omxdsp/omx_ramp.h>"
      echo "#include <omxdsp/mix/omx_rta_tap.h>"
      echo "#undef OMX_CONTRACT_STAGE"
      cut "$set" "$src"
    } > "$engine_inc/$(cut_header "$set")"
  fi
  # shellcheck disable=SC2086,SC2046
  if ! $CC $CFLAGS -ffp-contract=off $(set_define "$set") -H -I"$shim" -isystem "$eci" -I"$engine_inc" \
      -include "$PKG/test/support/log10f_subst.h" -o "$out/golden" "$PKG/test/mix/golden.test.c" -lm 2>"$out/trace"; then
    echo "mix-engine-identity: FAIL — the $set golden program does not compile against the engine's copies:"
    grep -E 'error' "$out/trace" | head -5
    return 1
  fi
  if [ "$set" = key ] || [ "$set" = pb ]; then
    grep -qE "^\.+ $engine_inc/$(cut_header "$set")$" "$out/trace" || { echo "mix-engine-identity: FAIL — the $set program never reached the copies cut out of $src/$(set_headers "$set"), so it compared nothing"; return 1; }
    if grep -qE "^\.+ .*/omxdsp/mix/$(lib_header "$set")$" "$out/trace"; then
      echo "mix-engine-identity: FAIL — the $set engine build reached the library's $(lib_header "$set"), so it compared the library with itself"
      return 1
    fi
    carries "$set" "$src" || { echo "mix-engine-identity: FAIL — $src lacks one of the $set engine copies"; return 1; }
  else
    for h in $(set_headers "$set"); do
      grep -qE "^\.+ $src/$h$" "$out/trace" || { echo "mix-engine-identity: FAIL — the $set program never reached $src/$h, so it compared nothing"; return 1; }
    done
  fi
  if [ "$set" = first ] && grep -qE "^\.+ .*/omxdsp/mix/" "$out/trace"; then
    echo "mix-engine-identity: FAIL — the first engine build reached the library's mix headers, so it compared the library with itself"
    return 1
  fi
  if [ "$set" = rest ]; then
    if grep -qE "^\.+ .*/omxdsp/mix/omx_route_gain\.h$" "$out/trace"; then
      echo "mix-engine-identity: FAIL — the rest's engine build reached the library's route gain stages, so it compared the library with itself"
      return 1
    fi
    carries rest "$src" || { echo "mix-engine-identity: FAIL — $src lacks one of the rest's engine copies"; return 1; }
  fi
  if [ "$set" = strip ]; then
    if grep -qE "^\.+ .*/omxdsp/mix/omx_bus_stage\.h$" "$out/trace"; then
      echo "mix-engine-identity: FAIL — the bus stages' engine build reached the library's omx_bus_stage.h, so it compared the library with itself"
      return 1
    fi
    carries strip "$src" || { echo "mix-engine-identity: FAIL — $src lacks one of the bus stages' engine copies"; return 1; }
  fi
}

# run_set <set> <engine src dir> <scratch dir>: one set's digests through the engine's copies
run_set() {
  build "$1" "$2" "$3" || return 1
  if ! "$3/golden" "$PKG/test/golden/mix.sha256" >"$3/out" 2>&1; then
    echo "mix-engine-identity: FAIL — $1 kernels, through the engine's copies:"
    sed 's/^/  /' "$3/out"
    return 1
  fi
  echo "mix-engine-identity: $1 kernels, through the engine's copies in $(cd "$2" && pwd): $(cat "$3/out")"
}

# run_check <engine src dir> <scratch dir>: every set the tree carries; carrying none is a failure
run_check() {
  local set ran=0
  for set in $SETS; do
    if carries "$set" "$1"; then
      run_set "$set" "$1" "$2/$set" || return 1
      ran=1
    else
      echo "mix-engine-identity: $1 does not carry the $set kernels' engine copies, not compared"
    fi
  done
  [ "$ran" = 1 ] || { echo "mix-engine-identity: FAIL — $1 carries no set's engine copies, so nothing was compared"; return 1; }
}

if [ "${1:-}" = --write ]; then
  src="${2:?usage: mix-engine-identity.sh --write <engine-src>}"
  for set in $SETS; do
    if carries "$set" "$src"; then
      build "$set" "$src" "$PKG/build/mix-engine-identity/$set"
      "$PKG/build/mix-engine-identity/$set/golden" --write
    fi
  done
  exit 0
fi

if [ "${1:-}" = --self-test ]; then
  src="$(cd "${2:?usage: mix-engine-identity.sh --self-test <engine-src>}" && pwd)"
  scratch="$PKG/build/mix-engine-identity-selftest"
  rm -rf "$scratch" && mkdir -p "$scratch"
  fail=0
  # set | kernel | file | sed expression: each one moves the arithmetic of one kernel's engine
  # copy, and that kernel's digests must move; a set's sabotages run when the tree carries the set
  ran=0
  while IFS='|' read -r set name file expr; do
    carries "$set" "$src" || continue
    ran=$((ran + 1))
    rm -rf "$scratch/x" && mkdir -p "$scratch/x/y"
    cp -r "$src" "$scratch/x/y/src"
    sed -i "$expr" "$scratch/x/y/src/$file"
    if cmp -s "$src/$file" "$scratch/x/y/src/$file"; then
      echo "SABOTAGE $name: the edit changed nothing in $file"; fail=1; continue
    fi
    if run_set "$set" "$scratch/x/y/src" "$scratch/out" >"$scratch/$name.log" 2>&1; then
      echo "SABOTAGE $name: STAYED GREEN"; fail=1
    elif ! grep -qE "FAIL: $name [0-9]+ Hz output moved" "$scratch/$name.log"; then
      echo "SABOTAGE $name: red, but not on the $name digests — $(grep -m1 -E 'FAIL' "$scratch/$name.log")"; fail=1
    else
      echo "SABOTAGE $name: red — $(grep -m1 -E "FAIL: $name " "$scratch/$name.log" | sed 's/^ *//')"
    fi
  done <<'EOF'
first|pan|mix_dsp.h|s/  \*gR = sinf(theta);/  *gR = sinf(theta) * 0.9999f;/
first|lcr|mix_lcr.h|s/  float phantom = sqrtf(phantom_sq);/  float phantom = sqrtf(phantom_sq) * 0.9999f;/
first|strip|mix_dsp.h|s/  return gain \* fader \* (mute ? 0.0f : 1.0f);/  return gain * fader * (mute ? 0.0f : 1.0001f);/
first|fold|mix_bus.h|s/    float t = tgt \* fold_coef;/    float t = tgt * fold_coef * 1.0001f;/
first|bus_out|mix_dsp.h|s/    busL\[i\] \*= omx_ramp_at(rl, i);/    busL[i] *= omx_ramp_at(rl, i) * 0.9999f;/
first|peak|mix_dsp.h|s/      if (y > peak) peak = y;/      if (y > peak) peak = x;/
first|clip|mix_dsp.h|s/      if (run_b >= need) return true;/      if (run_b > need) return true;/
first|hold|mix_dsp.h|s/  if (l > atomic_load_explicit(slot_l, memory_order_relaxed))/  if (l < atomic_load_explicit(slot_l, memory_order_relaxed))/
first|gr|mix_gr_meter.h|s/    m->hold_left_s = OMX_GR_HOLD_MS \* 0.001f;/    m->hold_left_s = OMX_GR_HOLD_MS * 0.0011f;/
first|rta|mix_dsp.h|s/    snap->block\[i\] = 0.5f \* (snap->block\[i\] + leg_scratch\[i\]);/    snap->block[i] = 0.5001f * (snap->block[i] + leg_scratch[i]);/
first|rta|mix_dsp.h|s/  omx_decimate_halfband(scr->stage2, OMX_RTA_LOW_WINDOW \/ 4, scr->low_block);/  omx_decimate_halfband(scr->stage1, OMX_RTA_LOW_WINDOW \/ 4, scr->low_block);/
first|bus_sum|mix_bus.h|s/    for (uint32_t i = 0; i < n; i++) busL\[i\] += (inL\[i\] + inR\[i\]) \* (c + step \* (float)i);/    for (uint32_t i = 0; i < n; i++) busL[i] += (inL[i] + inL[i]) * (c + step * (float)i);/
first|rt_load|mix_dsp.h|s/#define OMX_RT_LOAD_EMA_COEFF 0.9f/#define OMX_RT_LOAD_EMA_COEFF 0.8f/
rest|split|mix_sum.h|s/    omx_pan_stereo(pan, imgL, imgR);/    omx_pan_stereo(-pan, imgL, imgR);/
rest|route_fill|mix_dsp.h|s/  for (uint32_t i = 0; i < n; i++) out\[i\] = in\[i\] \* omx_ramp_at(r, i);/  for (uint32_t i = 0; i + 1 < n; i++) out[i] = in[i] * omx_ramp_at(r, i);/
rest|route_fold|mix_dsp.h|s/  for (uint32_t i = 0; i < n; i++) out\[i\] = (inL\[i\] + inR\[i\]) \* omx_ramp_at(r, i);/  for (uint32_t i = 0; i < n; i++) out[i] = (inL[i] - inR[i]) * omx_ramp_at(r, i);/
rest|fold_fill|mix_dsp.h|s/    cr += stepR;/    cr += stepR * 0.5f;/
rest|strip_meter|mix_dsp.h|s/  return omx_meter_block_peak(inL, inR, n) \* eff;/  return omx_meter_block_peak(inL, inR, n) * eff * 0.9999f;/
rest|settled|mix_dsp.h|s/  return tgtL == 0.0f \&\& tgtR == 0.0f \&\& curL == 0.0f \&\& curR == 0.0f;/  return tgtL == 0.0f \&\& tgtR == 0.0f \&\& curL == 0.0f \&\& curR < 1.0f;/
rest|lcr_clev|mix_dsp.h|s/#define OMX_CUE_FOLD_CLEV 0.70710678f/#define OMX_CUE_FOLD_CLEV 0.7071068f/
strip|mix_strip|mix_dsp.h|s/  \*curL = el.g_cur;/  *curL = tgtL;/
strip|bus_out_gain|mix_dsp.h|s/  omx_bus_out_apply(busL, busR, n, tgtL, tgtR, curL, curR);/  omx_bus_out_apply(busL, busR, n, tgtR, tgtL, curL, curR);/
strip|bus_out_stage|mix_dsp.h|s/  omx_meter_hold(peakL, peakR, mL, mR);/  omx_meter_hold(peakL, peakR, mR, mL);/
key|key|mix_lane.h|s/    omx_rta_tap_mono_sum(kb, l, r, n);/    omx_rta_tap_mono_sum(kb, l, l, n);/
key|key|mix_lane.h|s/    for (uint32_t i = 0; i < n; i++) kb\[i\] = omx_biquad(kb\[i\], k->hp_c, hp_state);/    for (uint32_t i = 0; i < n; i++) kb[i] = omx_biquad(kb[i], k->lp_c, hp_state);/
key|key|mix_lane.h|s/  return k->to_detector ? kb : NULL;/  return kb;/
pb|pb|mix_pb.h|s/    track\[i\] = l + a \* (track\[i\] - l);/    track[i] = l + a * (track[i] + l);/
pb|pb|mix_pb.h|s/      else memset(track, 0, (size_t)n \* sizeof(float));/      else memset(track, 0, (size_t)(n \/ 2u) * sizeof(float));/
EOF
  [ "$ran" -gt 0 ] || { echo "SABOTAGE: the tree carries no set, so no sabotage ran"; fail=1; }
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
