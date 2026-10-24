#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# definitions-render.sh — this library's own C header, rendered from its definitions (data/) by omx-contract's tool:
# the defines of every item omx-dsp owns, the same names and bytes omx-contract's header carried for them.
#
#   tools/definitions-render.sh [--check] <omx-contract checkout> <build dir>
#
# data/ is installed as omx-dsp beside the checkout's own data/ as omx-contract (the way `make install` installs it),
# and `omx-contract render --target c --package omx-dsp` renders omx-dsp's items alone into include/: the header
# its files' render word names. --check writes nothing and fails when the committed header differs from the render;
# without it the header is written. A release build never runs this (R-056: no node): it ships the committed header.
# Exit 0 rendered or equal; 1 the committed header differs; 2 usage or environment (no node, no checkout, a tool
# that cannot render one package), never a pass.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
check=''
[ "${1-}" = --check ] && { check=--check; shift; }
[ "$#" -eq 2 ] || { sed -n '8p' "$0" >&2; exit 2; }
OC="$(cd "$1" 2>/dev/null && pwd)" || { echo "definitions-render: no omx-contract checkout at $1" >&2; exit 2; }
out="$(mkdir -p "$2" && cd "$2" && pwd)"
command -v node >/dev/null 2>&1 || { echo "definitions-render: node is missing" >&2; exit 2; }
[ -f "$OC/bin/omx-contract.mjs" ] && [ -f "$OC/tools/install-contract-dir.sh" ] && [ -d "$OC/node_modules" ] || { echo "definitions-render: $OC has no bin/omx-contract.mjs, tools/install-contract-dir.sh or node_modules (npm ci there)" >&2; exit 2; }
grep -qE 'render --target c\|ts\|json(\|[a-z0-9]+)* .*--package' "$OC/bin/omx-contract.mjs" || { echo "definitions-render: the tool at $OC renders no single package (render --package): it needs omx-contract 2.15.0's tool" >&2; exit 2; }
version="$(make -s -C "$HERE" version)"
major="$(sed -n 's/^  "x-omx-schema-version": "\([0-9]*\)\..*",$/\1/p' "$OC/schema/omx-contract.schema.json")"
ocversion="$(sed -n 's/^  "version": "\(.*\)",$/\1/p' "$OC/package.json")"
[ -n "$major" ] && [ -n "$ocversion" ] || { echo "definitions-render: $OC states no schema major or version" >&2; exit 2; }
dir="$out/dir"
rm -rf "$dir"; mkdir -p "$dir"
sh "$OC/tools/install-contract-dir.sh" -x codes.json omx-contract "$ocversion" "$major" "$OC/data" "$dir" data >/dev/null
sh "$OC/tools/install-contract-dir.sh" omx-dsp "$version" "$major" "$HERE/data" "$dir" data >/dev/null
header=omxdsp/omxdsp_definitions.h
[ -z "$check" ] || [ -f "$HERE/include/$header" ] || { echo "definitions-render: no committed include/$header to check" >&2; exit 2; }
rc=0
got="$(cd "$OC" && node bin/omx-contract.mjs render --target c --package omx-dsp --contract-dir "$dir" --out "$HERE/include" $check 2>&1)" || rc=$?
printf '%s\n' "$got"
[ "$rc" = 0 ] || exit "$rc"
# a render of no file is no render: the files' render word must name the header
grep -Eq ' [1-9][0-9]* files? ' <<<"$got" && [ -f "$HERE/include/$header" ] || { echo "definitions-render: the render wrote no include/$header (no render word in data/ names it)" >&2; exit 2; }
