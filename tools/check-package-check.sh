#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# check-package-check.sh — make check-package (the package build's check) leaves out of make test only
# what proves the tests rather than the library.
#
#   tools/check-package-check.sh [Makefile]   the check, over this tree's Makefile (or the one given)
#   tools/check-package-check.sh --self-test  an oracle left out of check-package and a check-package
#                                             target make test does not run must each be named
#
# Every target of test-parallel is in CHECK_PACKAGE, is the parent of one (test-fx of fx-oracles), or is
# a sabotage or perturbation arm or a source check: *-sabotage, perturb, checks, copy-check, gen-once and
# fastmath-plain (a second build with FASTMATH=off). Every target of CHECK_PACKAGE is in test-parallel
# or a prerequisite of one of its targets.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"

# The Makefile is read as text, not through make, which needs omx-contract installed to parse it.
# the prerequisites of a rule, as the line `target: ...` spells them (the rules read here are one line)
prereqs() { awk -v t="$2" '$1 == t":" { $1 = ""; print; exit }' "$1"; }

check() {
  local mk="$1" pkg tp t p rc=0 parents=""
  pkg=$(awk '/^CHECK_PACKAGE = / { on = 1; sub(/^CHECK_PACKAGE = /, "") }
             on { more = sub(/\\$/, ""); printf "%s ", $0; if (!more) exit }' "$mk" | xargs)
  tp=$(prereqs "$mk" test-parallel)
  [ -n "$pkg" ] || { echo "check-package-check: $mk names no CHECK_PACKAGE"; return 1; }
  [ -n "$tp" ] || { echo "check-package-check: $mk has no test-parallel rule"; return 1; }
  for t in $pkg; do
    if [[ " $tp " == *" $t "* ]]; then continue; fi
    for p in $tp; do
      if [[ " $(prereqs "$mk" "$p") " == *" $t "* ]]; then parents="$parents $p"; continue 2; fi
    done
    echo "check-package-check: check-package runs $t, which make test does not"; rc=1
  done
  for t in $tp; do
    [[ " $pkg $parents " == *" $t "* ]] && continue
    case "$t" in *-sabotage|perturb|checks|copy-check|gen-once|fastmath-plain) continue ;; esac
    echo "check-package-check: make test runs $t, which make check-package does not, and it is no sabotage, perturbation or source check"; rc=1
  done
  return $rc
}

if [ "${1:-}" = "--self-test" ]; then
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  check "$HERE/Makefile" > /dev/null || { echo "check-package-check: self-test: the tree's Makefile is not green"; exit 1; }
  sed 's/^\(CHECK_PACKAGE = lib suite\) negative /\1 /' "$HERE/Makefile" > "$tmp/drop.mk"
  out=$(check "$tmp/drop.mk" || true)
  grep -q 'make test runs negative, which make check-package does not' <<<"$out" || \
    { echo "check-package-check: self-test: an oracle left out of check-package went unnamed"; exit 1; }
  sed 's/^\(CHECK_PACKAGE = lib suite\) /\1 test-tsan /' "$HERE/Makefile" > "$tmp/extra.mk"
  out=$(check "$tmp/extra.mk" || true)
  grep -q 'check-package runs test-tsan, which make test does not' <<<"$out" || \
    { echo "check-package-check: self-test: a target make test does not run went unnamed"; exit 1; }
  echo "check-package-check: self-test green"
  exit 0
fi

check "${1:-$HERE/Makefile}"
echo "check-package-check: make check-package leaves out only the sabotage, perturbation and source checks"
