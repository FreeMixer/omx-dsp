#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# face-conformance.sh — every instance face's resolve() takes its kernel's contract controls: the
# same names, in the declared order, nothing extra and nothing missing.
#
#   tools/face-conformance.sh              the check, over include/omxdsp/fx/omx_<k>_instance.h
#   tools/face-conformance.sh --self-test  a renamed and a dropped argument, and a control renamed in
#                                          data/kernels/, must each be named
#
# Each kernel's controls are an ordered list in its definition file, data/kernels/<k>.json of this tree: the
# library defines every kernel it implements. A face's resolve is
#   omx_<k>_instance_resolve(Omx<K>Instance *s, int bypass, <one argument per control>)
# where each argument is named after its control in C case (`rateHz` -> `rate_hz`) and typed by its
# kind: a travel is a `float` in the control's user unit, a choice is an `int`, its index. A control
# that declares a `count` is taken once per band and is passed as `const <type> <name>[X]`, an array
# whose extent is a count the installed contract renders (`band[OMX_GEQ_BANDS]`) or a face macro defined
# as one; any other extent, or a scalar for a counted control, is named as a mismatch. A control with
# `of` (another control's travel under a choice) is no argument. A plugin binding generated from the contract then calls resolve without a
# hand-written map.
#
# PENDING names the faces written before the rule, whose arguments carry units or another shape
# (`depth_ms`, a ports struct, a band array). Each is reported, not failed; a PENDING face that
# conforms fails, so the list only shrinks.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PENDING=""
# NEWER names the controls a contract released after the floor (packaging/omx-dsp.spec) added to a kernel that the
# face does not take yet: "<kernel>:<control>" in C case. An installed contract inside the range may carry them
# (FreeMixer/omx-dsp#95); the face may omit them, and must take them in order when it does. Every other
# control still has to be taken, so the list only shrinks as faces catch up.
NEWER="gate:hp_hz gate:lp_hz drive:oversample
  delay:fx_delay_taps delay:tap0_gain delay:tap0_pan delay:tap1_gain delay:tap1_pan delay:tap2_gain delay:tap2_pan
  delay:tap3_gain delay:tap3_pan geq:mode"

# controls <kernel.json>: "name kind count" per control, in the declared order; `count` is the
# control's count (a control taken once per band) or "-". A control that carries `of` is another
# control's travel while a choice holds an id (eq's notchQ is q's while the band is a notch): it is
# no argument of its own and is skipped.
controls() {
  awk '
    /^  "controls": \[/ { inside = 1; next }
    inside && /^  \]/  { inside = 0 }
    inside && /^    \{/ { n = ""; k = ""; c = "-"; o = 0 }
    inside && /^      "name": "/ { n = $0; sub(/^      "name": "/, "", n); sub(/".*/, "", n) }
    inside && /^      "kind": "/ { k = $0; sub(/^      "kind": "/, "", k); sub(/".*/, "", k) }
    inside && /^      "count": "/ { c = $0; sub(/^      "count": "/, "", c); sub(/".*/, "", c) }
    inside && /^      "of": / { o = 1 }
    inside && /^    \}/ { if (n != "" && !o) print n, k, c }
  ' "$1"
}

