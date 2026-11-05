#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Debian bookworm's GCC 12.2 on arm64 dies with an internal compiler error in
# vect_transform_reduction on a `double` running maximum or minimum kept with fmax/fmin over values
# widened from float (BUILDING.md, "GCC 12.2 on arm64"). Only an arm64 bookworm build sees the
# crash, so this scan refuses the shape on any host: `x = fmax(x, ... (double) ...)`. Write it as
# `if (!(e <= x)) x = e;` instead.
#
#   tools/reduction-check.sh [TREE]      the check, over the C files TREE (default: this checkout) tracks
#   tools/reduction-check.sh --self-test each failure, planted in a scratch tree, must go red
#
# No file read is a failure: a git that cannot read the tree hands the scan an empty list.
set -euo pipefail

check() { # <tree>
  local tree=$1 found=0 hit
  local -a files=()
  mapfile -d '' -t files < <(git -C "$tree" ls-files -z -- '*.c' '*.h') || true
  if [[ ${#files[@]} == 0 ]]; then
    echo "reduction-check: git lists no C file in $tree, so nothing was read" >&2
    return 1
  fi
  while IFS= read -r hit; do
    echo "reduction-check: $hit" >&2
    found=1
  done < <(cd "$tree" && printf '%s\0' "${files[@]}" | xargs -0 grep -nHE '\b([A-Za-z_][A-Za-z0-9_]*) = fm(ax|in)\(\1, [^;]*\(double\)' || true)
  if [[ $found == 0 ]]; then
    echo "reduction-check: ${#files[@]} C files read, no fmax/fmin running reduction over widened floats"
  else
    echo "reduction-check: write these as comparisons, see BUILDING.md \"GCC 12.2 on arm64\"" >&2
  fi
  return "$found"
}

if [[ ${1:-} == --self-test ]]; then
  t=$(mktemp -d); trap 'rm -rf "$t"' EXIT; fail=0
  mk() { rm -rf "$t/r"; git init -q "$t/r"; printf 'double f(float a) { return (double)a; }\n' > "$t/r/a.c"; git -C "$t/r" add -A; }
  say() { # <verdict wanted: green|red> <name> <text the output names>
    local out rc=0
    out=$(check "$t/r" 2>&1) || rc=$?
    if { [[ $1 == green ]] && [[ $rc == 0 ]]; } || { [[ $1 == red ]] && [[ $rc != 0 ]]; }; then
      if grep -qF -- "$3" <<<"$out"; then echo "ok   $2"; else echo "FAIL $2: for another reason: $out"; fail=1; fi
    else echo "FAIL $2: $out"; fail=1; fi
  }
  mk; say green "a clean tree is green and says how much it read" '1 C files read'
  mk; printf 'double f(float a) { double x = 0; x = fmax(x, (double)a); return x; }\n' > "$t/r/b.c"; git -C "$t/r" add -A
  say red "the shape is red" 'b.c:1'
  rm -rf "$t/r"; git init -q "$t/r"; say red "an empty tree is red" 'lists no C file'
  rm -rf "$t/r"; git init -q "$t/r"; printf 'x\n' > "$t/r/a.txt"; git -C "$t/r" add -A; say red "a tree with no C file is red" 'lists no C file'
  rm -rf "$t/r"; mkdir -p "$t/r"; say red "a directory git cannot read is red" 'lists no C file'
  # as a container's checkout reads: another user's tree, no safe.directory
  mk; out=$(GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 GIT_TEST_ASSUME_DIFFERENT_OWNER=1 check "$t/r" 2>&1) && { echo "FAIL a checkout git refuses stays green: $out"; fail=1; } || echo "ok   a checkout git refuses is red"
  [[ $fail == 0 ]] || exit 1
  echo "reduction-check self-test: every failure red"
  exit 0
fi

check "${1:-$(cd "$(dirname "$0")/.." && pwd)}"
