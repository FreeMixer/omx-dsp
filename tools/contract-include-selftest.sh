#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-include-selftest.sh — holds tools/contract-include.sh to its range (FreeMixer/omx-dsp#95).
# It renders a scratch omx-contract package at several versions and runs the script against each:
# the floor, a later minor and patch, and the last 2.x are taken; one below the floor, 3.0.0, and no
# package at all are refused, the refusal naming the package. No network is used: a refusal that
# fetched anything would be a download path back.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
S="$(mktemp -d)"; trap 'rm -rf "$S"' EXIT
mkdir -p "$S/lib/pkgconfig" "$S/include/omxcontract" "$S/share/omx-contract/data/kernels"
: > "$S/include/omxcontract/omx_contract_limits.h"
echo '{}' > "$S/share/omx-contract/data/kernels/gate.json"
render() {
  printf 'prefix=%s\nincludedir=${prefix}/include\nName: omx-contract\nDescription: scratch\nVersion: %s\nCflags: -I${includedir}\n' \
    "$S" "$1" > "$S/lib/pkgconfig/omx-contract.pc"
}
run() { env -u OMX_CONTRACT_INC PKG_CONFIG_PATH="$S/lib/pkgconfig" PKG_CONFIG_LIBDIR="$S/lib/pkgconfig" \
        http_proxy=http://127.0.0.1:9 https_proxy=http://127.0.0.1:9 sh "$HERE/tools/contract-include.sh" "$@"; }
bad=0
fail() { echo "contract-include-selftest: FAIL — $*" >&2; bad=1; }
for v in 2.6.0 2.6.1 2.10.1 2.99.9; do
  render "$v"
  out="$(run 2>&1)" || { fail "$v is in range and was refused: $out"; continue; }
  [ "$out" = "$S/include" ] || fail "$v printed '$out', not $S/include"
  out="$(run --data 2>&1)" || { fail "--data refused at $v: $out"; continue; }
  [ "$out" = "$S/share/omx-contract/data/kernels" ] || fail "--data at $v printed '$out'"
done
for v in 2.5.9 2.0.0 3.0.0 3.1.0; do
  render "$v"
  if out="$(run 2>&1)"; then fail "$v is out of range and was taken: $out"; continue; fi
  case "$out" in *omx-contract-devel*libomx-contract-dev*) ;; *) fail "the refusal at $v does not name the package: $out" ;; esac
  case "$out" in *"$v"*) ;; *) fail "the refusal at $v does not name the installed version: $out" ;; esac
done
rm -f "$S/lib/pkgconfig/omx-contract.pc"
if out="$(run 2>&1)"; then fail "no package installed and the script answered: $out"
else case "$out" in *omx-contract-devel*libomx-contract-dev*) ;; *) fail "the refusal with nothing installed does not name the package: $out" ;; esac; fi
# the removed paths stay removed
if grep -nE 'curl|codeload|release\.tgz|exact-version|\{ print \$4 \}' "$HERE/tools/contract-include.sh" | grep -v '^[0-9]*:#' ; then
  fail "tools/contract-include.sh carries a download or exact-version path again"
fi
[ "$bad" = 0 ] || exit 1
echo "contract-include-selftest: the floor and later 2.x are taken; below the floor, 3.x and nothing are refused, naming the package; no download path"
