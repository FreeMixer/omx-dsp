#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-single-source.sh — no name omx-contract or this library's definitions define is defined again here.
#
#   tools/contract-single-source.sh            the check
#   tools/contract-single-source.sh --self-test  a redefinition, a staged guard and an #undef in a
#                                                scratch copy must each be named
#
# omx-contract is the one home of every value more than one repository reads. A `#define` in include/,
# src/, test/ or tools/ of a name the installed release already defines is a second copy that can drift
# from it; the compiler only objects when the two spellings differ in text. A guard on such a name
# (`#ifndef`, `#ifdef`, `#if [!]defined(...)`) is the staged form of the same copy, written for a
# contract that did not declare the name yet: once the installed release declares it, the guard is a
# copy waiting to drift, and is named too. So is an `#undef`, the first half of a redefinition. The names
# this library owns are defined once, in include/omxdsp/omxdsp_definitions.h, rendered from data/ by
# omx-contract's tool (make definitions): that header is the one second definer allowed, and every other line
# that defines, undefines or guards one of its names is named as for a name of the installed omx-contract.
# Part of `make lint`, `make checks` and so of `make test`.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
: "${CONTRACT_INC:=$(sh "$HERE/tools/contract-include.sh")}"
H="$CONTRACT_INC/omxcontract/omx_contract_limits.h"
OWN=include/omxdsp/omxdsp_definitions.h

names() { grep -oE '^#[[:space:]]*define[[:space:]]+OMX_[A-Z0-9_]+' "$1" | awk '{print $2}' | sort -u; }

# scan <contract names file> <dirs...>: "file:line: <directive>" for every preprocessor line that
# defines, undefines or guards on a name of the contract
scan() {
  local theirs="$1"; shift
  (cd "${SCAN_ROOT:-$HERE}" && grep -rnE '^[[:space:]]*#[[:space:]]*(define|undef|ifndef|ifdef|if|elif)[[:space:]].*OMX_[A-Z0-9_]+' "$@" 2>/dev/null) \
    | grep -v "^$OWN:" \
    | while IFS= read -r l; do
        local d n
        d="${l#*:}"; d="${d#*:}"
        n="$(printf '%s' "$d" | grep -oE '#[[:space:]]*(define|undef|ifndef|ifdef)[[:space:]]+OMX_[A-Z0-9_]+' | grep -oE 'OMX_[A-Z0-9_]+' || true)"
        n="$n $(printf '%s' "$d" | grep -oE 'defined[[:space:]]*\(?[[:space:]]*OMX_[A-Z0-9_]+' | grep -oE 'OMX_[A-Z0-9_]+' || true)"
        for x in $n; do
          if grep -qx "$x" "$theirs"; then printf '%s\n' "$l"; break; fi
        done
      done || true
}

scratch="$(mktemp -d)"; trap 'rm -rf "$scratch"' EXIT
[ -f "$HERE/$OWN" ] || { echo "contract-single-source: no $OWN (make definitions renders it)" >&2; exit 2; }
cat <(names "$H") <(names "$HERE/$OWN") | sort -u > "$scratch/theirs"
[ "$(wc -l < "$scratch/theirs")" -gt 100 ] || { echo "contract-single-source: only $(wc -l < "$scratch/theirs") names in $H and $OWN; the check would prove nothing" >&2; exit 2; }

if [ "${1:-}" = --self-test ]; then
  # the edits land on copies under the scratch directory; the tracked tree is never touched, so a
  # parallel `make -j test` cannot compile a half-edited header
  mkdir -p "$scratch/tree/include/omxdsp/fx" "$scratch/tree/test"
  cp "$HERE/include/omxdsp/fx/omx_chorus.h" "$scratch/tree/include/omxdsp/fx/omx_chorus.h"
  cp "$HERE/include/omxdsp/fx/omx_delay.h" "$scratch/tree/include/omxdsp/fx/omx_delay.h"
  printf '#define OMX_CHORUS_BASE_MS 10.0f\n' >> "$scratch/tree/include/omxdsp/fx/omx_chorus.h"
  printf '#if !defined(OMX_FX_DELAY_TIME_RANGE_MAX)\n#endif\n' >> "$scratch/tree/include/omxdsp/fx/omx_delay.h"
  printf '#undef OMX_RT_HARD_TARGET_RATE\n' > "$scratch/tree/test/staged.c"
  # the rendered header defines every name it holds, and is the one place allowed to
  cp "$HERE/$OWN" "$scratch/tree/$OWN"
  export SCAN_ROOT="$scratch/tree"
  got="$(scan "$scratch/theirs" include src test)"
  bad=0
  for want in 'omx_chorus.h:.*define OMX_CHORUS_BASE_MS' 'omx_delay.h:.*defined(OMX_FX_DELAY_TIME_RANGE_MAX)' 'staged.c:.*undef OMX_RT_HARD_TARGET_RATE'; do
    grep -q "$want" <<<"$got" || { echo "contract-single-source self-test: '$want' was NOT named" >&2; bad=1; }
  done
  # the untouched copies' own lines and the rendered header are not named: only the three planted ones
  [ "$(grep -c . <<<"$got")" -eq 3 ] || { echo "contract-single-source self-test: named $(grep -c . <<<"$got") lines, want the 3 planted:" >&2; printf '%s\n' "$got" >&2; bad=1; }
  [ "$bad" = 0 ] || exit 1
  echo "contract-single-source self-test: a redefinition, a staged guard and an #undef in scratch copies were each named"
  exit 0
fi

found="$(scan "$scratch/theirs" include src test tools)"
if [ -n "$found" ]; then
  echo "contract-single-source: these names are defined by omx-contract or $OWN and again here:" >&2
  printf '%s\n' "$found" >&2
  exit 1
fi
echo "contract-single-source: no name of the $(wc -l < "$scratch/theirs") of omx-contract and $OWN is defined again here"
