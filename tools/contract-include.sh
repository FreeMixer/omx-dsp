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
#      is at or above the floor packaging/omx-dsp.spec declares (BuildRequires: omx-contract-devel >= floor;
#      debian/control and omxdsp.pc.in carry the same floor, tools/contract-include-selftest.sh holds it).
#      There is no ceiling: a floor is what the tree needs, a ceiling would only forbid a newer contract.
#
# Nothing is downloaded. When the installed omx-contract is missing or below the floor the script
# stops, naming the package, the floor and the version installed.
#
#   --floor  prints the floor as the spec declares it (the Makefile renders omxdsp.pc with it).
#   --data   prints instead the directory holding the installed package's kernel files
#            (<kernel>.json, each kernel's ordered `controls`), which tools/face-conformance.sh
#            reads; when the package ships them only inside its omx-contract.json, they are cut
#            out of that into build/omx-contract-kernels/<version>/.
#
# Prints one line on stdout and nothing else; the reason for a refusal goes to stderr, exit 1.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SPEC="$HERE/packaging/omx-dsp.spec"
mode="${1:-}"
floor="$(sed -n 's/^BuildRequires: *omx-contract-devel *>= *\([0-9][0-9.]*\) *$/\1/p' "$SPEC")"
[ -n "$floor" ] || { echo "contract-include: $SPEC declares no 'BuildRequires: omx-contract-devel >= <floor>'" >&2; exit 1; }
if [ "$mode" = --floor ]; then printf '%s\n' "$floor"; exit 0; fi
if [ "$mode" != --data ] && [ -n "${OMX_CONTRACT_INC:-}" ]; then
  [ -f "$OMX_CONTRACT_INC/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: OMX_CONTRACT_INC=$OMX_CONTRACT_INC has no omxcontract/omx_contract_limits.h" >&2; exit 1; }
  printf '%s\n' "$OMX_CONTRACT_INC"; exit 0
fi
data=0
[ "$mode" = --data ] && data=1
need="omx-contract >= $floor (omx-contract-devel / libomx-contract-dev: dnf builddep packaging/omx-dsp.spec, apt-get build-dep ./)"
command -v pkg-config >/dev/null 2>&1 || { echo "contract-include: pkg-config is missing, so the installed omx-contract cannot be found; this tree needs $need" >&2; exit 1; }
if ! pkg-config --atleast-version="$floor" omx-contract 2>/dev/null; then
  have="$(pkg-config --modversion omx-contract 2>/dev/null || true)"
  if [ -n "$have" ]; then
    echo "contract-include: omx-contract $have is installed, below the floor $floor: install $need" >&2
  else
    echo "contract-include: omx-contract is not installed: install $need" >&2
  fi
  exit 1
fi
have="$(pkg-config --modversion omx-contract)"
prefix="$(pkg-config --variable=prefix omx-contract)"
if [ "$data" = 1 ]; then
  datadir="$(pkg-config --variable=datadir omx-contract 2>/dev/null || true)"
  [ -n "$datadir" ] || datadir="$prefix/share/omx-contract"
  for d in "$datadir/data/kernels" "$datadir/kernels" "$datadir/omx-contract/data/kernels"; do
    [ -d "$d" ] && { printf '%s\n' "$d"; exit 0; }
  done
  # The package ships the whole contract as one omx-contract.json: its kernels' controls are cut into
  # one <kernel>.json each, in the layout of the release's data/kernels/, under build/.
  if [ -f "$datadir/omx-contract.json" ]; then
    out="$HERE/build/omx-contract-kernels/$have"
    rm -rf "$out"; mkdir -p "$out"
    awk -v out="$out" '
      /^  "kernels": \{/ { inside = 1; next }
      inside && /^  \}/ { inside = 0 }
      inside && /^    "[A-Za-z0-9_]+": \{$/ { k = $0; sub(/^    "/, "", k); sub(/".*/, "", k) }
      inside && /^      "controls": \[/ { f = out "/" k ".json"; print "{" > f; print "  \"controls\": [" > f; on = 1; next }
      on && /^      \]/ { print "  ]" > f; print "}" > f; close(f); on = 0; next }
      on { print substr($0, 5) > f }
    ' "$datadir/omx-contract.json"
    [ -n "$(ls "$out")" ] || { echo "contract-include: $datadir/omx-contract.json of omx-contract $have holds no kernels' controls" >&2; exit 1; }
    printf '%s\n' "$out"; exit 0
  fi
  echo "contract-include: the installed omx-contract $have has no kernel files under $datadir (data/kernels, kernels or omx-contract.json): ls says: $(ls "$datadir" 2>&1 | tr '\n' ' ')" >&2
  exit 1
fi
inc="$(pkg-config --variable=includedir omx-contract)"
[ -f "$inc/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: omx-contract $have is installed but $inc/omxcontract/omx_contract_limits.h is missing" >&2; exit 1; }
printf '%s\n' "$inc"
