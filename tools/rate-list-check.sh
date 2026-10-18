#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# rate-list-check.sh — no test or tool restates a rate list omx-contract declares (FreeMixer/omx-dsp#190).
# The four floor rates (OMX_ORACLE_FLOOR_RATES_*), the nine full-oracle rates (OMX_ORACLE_FULL_RATES_*)
# and the declared six (OMX_DECLARED_RATES) come from the contract; a copy typed into a test or a tool
# stays behind the day the contract moves them. Part of `make lint`.
#
# A list is three or more consecutive rates in the declared order: 44100, 48000, 96000 (the floor
# and the declared lists begin so) or 32000, 44100, 48000 (the full list). A table of values per rate
# starts each row at its rate and is not a list; a two-rate pair is not either.
#
#   tools/rate-list-check.sh [DIR...]   the check over DIR (default test tools), generated fixtures and
#                                       golden digests excluded
#   tools/rate-list-check.sh --self-test   each spelling of a list must be named, a per-rate table not
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
LIST="(^|[^0-9.])(44100(\\.0*f?)?[[:space:],]+48000(\\.0*f?)?[[:space:],]+96000|32000(\\.0*f?)?[[:space:],]+44100(\\.0*f?)?[[:space:],]+48000)"

# offenders <file>...: "file:line: text" for every line holding a list
offenders() {
  local f
  for f in "$@"; do
    grep -nE "$LIST" "$f" | sed "s|^|$f:|" || true
  done
}

files() { find "$@" -type f \( -name '*.c' -o -name '*.h' -o -name '*.sh' -o -name '*.mjs' -o -name '*.js' -o -name '*.py' \) \
  ! -path '*/fixtures/*' ! -path '*/golden/*' ! -name 'rate-list-check.sh' | sort; }

if [ "${1:-}" = --self-test ]; then
  T="$(mktemp -d "${TMPDIR:-/tmp}/rate-list-check.XXXXXX")"; trap 'rm -rf "$T"' EXIT
  fail=0
  while IFS= read -r good; do
    printf '%s\n' "$good" > "$T/g.c"
    [ -z "$(offenders "$T/g.c")" ] || { echo "rate-list-check --self-test: FAIL — named: $good"; fail=1; }
  done <<'GOOD'
static const double rates[2] = {44100.0, 48000.0};
{44100.0f, {-11.630863f, -17.018297f}},
static const float R[4] = OMX_ORACLE_FLOOR_RATES_INIT;
GOOD
  while IFS= read -r bad; do
    printf '%s\n' "$bad" > "$T/b.c"
    [ -n "$(offenders "$T/b.c")" ] || { echo "rate-list-check --self-test: FAIL — not named: $bad"; fail=1; }
  done <<'BAD'
static const float R[4] = {44100.0f, 48000.0f, 96000.0f, 192000.0f};
static const double R[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
const RATES = [32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000];
RATES="44100 48000 96000 192000"
BAD
  [ "$fail" -eq 0 ] && echo "rate-list-check --self-test: a typed list of rates is named; a pair, a per-rate table and the contract's own define are not"
  exit "$fail"
fi

[ "$#" -gt 0 ] || set -- "$HERE/test" "$HERE/tools"
mapfile -t all < <(files "$@")
[ "${#all[@]}" -gt 0 ] || { echo "rate-list-check: FAIL — no source found under: $*"; exit 1; }
found="$(offenders "${all[@]}")"
if [ -n "$found" ]; then
  echo "rate-list-check: FAIL — a rate list omx-contract declares is typed again; read OMX_ORACLE_FLOOR_RATES_INIT,"
  echo "OMX_ORACLE_FULL_RATES_INIT or OMX_DECLARED_RATES instead:"
  echo "$found"
  exit 1
fi
echo "rate-list-check: ${#all[@]} sources, none restates a rate list the contract declares"
