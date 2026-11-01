#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# clone-check.sh — a second copy of a body inside the library goes red. Part of `make lint`.
#
#   tools/clone-check.sh               the library's regions against tools/clone-baseline.txt
#   tools/clone-check.sh --write       writes the baseline: only after removing a copy, never to admit one
#   tools/clone-check.sh --self-test   a planted copy, a function defined twice and a stale baseline line
#                                      must each be named
#
# tools/clone-check.c lists every file pair of include/omxdsp and src that shares a run of 30 tokens or more
# (omxdsp_definitions.h is generated and left out) and every function name two files define. The baseline
# lists what is there today, and it only shrinks: a pair that is not in it, or more regions or tokens than it
# says, is a new copy. A pair below its line is a copy removed, and the line must come down with it.
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

compare() { # <tree> <baseline>: prints each difference, returns 1 on any
  local live; live=$(scan "$1")
  awk -v baseline="$2" '
    BEGIN { while ((getline l < baseline) > 0) if (l !~ /^#/) { split(l, a, " "); k = a[2] " " a[3]; if (a[1] == "def") k = a[2]; want[a[1] " " k] = l } }
    {
      k = $1 " " $2 ($1 == "clone" ? " " $3 : "")
      if (!(k in want)) { print "clone-check: new: " $0; bad = 1; next }
      split(want[k], a, " ")
      if ($1 == "clone" && ($4 > a[4] || $5 > a[5])) { print "clone-check: grew: " $0 " (baseline " a[4] " " a[5] ")"; bad = 1 }
      if ($1 == "clone" && ($4 < a[4] || $5 < a[5])) { print "clone-check: shrank, lower the baseline: " $0 " (baseline " a[4] " " a[5] ")"; bad = 1 }
      if ($1 == "def" && $3 != a[3]) { print "clone-check: differs from the baseline: " $0; bad = 1 }
      delete want[k]
    }
    END { for (k in want) { print "clone-check: gone, remove its baseline line: " want[k]; bad = 1 } exit bad }
  ' <<<"$live"
}

case "${1:-}" in
  --write)
    { sed -n '/^#/p' "$HERE/tools/clone-baseline.txt" 2>/dev/null; scan "$HERE"; } > "$scratch/new"
    mv "$scratch/new" "$HERE/tools/clone-baseline.txt"
    ;;
  --self-test)
    t="$scratch/tree"
    fresh() { rm -rf "$t"; mkdir -p "$t"; cp -r "$HERE/include" "$HERE/src" "$t/"; }
    red() { # <label> <expected text>
      if out=$(compare "$t" "$HERE/tools/clone-baseline.txt"); then echo "clone-check: FAIL: $1 left the check green" >&2; exit 1; fi
      grep -q "$2" <<<"$out" || { echo "clone-check: FAIL: $1 went red without naming $2: $out" >&2; exit 1; }
    }
    fresh
    { sed -n '/^static inline OmxMatchedPair omx_matched_pair/,/^}/p' "$HERE/include/omxdsp/omx_matched_pair.h" | sed 's/omx_matched_pair/omx_planted_pair/'; } > "$t/include/omxdsp/omx_planted.h"
    red "a body copied under another name" 'new: clone include/omxdsp/omx_matched_pair.h include/omxdsp/omx_planted.h\|new: clone include/omxdsp/omx_eq_design.h'
    fresh
    echo 'static inline float omx_clampf(float a) { return a; }' > "$t/include/omxdsp/omx_planted.h"
    red "a function two files define" 'new: def omx_clampf 2'
    fresh
    cp "$HERE/tools/clone-baseline.txt" "$scratch/stale"
    echo 'clone include/omxdsp/gone_a.h include/omxdsp/gone_b.h 1 30' >> "$scratch/stale"
    if out=$(compare "$t" "$scratch/stale"); then echo "clone-check: FAIL: a stale baseline line left the check green" >&2; exit 1; fi
    grep -q 'gone, remove its baseline line' <<<"$out" || { echo "clone-check: FAIL: a stale line was not named: $out" >&2; exit 1; }
    echo "clone-check: self-test: a planted copy, a function defined twice and a stale baseline line were each named"
    ;;
  '')
    compare "$HERE" "$HERE/tools/clone-baseline.txt" || exit 1
    echo "clone-check: $(grep -c '^clone' "$HERE/tools/clone-baseline.txt") file pairs share what the baseline says, no more"
    ;;
  *) sed -n '7,10p' "$0" >&2; exit 2 ;;
esac
