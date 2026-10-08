#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-single-source.sh — no name omx-contract defines is defined again here.
#
#   tools/contract-single-source.sh            the check
#   tools/contract-single-source.sh --self-test  a restored copy must be named
#
# omx-contract is the one home of every value more than one repository reads. A `#define` in include/,
# src/, test/ or tools/ of a name the pinned release already defines is a second copy that can drift
# from it; the compiler only objects when the two spellings differ in text. Part of `make lint`.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
: "${CONTRACT_INC:=$(sh "$HERE/tools/contract-include.sh")}"
H="$CONTRACT_INC/omxcontract/omx_contract_limits.h"

names() { grep -oE '^#[[:space:]]*define[[:space:]]+OMX_[A-Z0-9_]+' "$1" | awk '{print $2}' | sort -u; }

# scan <contract names file> <dirs...>: "file:line: #define NAME" for every redefinition
scan() {
  local theirs="$1"; shift
  (cd "$HERE" && grep -rnE '^[[:space:]]*#[[:space:]]*define[[:space:]]+OMX_[A-Z0-9_]+' "$@" 2>/dev/null) \
    | while IFS= read -r l; do
        n="$(printf '%s' "$l" | grep -oE 'define[[:space:]]+OMX_[A-Z0-9_]+' | awk '{print $2}')"
        grep -qx "$n" "$theirs" && printf '%s\n' "$l"
      done || true
}

scratch="$(mktemp -d)"; trap 'rm -rf "$scratch"' EXIT
names "$H" > "$scratch/theirs"
[ "$(wc -l < "$scratch/theirs")" -gt 100 ] || { echo "contract-single-source: only $(wc -l < "$scratch/theirs") names in $H; the check would prove nothing" >&2; exit 2; }

if [ "${1:-}" = --self-test ]; then
  copy="$HERE/include/omxdsp/fx/omx_chorus.h"
  cp "$copy" "$scratch/orig"
  trap 'cp "$scratch/orig" "$copy"; rm -rf "$scratch"' EXIT
  printf '#define OMX_CHORUS_BASE_MS 10.0f\n' >> "$copy"
  if scan "$scratch/theirs" include src | grep -q 'OMX_CHORUS_BASE_MS'; then echo "contract-single-source self-test: a restored copy was named"; exit 0; fi
  echo "contract-single-source self-test: a restored copy of OMX_CHORUS_BASE_MS was NOT named" >&2; exit 1
fi

found="$(scan "$scratch/theirs" include src test tools)"
if [ -n "$found" ]; then
  echo "contract-single-source: these names are defined by omx-contract and again here:" >&2
  printf '%s\n' "$found" >&2
  exit 1
fi
echo "contract-single-source: no name of omx-contract's $(wc -l < "$scratch/theirs") is defined again here"
