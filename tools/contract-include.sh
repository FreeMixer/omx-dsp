#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-include.sh — prints the directory whose omxcontract/ holds the omx-contract C header
# this tree builds against (FreeMixer/omx-dsp#1, #9, #107). The header is never committed here.
#
#   1. $OMX_CONTRACT_INC, when set: a scratch render, used by the perturbation arms. Not
#      version-checked: it is a deliberate other contract.
#   2. the installed omx-contract-devel / libomx-contract-dev, found by pkg-config, when its version
#      is inside the range packaging/omx-dsp.spec declares (BuildRequires: omx-contract-devel >= floor,
#      < ceiling; debian/control carries the same range, tools/contract-include-selftest.sh holds it).
#
# Nothing is downloaded. When the installed omx-contract is missing or outside the range the script
# stops, naming the package, the range and the version installed.
#
#   --range  prints "<floor> <ceiling>" as the spec declares them (the Makefile renders omxdsp.pc
#            with them).
#   --data   prints instead a directory of the installed package's kernels' controls, one
#            <kernel>.json each (each kernel's ordered `controls`), cut from its omx-contract.json
#            into build/omx-contract-kernels/<version>/; tools/face-conformance.sh reads them.
#
# Prints one line on stdout and nothing else; the reason for a refusal goes to stderr, exit 1.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SPEC="$HERE/packaging/omx-dsp.spec"
mode="${1:-}"
floor="$(sed -n 's/^BuildRequires: *omx-contract-devel *>= *\([0-9][0-9.]*\) *$/\1/p' "$SPEC")"
ceiling="$(sed -n 's/^BuildRequires: *omx-contract-devel *< *\([0-9][0-9.]*\) *$/\1/p' "$SPEC")"
[ -n "$floor" ] && [ -n "$ceiling" ] || { echo "contract-include: $SPEC declares no 'BuildRequires: omx-contract-devel >= <floor>' and '< <ceiling>' pair" >&2; exit 1; }
if [ "$mode" = --range ]; then printf '%s %s\n' "$floor" "$ceiling"; exit 0; fi
if [ "$mode" != --data ] && [ -n "${OMX_CONTRACT_INC:-}" ]; then
  [ -f "$OMX_CONTRACT_INC/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: OMX_CONTRACT_INC=$OMX_CONTRACT_INC has no omxcontract/omx_contract_limits.h" >&2; exit 1; }
  printf '%s\n' "$OMX_CONTRACT_INC"; exit 0
fi
need="omx-contract >= $floor, < $ceiling (omx-contract-devel / libomx-contract-dev: dnf builddep packaging/omx-dsp.spec, apt-get build-dep ./)"
command -v pkg-config >/dev/null 2>&1 || { echo "contract-include: pkg-config is missing, so the installed omx-contract cannot be found; this tree needs $need" >&2; exit 1; }
have="$(pkg-config --modversion omx-contract 2>/dev/null || true)"
[ -n "$have" ] || { echo "contract-include: omx-contract is not installed; this tree needs $need" >&2; exit 1; }
pkg-config --atleast-version="$floor" omx-contract || { echo "contract-include: omx-contract $have is installed, below the floor $floor; this tree needs $need" >&2; exit 1; }
# The ceiling is a version comparison, not a prefix: under "< 2.11", 2.12.0 is refused too.
if pkg-config --atleast-version="$ceiling" omx-contract; then
  echo "contract-include: omx-contract $have is installed, at or above the ceiling $ceiling; this tree needs $need" >&2; exit 1
fi
if [ "$mode" = --data ]; then
  datadir="$(pkg-config --variable=datadir omx-contract)"
  json="$datadir/omx-contract.json"
  [ -f "$json" ] || { echo "contract-include: the installed omx-contract $have has no $json" >&2; exit 1; }
  # The kernels object, pretty-printed two spaces deep: each kernel's "controls" array is written
  # out as <kernel>.json in the layout tools/face-conformance.sh reads.
  out="$HERE/build/omx-contract-kernels/$have"
  rm -rf "$out"; mkdir -p "$out"
  awk -v out="$out" '
    /^  "kernels": \{/ { inside = 1; next }
    inside && /^  \}/ { inside = 0 }
    inside && /^    "[A-Za-z0-9_]+": \{$/ { k = $0; sub(/^    "/, "", k); sub(/".*/, "", k) }
    inside && /^      "controls": \[/ { f = out "/" k ".json"; print "{" > f; print "  \"controls\": [" > f; on = 1; next }
    on && /^      \]/ { print "  ]" > f; print "}" > f; close(f); on = 0; next }
    on { print substr($0, 5) > f }
  ' "$json"
  [ -n "$(ls "$out")" ] || { echo "contract-include: $json (omx-contract $have) holds no kernels' controls" >&2; exit 1; }
  printf '%s\n' "$out"; exit 0
fi
inc="$(pkg-config --variable=includedir omx-contract)"
[ -f "$inc/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: omx-contract $have is installed but $inc/omxcontract/omx_contract_limits.h is missing" >&2; exit 1; }
printf '%s\n' "$inc"
