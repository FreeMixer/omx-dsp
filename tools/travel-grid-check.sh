#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# travel-grid-check.sh — every declared travel can be stepped from its minimum onto its maximum and its
# default, and every choice names its ids or uses a set another control of its kernel declares.
#
#   tools/travel-grid-check.sh [dir]        the check, over data/kernels of this tree (or dir)
#   tools/travel-grid-check.sh --self-test  a step off the grid, a default off the grid and a choice
#                                           with no ids must each be named
#
# A surface that steps a travel starts at `min`: a step that misses `max` or `default` leaves a value the
# control cannot reach. A value spelled as a reference to another declaration is read where it is declared
# and skipped here. A control with `of` is another control's travel and has no ids to name. A choice with a
# `global` and no ids is a use: it takes the ids of the control that declares that set (omx-contract's reader
# serves them under the use), so one control of the kernel has to hold them.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"

scan() {
  local f rc=0
  for f in "$1"/*.json; do
    awk -v file="$(basename "$f")" '
      function num(s) { sub(/^[^:]*: */, "", s); sub(/,? *$/, "", s); return s }
      function isnum(s) { return s ~ /^-?[0-9.]+([eE][-+]?[0-9]+)?$/ }
      function grid(what, v) {
        if (!isnum(v)) return
        d = (v - mn) / st; r = d - int(d + 0.5)
        if (r < -1e-6 || r > 1e-6) { printf "travel-grid: %s %s: %s %s is not min %s + k x step %s\n", file, name, what, v, mn, st; bad = 1 }
      }
      /^  "controls": \[/ { inside = 1; next }
      inside && /^  \]/ { inside = 0 }
      inside && /^    \{/ { name = ""; kind = ""; gl = ""; ids = 0; of = 0; mn = st = mx = df = "" }
      inside && /^      "name": "/ { name = $0; sub(/^ *"name": "/, "", name); sub(/".*/, "", name) }
      inside && /^      "kind": "/ { kind = $0; sub(/^ *"kind": "/, "", kind); sub(/".*/, "", kind) }
      inside && /^      "ids": / { ids = 1 }
      inside && /^      "of": / { of = 1 }
      inside && /^      "global": "/ { gl = $0; sub(/^ *"global": "/, "", gl); sub(/".*/, "", gl) }
      inside && /^        "min": / { mn = num($0) }
      inside && /^        "max": / { mx = num($0) }
      inside && /^        "step": / { st = num($0) }
      inside && /^        "default": / { df = num($0) }
      inside && /^    \}/ {
        if (kind == "choice" && ids && gl != "") held[gl] = 1
        if (kind == "choice" && !ids && !of) {
          if (gl == "") { printf "travel-grid: %s %s: a choice with no ids\n", file, name; bad = 1 }
          else { used[gl] = name }
        }
        if (isnum(mn) && isnum(st) && st + 0 > 0) { n++; grid("max", mx); grid("default", df) }
      }
      END {
        for (g in used) if (!(g in held)) { printf "travel-grid: %s %s: uses %s, which no control of this kernel holds\n", file, used[g], g; bad = 1 }
        printf "%d\n", n > "/dev/stderr"; exit bad
      }
    ' "$f" 2>>"$2" || rc=1
  done
  return "$rc"
}

check() {
  local counts total
  counts=$(mktemp); trap 'rm -f "$counts"' RETURN
  local rc=0
  scan "$1" "$counts" || rc=1
  total=$(awk '{ s += $1 } END { print s + 0 }' "$counts")
  [ "$total" -gt 0 ] || { echo "travel-grid: no travel read; the check would prove nothing" >&2; return 2; }
  [ "$rc" = 0 ] && echo "travel-grid: $total numeric travels reach their maximum and default from their minimum; every choice names its ids"
  return "$rc"
}

if [ "${1:-}" = --self-test ]; then
  scratch=$(mktemp -d); trap 'rm -rf "$scratch"' EXIT
  red() { # red <label> <file> <sed> <expected text>
    rm -rf "$scratch/k"; cp -r "$HERE/data/kernels" "$scratch/k"
    sed -i "$3" "$scratch/k/$2"
    if out=$(check "$scratch/k" 2>&1); then echo "travel-grid: FAIL: $1 left the check green" >&2; exit 1; fi
    grep -q "$4" <<<"$out" || { echo "travel-grid: FAIL: $1 went red without naming $4: $out" >&2; exit 1; }
  }
  red "a step that misses the maximum" comp.json '/"name": "releaseMs"/,/"defaultFrom"/s/"step": 5,/"step": 7,/' 'comp.json releaseMs: max'
  red "a default off the grid" comp.json '/"name": "attackMs"/,/"defaultFrom"/s/"default": 5,/"default": 5.05,/' 'comp.json attackMs: default 5.05'
  red "a use whose set no control holds" eq.json '/"name": "hpfSlope"/,/"default"/{/"ids": \[/,/\],/d}' 'uses FILTER_SLOPES, which no control'
  red "a choice with no ids" eq.json '/"name": "lpfSlope"/,/"global"/{/"global"/d}' 'eq.json lpfSlope: a choice with no ids'
  echo "travel-grid: self-test: all 4 sabotages named"
  exit 0
fi

check "${1:-$HERE/data/kernels}"