# expected <kernel.json>: "type name" per control, the argument the face must take; a counted
# control is "type name[]", an array argument.
expected() {
  controls "$1" | awk '{
    c = ""; for (i = 1; i <= length($1); i++) { ch = substr($1, i, 1); c = c (ch ~ /[A-Z]/ ? "_" tolower(ch) : ch) }
    print ($2 == "choice" ? "int" : "float"), c ($3 == "-" ? "" : "[]")
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

# per_band <face.h>: an array argument `const T name[X]` read as `T name[]` when X is a count the
# render defines (the installed contract's header or this library's include/omxdsp/omxdsp_definitions.h), or a macro the face defines as one (`#define
# OMX_EQ_INSTANCE_BANDS OMX_EQ_BAND_COUNTS_EQ8_MAX`); every other line, and any other extent,
# unchanged, so it is named as a mismatch.
LIMITS="$(sh "$HERE/tools/contract-include.sh")/omxcontract/omx_contract_limits.h $HERE/include/omxdsp/omxdsp_definitions.h"
per_band() {
  local face="$1" line t n x to
  while IFS= read -r line; do
    if [[ "$line" =~ ^const\ (float|int)\ ([a-z_0-9]+)\[(OMX_[A-Z0-9_]+)\]$ ]]; then
      t="${BASH_REMATCH[1]}" n="${BASH_REMATCH[2]}" x="${BASH_REMATCH[3]}"
      to="$(sed -n "s/^#[ \t]*define $x \(OMX_[A-Z0-9_]*\)\$/\1/p" "$face" | head -1)"
      if grep -qh "^#define $x [0-9]" $LIMITS || { [ -n "$to" ] && grep -qh "^#define $to [0-9]" $LIMITS; }; then
        echo "$t $n[]"; continue
      fi
    fi
    echo "$line"
  done
}

# check <fx include dir> <kernel dir>: 0 when every face conforms (PENDING aside).
check() {
  local fx="$1" own="$2" bad=0 ok=0 k face got want n file
  for face in "$fx"/omx_*_instance.h; do
    k="$(basename "$face" _instance.h)"; k="${k#omx_}"
    file="$own/$k.json"
    if [ ! -f "$file" ]; then
      if [[ " $PENDING " == *" $k "* ]]; then echo "face-conformance: $k pending (no kernel file $k.json)"; continue; fi
      echo "face-conformance: $k: this tree has no kernel file data/kernels/$k.json" >&2; bad=1; continue
    fi
    want="$(expected "$file")"
    [ -n "$want" ] || { echo "face-conformance: $k: no controls read from $k.json" >&2; bad=1; continue; }
    got="$(arguments "$face" "$k" | per_band "$face")"
    for n in $NEWER; do
      [ "${n%%:*}" = "$k" ] || continue
      grep -q " ${n#*:}\(\[\]\)\?\$" <<<"$got" || want="$(grep -v " ${n#*:}\(\[\]\)\?\$" <<<"$want" || true)"
    done
    if [ "$got" = "$want" ]; then
      if [[ " $PENDING " == *" $k "* ]]; then
        echo "face-conformance: $k conforms but is listed PENDING: remove it from the list" >&2; bad=1
      else
        ok=$((ok + 1))
      fi
      continue
    fi
    if [[ " $PENDING " == *" $k "* ]]; then echo "face-conformance: $k pending (arguments predate the rule; see PENDING)"; continue; fi
    echo "face-conformance: omx_${k}_instance_resolve does not take $k's contract controls:" >&2
    diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed -n 's/^</  contract:/p; s/^>/  face:    /p' >&2 || true
    bad=1
  done
  [ "$ok" -gt 0 ] || { echo "face-conformance: no face conforms; the check would prove nothing" >&2; return 2; }
  [ "$bad" = 0 ] && echo "face-conformance: $ok faces take their kernel's contract controls, by name, order and kind"
  return "$bad"
}

OWN="$HERE/data/kernels"

if [ "${1:-}" = --self-test ]; then
  scratch="$(mktemp -d)"; trap 'rm -rf "$scratch"' EXIT
  cp "$HERE"/include/omxdsp/fx/omx_*_instance.h "$scratch/"
  check "$scratch" "$OWN" >/dev/null || { echo "face-conformance self-test: the unmodified faces do not pass" >&2; exit 1; }
  # A renamed argument: the contract's `depth` written as `depth_pct`.
  sed -i 's/^\( *\)float depth, float mix, int mode) {$/\1float depth_pct, float mix, int mode) {/' "$scratch/omx_tremolo_instance.h"
  out="$(check "$scratch" "$OWN" 2>&1)" && { echo "face-conformance self-test: a renamed tremolo argument passed" >&2; exit 1; }
  grep -q 'contract: *float depth$' <<<"$out" || { echo "face-conformance self-test: a renamed tremolo argument was NOT named" >&2; exit 1; }
  cp "$HERE/include/omxdsp/fx/omx_tremolo_instance.h" "$scratch/"
  # A dropped argument: the limiter without its release.
  sed -i 's/float lookahead_ms, float release_ms) {$/float lookahead_ms) {/' "$scratch/omx_limiter_instance.h"
  out="$(check "$scratch" "$OWN" 2>&1)" && { echo "face-conformance self-test: a dropped limiter argument passed" >&2; exit 1; }
  grep -q 'contract: *float release_ms$' <<<"$out" || { echo "face-conformance self-test: a dropped limiter argument was NOT named" >&2; exit 1; }
  cp "$HERE/include/omxdsp/fx/omx_limiter_instance.h" "$scratch/"
  # A per-band array whose extent is no contract count: the geq's bands as a literal 31.
  sed -i 's/const float band\[OMX_GEQ_BANDS\]/const float band[31]/' "$scratch/omx_geq_instance.h"
  out="$(check "$scratch" "$OWN" 2>&1)" && { echo "face-conformance self-test: a geq band array of literal extent passed" >&2; exit 1; }
  grep -q 'face: *const float band\[31\]$' <<<"$out" || { echo "face-conformance self-test: a geq band array of literal extent was NOT named" >&2; exit 1; }
  # This tree's kernel file is the one read: its tremolo `depth` renamed `depthPct` must be named against the face.
  mkdir "$scratch/own"; cp "$OWN"/*.json "$scratch/own/"
  sed -i 's/^      "name": "depth",$/      "name": "depthPct",/' "$scratch/own/tremolo.json"
  out="$(check "$HERE/include/omxdsp/fx" "$scratch/own" 2>&1)" && { echo "face-conformance self-test: a tremolo control renamed in this tree's kernel file passed" >&2; exit 1; }
  grep -q 'contract: *float depth_pct$' <<<"$out" || { echo "face-conformance self-test: a tremolo control renamed in this tree's kernel file was NOT named" >&2; exit 1; }
  echo "face-conformance self-test: a renamed and a dropped argument, a band array of no contract count and a control renamed in this tree's kernel file were each named"
  exit 0
fi

check "$HERE/include/omxdsp/fx" "$OWN"
