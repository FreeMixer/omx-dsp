#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# writable-data-check.sh — libomxdsp carries no mutable global or function-static state
# (docs/design/specs/2026-09-26-dsp-primitives.md §4.3 (a) and (b)).
#
#  (a) SYMBOLS. Every public header compiled into its own object (a translation unit that
#      includes it and defines one anchor function so the object is not empty), plus the one
#      compiled unit, at release flags: `nm` must list no symbol of type b B d D c C g G s S.
#      The same objects under -DOMX_CONTRACTS, with the storage macro set on the contract
#      header's own unit, must list exactly one such symbol: omx_contract_log.
#  (b) SOURCE. No file-scope or function-scope `static` object that is not `const`, and no
#      `_Thread_local`, under include/ and src/. A `static` FUNCTION carries `(`; a
#      `static const` table is allowed. Positive control: a planted `static float g_last;`
#      in a scratch copy must be caught.
#
# Usage: writable-data-check.sh <scratch-dir>   (CC and CFLAGS from the environment)
set -euo pipefail
shopt -s nullglob
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
mkdir -p "$OUT"
cd "$PKG"

fail=0
writable() { nm "$1" | awk '$2 ~ /^[bBdDcCgGsS]$/ { print $3 }'; }

# ---- (a) symbols -----------------------------------------------------------------------------
for h in include/omxdsp/*.h; do
  base="$(basename "$h" .h)"
  tu="$OUT/$base.c"
  printf '#include <omxdsp/%s.h>\nvoid omxdsp_wd_anchor_%s(void) {}\n' "$base" "$base" > "$tu"
  $CC $CFLAGS -Iinclude -c -o "$OUT/$base.rel.o" "$tu"
  w="$(writable "$OUT/$base.rel.o")"
  if [ -n "$w" ]; then echo "writable-data-check: FAIL $h (release) defines writable data: $w"; fail=1; fi
  storage=""; [ "$base" = omx_contract ] && storage=-DOMX_CONTRACT_STORAGE
  $CC $CFLAGS -Iinclude -DOMX_CONTRACTS $storage -c -o "$OUT/$base.con.o" "$tu"
  w="$(writable "$OUT/$base.con.o" | grep -v '^omx_contract_log$' || true)"
  if [ -n "$w" ]; then echo "writable-data-check: FAIL $h (contracts) defines writable data beyond the ledger: $w"; fail=1; fi
  if [ "$base" = omx_contract ] && [ "$(writable "$OUT/$base.con.o")" != omx_contract_log ]; then
    echo "writable-data-check: FAIL the contract unit must define exactly omx_contract_log"; fail=1
  fi
done
for c in src/*.c; do
  base="$(basename "$c" .c)"
  $CC $CFLAGS -Iinclude -c -o "$OUT/$base.src.rel.o" "$c"
  w="$(writable "$OUT/$base.src.rel.o")"
  if [ -n "$w" ]; then echo "writable-data-check: FAIL $c (release) defines writable data: $w"; fail=1; fi
  $CC $CFLAGS -Iinclude -DOMX_CONTRACTS -c -o "$OUT/$base.src.con.o" "$c"
  w="$(writable "$OUT/$base.src.con.o")"
  if [ -n "$w" ]; then echo "writable-data-check: FAIL $c (contracts) defines writable data: $w"; fail=1; fi
done
# the positive control for (a): a planted mutable static must be seen
printf 'static float g_last;\nfloat omxdsp_wd_probe(float x) { float p = g_last; g_last = x; return p; }\n' > "$OUT/probe.c"
$CC $CFLAGS -c -o "$OUT/probe.o" "$OUT/probe.c"
if [ -z "$(writable "$OUT/probe.o")" ]; then echo "writable-data-check: FAIL the symbol probe cannot see a planted static"; fail=1; fi

# ---- (b) source ------------------------------------------------------------------------------
scan() { # files…
  grep -nE '^[[:space:]]*static[[:space:]]' "$@" 2>/dev/null \
    | grep -vE 'static[[:space:]]+(inline|const)[[:space:]]' \
    | grep -vE '\(' || true
  grep -nE '_Thread_local' "$@" 2>/dev/null || true
}
hits="$(scan include/omxdsp/*.h src/*.c)"
if [ -n "$hits" ]; then echo "writable-data-check: FAIL mutable static or thread-local state in the library:"; echo "$hits"; fail=1; fi
printf 'static float g_last;\n' > "$OUT/probe_src.h"
if [ -z "$(scan "$OUT/probe_src.h")" ]; then echo "writable-data-check: FAIL the source scan cannot see a planted static"; fail=1; fi
printf 'static const float k_tab[2] = {1.0f, 2.0f};\nstatic inline float omx_x(float a) { return a; }\nstatic void helper(void) {}\n' > "$OUT/probe_ok.h"
if [ -n "$(scan "$OUT/probe_ok.h")" ]; then echo "writable-data-check: FAIL the source scan refuses a const table, an inline or a static function"; fail=1; fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "writable-data-check: no writable data in the library (release: none; contracts: the ledger only); source scan clean"
