#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# face-conformance.sh — every instance face's resolve() takes its kernel's contract controls: the
# same names, in the declared order, nothing extra and nothing missing.
#
#   tools/face-conformance.sh              the check, over include/omxdsp/fx/omx_<k>_instance.h
#   tools/face-conformance.sh --self-test  a renamed and a dropped argument must each be named
#
# The pinned omx-contract declares each kernel's controls as an ordered list in
# data/kernels/<k>.json (tools/contract-include.sh --data). A face's resolve is
#   omx_<k>_instance_resolve(Omx<K>Instance *s, int bypass, <one argument per control>)
# where each argument is named after its control in C case (`rateHz` -> `rate_hz`) and typed by its
# kind: a travel is a `float` in the control's user unit, a choice is an `int`, its index. A control
# the kernel takes once per band is passed as `const float <name>[OMX_<COUNT>]`, an array whose
# extent is a count the pinned contract renders (`band[OMX_GEQ_BANDS]`); any other extent is named
# as a mismatch. A plugin binding generated from the contract then calls resolve without a
# hand-written map.
#
# PENDING names the faces written before the rule, whose arguments carry units or another shape
# (`depth_ms`, a ports struct, a band array). Each is reported, not failed; a PENDING face that
# conforms fails, so the list only shrinks.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PENDING="chorus dynamics eq flanger gate reverb"

# controls <kernel.json>: "name kind" per control, in the declared order.
controls() {
  awk '
    /^  "controls": \[/ { inside = 1; next }
    inside && /^  \]/  { inside = 0 }
    inside && /^      "name": "/ { n = $0; sub(/^      "name": "/, "", n); sub(/".*/, "", n) }
    inside && /^      "kind": "/ { k = $0; sub(/^      "kind": "/, "", k); sub(/".*/, "", k); print n, k }
  ' "$1"
}

# expected <kernel.json>: "type name" per control, the argument the face must take.
expected() {
  controls "$1" | awk '{
    c = ""; for (i = 1; i <= length($1); i++) { ch = substr($1, i, 1); c = c (ch ~ /[A-Z]/ ? "_" tolower(ch) : ch) }
    print ($2 == "choice" ? "int" : "float"), c
  }'
}

