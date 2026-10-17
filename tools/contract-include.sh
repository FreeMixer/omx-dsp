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
#
# Prints one line on stdout and nothing else; the reason for a refusal goes to stderr, exit 1.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SPEC="$HERE/packaging/omx-dsp.spec"
mode="${1:-}"
floor="$(sed -n 's/^BuildRequires: *omx-contract-devel *>= *\([0-9][0-9.]*\) *$/\1/p' "$SPEC")"
[ -n "$floor" ] || { echo "contract-include: $SPEC declares no 'BuildRequires: omx-contract-devel >= <floor>'" >&2; exit 1; }
if [ "$mode" = --floor ]; then printf '%s\n' "$floor"; exit 0; fi
if [ -n "${OMX_CONTRACT_INC:-}" ]; then
  [ -f "$OMX_CONTRACT_INC/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: OMX_CONTRACT_INC=$OMX_CONTRACT_INC has no omxcontract/omx_contract_limits.h" >&2; exit 1; }
  printf '%s\n' "$OMX_CONTRACT_INC"; exit 0
fi
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
inc="$(pkg-config --variable=includedir omx-contract)"
[ -f "$inc/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: omx-contract $have is installed but $inc/omxcontract/omx_contract_limits.h is missing" >&2; exit 1; }
printf '%s\n' "$inc"
