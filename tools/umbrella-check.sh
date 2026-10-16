#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# umbrella-check.sh — every public header under include/omxdsp is reached from omxdsp.h, or is named in
# tools/umbrella-separate.txt as deliberately separate (FreeMixer/omx-dsp#74). A header that neither
# the umbrella includes nor the list names is easy to miss, so a new one has to be put in one place.
# Part of `make lint`.
#
#   tools/umbrella-check.sh [INCLUDE_DIR [SEPARATE_LIST]]   the check
#   tools/umbrella-check.sh --self-test                     a header left out of both must be named
#
# The list holds paths relative to include/omxdsp; a name ending in `/` stands for the whole directory.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"

# check <include dir> <separate list>: prints every public header neither reached nor listed
check() {
  local inc="$1" list="$2" q f d i c
  local -A seen=()
  q=("omxdsp/omxdsp.h")
  while [ "${#q[@]}" -gt 0 ]; do
    f="${q[0]}"; q=("${q[@]:1}")
    [ -z "${seen[$f]:-}" ] || continue
    seen[$f]=1
    d="$(dirname "$f")"
    while IFS= read -r i; do
      for c in "$d/$i" "$i"; do
        if [ -f "$inc/$c" ]; then q+=("$c"); break; fi
      done
    done < <(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*["<]\([^">]*\)[">].*/\1/p' "$inc/$f")
  done
  (cd "$inc" && find omxdsp -name '*.h' | sort) | while IFS= read -r f; do
    [ -z "${seen[$f]:-}" ] || continue
    rel="${f#omxdsp/}"
    if grep -vE '^[[:space:]]*(#|$)' "$list" | awk -v r="$rel" '
        { n=$1; if (n == r || (substr(n, length(n)) == "/" && index(r, n) == 1)) found=1 }
        END { exit !found }'; then continue; fi
    printf '%s\n' "$rel"
  done
}

if [ "${1:-}" = --self-test ]; then
  T="$(mktemp -d "${TMPDIR:-/tmp}/umbrella-check.XXXXXX")"; trap 'rm -rf "$T"' EXIT
  mkdir -p "$T/omxdsp/fx"
  printf '#include "omx_a.h"\n' > "$T/omxdsp/omxdsp.h"
  printf '#include "omx_b.h"\n' > "$T/omxdsp/omx_a.h"   # b is reached through a, not named by the umbrella
  : > "$T/omxdsp/omx_b.h"; : > "$T/omxdsp/fx/omx_sep.h"; : > "$T/omxdsp/omx_lost.h"
  printf 'fx/\n' > "$T/list"
  got="$(check "$T" "$T/list")"
  fail=0
  [ "$got" = omx_lost.h ] || { echo "umbrella-check --self-test: FAIL — expected only omx_lost.h, got: ${got:-nothing}"; fail=1; }
  printf 'fx/\nomx_lost.h\n' > "$T/list"
  [ -z "$(check "$T" "$T/list")" ] || { echo "umbrella-check --self-test: FAIL — a listed header was named"; fail=1; }
  [ "$fail" -eq 0 ] && echo "umbrella-check --self-test: a header in neither place is named; reached, listed and directory-listed ones are not"
  exit "$fail"
fi

found="$(check "${1:-$HERE/include}" "${2:-$HERE/tools/umbrella-separate.txt}")"
if [ -n "$found" ]; then
  echo "umbrella-check: these public headers are neither included by omxdsp.h nor listed in tools/umbrella-separate.txt:" >&2
  printf '  %s\n' $found >&2
  exit 1
fi
echo "umbrella-check: every public header is reached from omxdsp.h or listed as separate"
