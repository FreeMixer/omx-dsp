#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# clone-check.sh — a second copy of a body inside the library goes red. Part of `make lint`.
#
#   tools/clone-check.sh               the library's regions: any region two files share is red
#   tools/clone-check.sh --self-test   a planted copy and a function defined twice must each be named,
#                                      and the library as it stands must be green
#
# tools/clone-check.c lists every file pair of include/omxdsp and src that shares a run of 30 tokens or more
# (omxdsp_definitions.h is generated and left out) and every function name two files define. The library
# keeps one copy of each body: shared code lives in one header and the others call it, so the list is empty
# and any line in it is a copy.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-cc}"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/clone-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
$CC -std=c11 -O2 -Wall -Wextra -Werror -o "$scratch/clone-check" "$HERE/tools/clone-check.c"

scan() { # <tree>: relative paths, so a checkout's location is no part of the result
  (cd "$1" && find include/omxdsp src \( -name '*.h' -o -name '*.c' \) ! -name omxdsp_definitions.h | LC_ALL=C sort |
    xargs "$scratch/clone-check")
}

check() { # <tree>: prints each copy, returns 1 on any
  local live; live=$(scan "$1")
  [ -z "$live" ] && return 0
  sed 's/^/clone-check: copy: /' <<<"$live"
  return 1
}

case "${1:-}" in
  --self-test)
    t="$scratch/tree"
    fresh() { rm -rf "$t"; mkdir -p "$t"; cp -r "$HERE/include" "$HERE/src" "$t/"; }
    red() { # <label> <expected text>
      if out=$(check "$t"); then echo "clone-check: FAIL: $1 left the check green" >&2; exit 1; fi
      grep -q "$2" <<<"$out" || { echo "clone-check: FAIL: $1 went red without naming $2: $out" >&2; exit 1; }
    }
    fresh
    check "$t" >/dev/null || { echo "clone-check: FAIL: the library as it stands is red" >&2; exit 1; }
    { sed -n '/^static inline OmxMatchedPair omx_matched_pair/,/^}/p' "$HERE/include/omxdsp/omx_matched_pair.h" | sed 's/omx_matched_pair/omx_planted_pair/'; } > "$t/include/omxdsp/omx_planted.h"
    red "a body copied under another name" 'copy: clone include/omxdsp/omx_matched_pair.h include/omxdsp/omx_planted.h'
    fresh
    echo 'static inline float omx_clampf(float a) { return a; }' > "$t/include/omxdsp/omx_planted.h"
    red "a function two files define" 'copy: def omx_clampf 2'
    echo "clone-check: self-test: a planted copy and a function defined twice were each named"
    ;;
  '')
    check "$HERE" || exit 1
    echo "clone-check: no two files of the library share a body"
    ;;
  *) sed -n '7,9p' "$0" >&2; exit 2 ;;
esac
