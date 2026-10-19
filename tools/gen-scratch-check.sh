#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# gen-scratch-check.sh — no script under test/ or tools/ writes tools/kernels-gen.sh's output into
# build/ (FreeMixer/omx-dsp#153, #163). The suite's own recipe writes build/kernels-suite.inc.c and
# build/kernels-perturb.inc.c; a second writer there lets a parallel make compile one target against
# a half-written file of the other. Every script generates into its own scratch directory.
# Part of `make lint`.
#
#   tools/gen-scratch-check.sh [DIR...]   the check, over the .sh files under DIR (default test tools)
#   tools/gen-scratch-check.sh --self-test   a script generating into build/ must be named
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"

# offenders <dir>...: "file:line: text" for every kernels-gen.sh call whose out dir is build/
offenders() {
  local f
  while IFS= read -r f; do
    grep -nE 'kernels-gen\.sh"?[[:space:]]+"?(\$PKG/|\$\{PKG\}/|\./)?(\$\(BUILD\)|\$BUILD|build)"?([[:space:]>;&|]|$)' "$f" |
      sed "s|^|$f:|" || true
  done < <(find "$@" -name '*.sh' ! -name 'gen-scratch-check.sh' | sort)
}

if [ "${1:-}" = --self-test ]; then
  T="$(mktemp -d "${TMPDIR:-/tmp}/gen-scratch-check.XXXXXX")"; trap 'rm -rf "$T"' EXIT
  printf 'bash "$PKG/tools/kernels-gen.sh" "$OUT" >/dev/null\nbash "$PKG/tools/kernels-gen.sh" "$OUT/gen"\n' > "$T/ok.sh"
  fail=0
  [ -z "$(offenders "$T")" ] || { echo "gen-scratch-check --self-test: FAIL — a scratch dir was named"; fail=1; }
  for bad in '"$PKG/build"' 'build' '"$PKG/build" >/dev/null' '$(BUILD)'; do
    printf 'bash "$PKG/tools/kernels-gen.sh" %s\n' "$bad" > "$T/bad.sh"
    [ -n "$(offenders "$T")" ] || { echo "gen-scratch-check --self-test: FAIL — not named: $bad"; fail=1; }
  done
  [ "$fail" -eq 0 ] && echo "gen-scratch-check --self-test: a generation into build/ is named; scratch dirs are not"
  exit "$fail"
fi

[ "$#" -gt 0 ] || set -- "$HERE/test" "$HERE/tools"
n="$(find "$@" -name '*.sh' | wc -l)"
[ "$n" -gt 0 ] || { echo "gen-scratch-check: FAIL — no script found under: $*"; exit 1; }
found="$(offenders "$@")"
if [ -n "$found" ]; then
  echo "gen-scratch-check: FAIL — kernels-gen.sh writes into build/, where the suite recipe writes the same files:"
  echo "$found"
  exit 1
fi
echo "gen-scratch-check: $n scripts, none generates the kernel lists into build/"