# arguments <face.h> <kernel>: "type name" per resolve argument after `s` and `bypass`; a first two
# that are not those are printed as found, so the comparison names them.
arguments() {
  awk -v fn="omx_$2_instance_resolve(" '
    index($0, fn) { on = 1 }
    on { proto = proto " " $0; if (index($0, ")")) exit }
    END {
      sub(/^[^(]*\(/, "", proto); sub(/\).*$/, "", proto)
      n = split(proto, a, ",")
      for (i = 1; i <= n; i++) {
        x = a[i]; gsub(/^[ \t]+|[ \t]+$/, "", x); gsub(/[ \t]+/, " ", x)
        if (i == 1 && x ~ /^Omx[A-Za-z]+Instance \*s$/) continue
        if (i == 2 && x == "int bypass") continue
        print x
      }
    }
  ' "$1"
}

# per_band: an array argument `const T name[OMX_X]` read as `T name` when OMX_X is a count the
# pinned contract's render defines; every other line, and any other extent, unchanged.
LIMITS="$(sh "$HERE/tools/contract-include.sh")/omxcontract/omx_contract_limits.h"
per_band() {
  local line t n x
  while IFS= read -r line; do
    if [[ "$line" =~ ^const\ (float|int)\ ([a-z_0-9]+)\[(OMX_[A-Z0-9_]+)\]$ ]]; then
      t="${BASH_REMATCH[1]}" n="${BASH_REMATCH[2]}" x="${BASH_REMATCH[3]}"
      if grep -q "^#define $x [0-9]" "$LIMITS"; then echo "$t $n"; continue; fi
    fi
    echo "$line"
  done
}

# check <fx include dir> <kernel data dir>: 0 when every face conforms (PENDING aside).
check() {
  local fx="$1" data="$2" bad=0 ok=0 k face got want
  for face in "$fx"/omx_*_instance.h; do
    k="$(basename "$face" _instance.h)"; k="${k#omx_}"
    if [ ! -f "$data/$k.json" ]; then
      if [[ " $PENDING " == *" $k "* ]]; then echo "face-conformance: $k pending (no kernel file $k.json)"; continue; fi
      echo "face-conformance: $k: the pinned contract has no data/kernels/$k.json" >&2; bad=1; continue
    fi
    want="$(expected "$data/$k.json")"
    [ -n "$want" ] || { echo "face-conformance: $k: no controls read from $k.json" >&2; bad=1; continue; }
    got="$(arguments "$face" "$k" | per_band)"
    if [ "$got" = "$want" ]; then
      if [[ " $PENDING " == *" $k "* ]]; then
        echo "face-conformance: $k conforms but is listed PENDING: remove it from the list" >&2; bad=1
      else
        ok=$((ok + 1))
      fi
      continue
    fi
    if [[ " $PENDING " == *" $k "* ]]; then echo "face-conformance: $k pending (arguments predate the rule)"; continue; fi
    echo "face-conformance: omx_${k}_instance_resolve does not take $k's contract controls:" >&2
    diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed -n 's/^</  contract:/p; s/^>/  face:    /p' >&2 || true
    bad=1
  done
  [ "$ok" -gt 0 ] || { echo "face-conformance: no face conforms; the check would prove nothing" >&2; return 2; }
  [ "$bad" = 0 ] && echo "face-conformance: $ok faces take their kernel's contract controls, by name, order and kind"
  return "$bad"
}

DATA="$(sh "$HERE/tools/contract-include.sh" --data)"

if [ "${1:-}" = --self-test ]; then
  scratch="$(mktemp -d)"; trap 'rm -rf "$scratch"' EXIT
  cp "$HERE"/include/omxdsp/fx/omx_*_instance.h "$scratch/"
  check "$scratch" "$DATA" >/dev/null || { echo "face-conformance self-test: the unmodified faces do not pass" >&2; exit 1; }
  # A renamed argument: the contract's `depth` written as `depth_pct`.
  sed -i 's/^\( *\)float depth, float mix, int mode) {$/\1float depth_pct, float mix, int mode) {/' "$scratch/omx_tremolo_instance.h"
  out="$(check "$scratch" "$DATA" 2>&1)" && { echo "face-conformance self-test: a renamed tremolo argument passed" >&2; exit 1; }
  grep -q 'contract: *float depth$' <<<"$out" || { echo "face-conformance self-test: a renamed tremolo argument was NOT named" >&2; exit 1; }
  cp "$HERE/include/omxdsp/fx/omx_tremolo_instance.h" "$scratch/"
  # A dropped argument: the limiter without its release.
  sed -i 's/float lookahead_ms, float release_ms) {$/float lookahead_ms) {/' "$scratch/omx_limiter_instance.h"
  out="$(check "$scratch" "$DATA" 2>&1)" && { echo "face-conformance self-test: a dropped limiter argument passed" >&2; exit 1; }
  grep -q 'contract: *float release_ms$' <<<"$out" || { echo "face-conformance self-test: a dropped limiter argument was NOT named" >&2; exit 1; }
  cp "$HERE/include/omxdsp/fx/omx_limiter_instance.h" "$scratch/"
  # A per-band array whose extent is no contract count: the geq's bands as a literal 31.
  sed -i 's/const float band\[OMX_GEQ_BANDS\]/const float band[31]/' "$scratch/omx_geq_instance.h"
  out="$(check "$scratch" "$DATA" 2>&1)" && { echo "face-conformance self-test: a geq band array of literal extent passed" >&2; exit 1; }
  grep -q 'face: *const float band\[31\]$' <<<"$out" || { echo "face-conformance self-test: a geq band array of literal extent was NOT named" >&2; exit 1; }
  echo "face-conformance self-test: a renamed and a dropped argument, and a band array of no contract count, were each named"
  exit 0
fi

check "$HERE/include/omxdsp/fx" "$DATA"
