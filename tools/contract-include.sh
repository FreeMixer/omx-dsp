#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-include.sh — prints the directory whose omxcontract/ holds the omx-contract C header
# this tree builds against. The version is the one .github/pins.txt names (the omx-contract line,
# third field); the header is never committed here (FreeMixer/omx-dsp#1, #9).
#
#   1. $OMX_CONTRACT_INC, when set: a scratch render, used by the perturbation arms. Not
#      version-checked: it is a deliberate other contract.
#   2. the installed omx-contract-devel / libomx-contract-dev, when pkg-config finds exactly that
#      version.
#   3. the release tarball of that version (its include/ is the render `omx-contract render --check`
#      holds equal to the data), fetched once into build/omx-contract/<version>/.
#
# Prints one line on stdout and nothing else; the reason for a refusal goes to stderr, exit 1.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${OMX_CONTRACT_INC:-}" ]; then
  [ -f "$OMX_CONTRACT_INC/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: OMX_CONTRACT_INC=$OMX_CONTRACT_INC has no omxcontract/omx_contract_limits.h" >&2; exit 1; }
  printf '%s\n' "$OMX_CONTRACT_INC"; exit 0
fi
v="$(awk '$1 == "omx-contract" { print $3 }' "$HERE/.github/pins.txt")"
[ -n "$v" ] || { echo "contract-include: .github/pins.txt has no omx-contract line" >&2; exit 1; }
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exact-version="$v" omx-contract 2>/dev/null; then
  inc="$(pkg-config --variable=includedir omx-contract)"
  [ -f "$inc/omxcontract/omx_contract_limits.h" ] || { echo "contract-include: omx-contract $v is installed but $inc/omxcontract/omx_contract_limits.h is missing" >&2; exit 1; }
  printf '%s\n' "$inc"; exit 0
fi
dir="$HERE/build/omx-contract/$v"
if [ ! -f "$dir/package/include/omxcontract/omx_contract_limits.h" ]; then
  command -v curl >/dev/null 2>&1 || { echo "contract-include: omx-contract $v is not installed (pkg-config) and curl is missing to fetch the release" >&2; exit 1; }
  rm -rf "$dir"; mkdir -p "$dir"
  curl -fsSL "https://github.com/FreeMixer/omx-contract/releases/download/v$v/openmixer-omx-contract-$v.tgz" -o "$dir/release.tgz" \
    || { echo "contract-include: omx-contract v$v could not be fetched" >&2; rm -rf "$dir"; exit 1; }
  tar xzf "$dir/release.tgz" -C "$dir"
fi
got="$(sed -n 's/^  "version": "\(.*\)",$/\1/p' "$dir/package/package.json")"
[ "$got" = "$v" ] || { echo "contract-include: the release unpacked at $dir is $got, the pin is $v" >&2; exit 1; }
printf '%s\n' "$dir/package/include"
