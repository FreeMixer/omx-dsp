#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-agree.sh — every limit this library reads agrees with FreeMixer/omx-contract.
#
#   tools/contract-agree.sh <omx-contract checkout>              the check
#   tools/contract-agree.sh --self-test <omx-contract checkout>  each sabotage must go red
#
# include/omxdsp/omx_contract_limits.h is a committed render (see BUILDING.md). omx-contract
# publishes the same limits as data and renders them to include/omxcontract/omx_contract_limits.h.
# Take every OMX_* name that src/, include/, test/, tools/ and the Makefile read and that the
# committed header defines (the header itself is not a reader). Each one is
#
#   - defined by omx-contract with the same value, or
#   - listed in tools/contract-gap.txt: a limit omx-contract does not carry yet.
#
# A value that differs, a name that is neither, a gap entry omx-contract now carries and a gap entry
# nothing reads any more are all failures, so the gap list can only shrink. When it is empty the
# committed header and tools/render-check.sh are deleted and the build reads the contract's render
# (FreeMixer/omx-dsp#1, #9).
#
# Reads only this tree and the omx-contract checkout at the commit .github/pins.txt names; the
# checkout needs no build. Writes nothing into either tree.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"

# defs <header>: "NAME<TAB>value" for every object-like #define, comment and spacing removed
defs() {
  awk '/^#define [A-Z][A-Z0-9_]* +[^ ]/ {
         name = $2; sub(/^#define [A-Z0-9_]+ +/, "")
         gsub(/\/\*.*\*\//, ""); sub(/\/\/.*/, ""); gsub(/^ +| +$/, "")
         print name "\t" $0 }' "$1" | sort -u
}

# check <ours> <contract header> <gap list> <names read, one per line>
check() {
  local ours="$1" theirs="$2" gap="$3" read_names="$4" fail=0 same=0 gapped=0 n v w
  local d_ours d_theirs
  d_ours="$(mktemp)"; d_theirs="$(mktemp)"
  defs "$ours" > "$d_ours"; defs "$theirs" > "$d_theirs"
  while IFS= read -r n; do
    v="$(awk -F'\t' -v n="$n" '$1 == n { print $2; exit }' "$d_ours")"
    [ -n "$v" ] || continue                      # a name the header does not define is not a limit
    w="$(awk -F'\t' -v n="$n" '$1 == n { print $2; exit }' "$d_theirs")"
    if [ -n "$w" ]; then
      if [ "$v" != "$w" ]; then
        echo "contract-agree: FAIL — $n is '$v' here and '$w' in omx-contract" >&2; fail=1
      elif grep -qxF "$n" "$gap"; then
        echo "contract-agree: FAIL — $n is in omx-contract now: delete it from tools/contract-gap.txt" >&2; fail=1
      else
        same=$((same + 1))
      fi
    elif grep -qxF "$n" "$gap"; then
      gapped=$((gapped + 1))
    else
      echo "contract-agree: FAIL — $n is read here, defined here and neither in omx-contract nor in tools/contract-gap.txt" >&2; fail=1
    fi
  done < "$read_names"
  while IFS= read -r n; do
    case "$n" in ''|'#'*) continue ;; esac
    grep -qxF "$n" "$read_names" || { echo "contract-agree: FAIL — tools/contract-gap.txt lists $n and nothing reads it: delete the line" >&2; fail=1; }
  done < "$gap"
  rm -f "$d_ours" "$d_theirs"
  # a check that compared nothing proves nothing
  if [ "$same" -lt 50 ]; then
    echo "contract-agree: FAIL — only $same limits compared; the checkout or the read set is wrong" >&2; fail=1
  fi
  [ "$fail" = 0 ] || return 1
  echo "contract-agree: $same limits agree with omx-contract, $gapped not carried by it yet (tools/contract-gap.txt)"
}

# names_read: every OMX_* token the sources read, the committed header excluded
names_read() {
  (cd "$HERE" && grep -rhoE '\bOMX_[A-Z0-9_]+\b' src include test tools Makefile \
      --exclude=omx_contract_limits.h --exclude=contract-gap.txt --exclude=contract-agree.sh) | sort -u
}

OURS="$HERE/include/omxdsp/omx_contract_limits.h"
GAP="$HERE/tools/contract-gap.txt"

if [ "${1:-}" = --self-test ]; then
  CONTRACT="${2:?usage: tools/contract-agree.sh --self-test <omx-contract checkout>}"
  THEIRS="$CONTRACT/include/omxcontract/omx_contract_limits.h"
  scratch="$(mktemp -d)"; trap 'rm -rf "$scratch"' EXIT
  names_read > "$scratch/read"
  grep -vE '^(#|$)' "$GAP" > "$scratch/gap"
  check "$OURS" "$THEIRS" "$scratch/gap" "$scratch/read" >/dev/null 2>&1 || { echo "self-test: the unsabotaged check is not green" >&2; exit 1; }
  fail=0
  sabotage() { # $1 name, then the check is run on whatever the caller prepared in $scratch
    if check "$scratch/ours" "$scratch/theirs" "$scratch/gap2" "$scratch/read" >"$scratch/log" 2>&1; then
      echo "SABOTAGE $1: STAYED GREEN"; fail=1
    else
      echo "SABOTAGE $1: red — $(grep -m1 FAIL "$scratch/log")"
    fi
  }
  reset() { cp "$OURS" "$scratch/ours"; cp "$THEIRS" "$scratch/theirs"; cp "$scratch/gap" "$scratch/gap2"; }
  # a shared limit moves here
  reset; sed -i 's/^#define OMX_PAN_PAN_MIN .*/#define OMX_PAN_PAN_MIN -0.5f/' "$scratch/ours"; sabotage "a shared value moved in the committed header"
  # ... or in the contract
  reset; sed -i 's/^#define OMX_PAN_PAN_MIN .*/#define OMX_PAN_PAN_MIN -0.5f/' "$scratch/theirs"; sabotage "a shared value moved in omx-contract"
  # a gap entry vanishes
  reset; sed -i '1d' "$scratch/gap2"; sabotage "a gap entry deleted"
  # a gap entry the contract carries
  reset; echo OMX_PAN_PAN_MIN >> "$scratch/gap2"; sabotage "a carried limit listed as a gap"
  # a gap entry nothing reads
  reset; echo OMX_NOTHING_READS_THIS >> "$scratch/gap2"; sabotage "a gap entry nothing reads"
  # a contract that carries nothing compares nothing
  reset; : > "$scratch/theirs"; sabotage "an empty contract header"
  [ "$fail" = 0 ] || exit 1
  echo "contract-agree self-test: every sabotage red"
  exit 0
fi

CONTRACT="${1:?usage: tools/contract-agree.sh <omx-contract checkout>}"
THEIRS="$CONTRACT/include/omxcontract/omx_contract_limits.h"
[ -f "$THEIRS" ] || { echo "contract-agree: $THEIRS does not exist" >&2; exit 2; }
pin="$(awk '$1 == "omx-contract" { print $3 }' "$HERE/.github/pins.txt")"
have="$(git -C "$CONTRACT" rev-parse HEAD)"
if [ -n "$pin" ] && [ "$have" != "$pin" ]; then
  echo "contract-agree: $CONTRACT is at $have, the pin is $pin" >&2
  exit 2
fi
scratch="$(mktemp -d)"; trap 'rm -rf "$scratch"' EXIT
names_read > "$scratch/read"
grep -vE '^(#|$)' "$GAP" > "$scratch/gap"
check "$OURS" "$THEIRS" "$scratch/gap" "$scratch/read"
