#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# pole-rescale-guard.sh — a pole quoted at a reference rate reaches the running rate through ONE word,
# omx_pole_rescale (include/omxdsp/omx_onepole.h, FreeMixer/omx-dsp#133). A `powf(<pole>, <ref> / sr)`
# anywhere else under the directories named on the command line is a second copy of that law; the
# delay's tone and the reverb's damping each carried one until the word existed. Part of `make lint`.
#
#   tools/pole-rescale-guard.sh <dir>...      the check (make lint passes include and src)
#   tools/pole-rescale-guard.sh --self-test   a restored copy must be named, the word itself must not
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
HOME_FILE=omx_onepole.h

scan() { # <dir>...: every `powf(…, … / sr)` outside the word's own header
  grep -rnE --include='*.h' --include='*.c' --include='*.inc' 'powf\([^;]*/ *sr *\)' "$@" 2>/dev/null |
    grep -v "/$HOME_FILE:" || true
}

if [ "${1:-}" = --self-test ]; then
  T=$(mktemp -d "${TMPDIR:-/tmp}/pole-rescale-guard.XXXXXX")
  trap 'rm -rf "$T"' EXIT
  mkdir -p "$T/omxdsp/fx"
  printf 'static inline float omx_pole_rescale(float p, float ref, float sr) { return powf(p, ref / sr); }\n' \
    > "$T/omxdsp/$HOME_FILE"
  printf 'static inline float f(float p, float sr) { return omx_pole_rescale(p, REF, sr); }\n' > "$T/omxdsp/fx/ok.h"
  fail=0
  if [ -n "$(scan "$T")" ]; then echo "pole-rescale-guard --self-test: FAIL — the word's own header was named"; fail=1; fi
  printf 'static inline float g(float p, float sr) {\n  return powf(p, OMX_X_REFERENCE_RATE / sr);\n}\n' \
    > "$T/omxdsp/fx/copy.h"
  if ! scan "$T" | grep -q 'fx/copy.h:2:'; then
    echo "pole-rescale-guard --self-test: FAIL — a restored copy of the law was not named"; fail=1
  fi
  [ "$fail" -eq 0 ] && echo "pole-rescale-guard --self-test: a restored copy is named, the word itself is not"
  exit "$fail"
fi

[ $# -gt 0 ] || { echo "usage: pole-rescale-guard.sh <dir>... | --self-test" >&2; exit 2; }
hits=$(scan "$@")
if [ -n "$hits" ]; then
  echo "pole-rescale-guard: a pole re-expressed at sr outside omx_pole_rescale; call the word instead:" >&2
  echo "$hits" >&2
  exit 1
fi
echo "pole-rescale-guard: every reference-rate pole under $* goes through omx_pole_rescale"
