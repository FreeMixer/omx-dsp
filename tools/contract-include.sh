#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-include.sh — prints the directory whose omxcontract/ holds the omx-contract C header
# this tree builds against (FreeMixer/omx-dsp#1, #9, #95). The header is never committed here.
#
#   1. $OMX_CONTRACT_INC, when set: a scratch render, used by the perturbation arms. Not
#      version-checked: it is a deliberate other contract.
#   2. the installed omx-contract-devel / libomx-contract-dev, when pkg-config finds a version
#      inside the range .github/pins.txt names, as the spec and debian/control declare it (>= 2.6.0, < 3).
#
# Nothing is downloaded: no commit, no release tarball. When no installed omx-contract is in range
# the script stops and names the package.
#
# With --data it prints instead the directory holding the installed package's kernel files
# (<kernel>.json, each kernel's ordered `controls`), which tools/face-conformance.sh reads.
#
# Prints one line on stdout and nothing else; the reason for a refusal goes to stderr, exit 1.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
data=0
[ "${1:-}" = --data ] && data=1
if [ "$data" = 0 ] && [ -n "${OMX_CONTRACT_INC:-}" ]; then
  [ -f "$OMX_CONTRACT_INC/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: OMX_CONTRACT_INC=$OMX_CONTRACT_INC has no omxcontract/omx_contract_limits.h" >&2; exit 1; }
  printf '%s\n' "$OMX_CONTRACT_INC"; exit 0
fi
# The range .github/pins.txt names, the one packaging/omx-dsp.spec and debian/control declare.
floor="$(awk '$1 == "omx-contract" && $2 == ">=" { print $3 }' "$HERE/.github/pins.txt")"
ceiling="$(awk '$1 == "omx-contract" && $4 == "<" { print $5 }' "$HERE/.github/pins.txt")"
[ -n "$floor" ] && [ -n "$ceiling" ] || { echo "contract-include: .github/pins.txt has no 'omx-contract >= <floor> < <ceiling>' line" >&2; exit 1; }
need="omx-contract-devel / libomx-contract-dev (>= $floor, < $ceiling)"
command -v pkg-config >/dev/null 2>&1 || { echo "contract-include: pkg-config is missing, so the installed omx-contract cannot be found: install $need" >&2; exit 1; }
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
case "$have" in
  "$ceiling".*|"$ceiling") echo "contract-include: omx-contract $have is installed, at or above the ceiling $ceiling: install $need" >&2; exit 1 ;;
esac
prefix="$(pkg-config --variable=prefix omx-contract)"
if [ "$data" = 1 ]; then
  datadir="$(pkg-config --variable=datadir omx-contract 2>/dev/null || true)"
  [ -n "$datadir" ] || datadir="$prefix/share"
  for d in "$datadir/omx-contract/data/kernels" "$datadir/omx-contract/kernels"; do
    [ -d "$d" ] && { printf '%s\n' "$d"; exit 0; }
  done
  echo "contract-include: the installed omx-contract $have ships no kernel files under $datadir/omx-contract (data/kernels or kernels)" >&2
  exit 1
fi
inc="$(pkg-config --variable=includedir omx-contract)"
[ -f "$inc/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: omx-contract $have is installed but $inc/omxcontract/omx_contract_limits.h is missing" >&2; exit 1; }
printf '%s\n' "$inc"
