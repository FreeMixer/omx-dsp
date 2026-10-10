#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# strip-input-identity.sh — the trim face then the eq face (bands off) give the same bits as
# omx-plugins' channel strip's input stage, read from the plugin's own plugins/omx-strip/omx_strip.h.
#
#   tools/strip-input-identity.sh <omx-plugins checkout>              the check
#   tools/strip-input-identity.sh --write <omx-plugins checkout>      print the strip's digests
#                                                                     (test/golden/trim_instance.sha256)
#   tools/strip-input-identity.sh --self-test <omx-plugins checkout>  a sabotaged strip trim must go red
#
# tools/strip-input-identity.c includes omx_strip.h (and its generated parameter table) from the
# checkout and this tree's include/, and renders the golden stimulus through it over
# test/fx/strip_input_script.h with the gate, the EQ and the compressor off; its digests are compared
# with test/golden/trim_instance.sha256, the file test/fx/trim_instance_golden.test.c holds the faces
# to. A desk check (the strip is another repository), not part of make test, like engine-identity.
set -euo pipefail
: "${CONTRACT_INC:=$(sh "$(dirname "$0")/contract-include.sh")}"
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
mode=check
case "${1:-}" in --write) mode=write; shift ;; --self-test) mode=self; shift ;; esac
[ -f build/libomxdsp.a ] || { echo "strip-input-identity: build/libomxdsp.a is missing: make lib first" >&2; exit 2; }
[ $# -eq 1 ] || { echo "usage: $0 [--write|--self-test] <omx-plugins checkout>" >&2; exit 2; }
strip="$(cd "$1" && pwd)/plugins/omx-strip"
[ -f "$strip/omx_strip.h" ] && [ -f "$strip/generated/omx_strip_params.h" ] \
  || { echo "strip-input-identity: $1 has no plugins/omx-strip/omx_strip.h and generated table" >&2; exit 2; }
out="build/strip-input-identity"
rm -rf "$out" && mkdir -p "$out"

build() { # $1 = directory holding omx_strip.h, $2 = binary
  $CC $CFLAGS -Iinclude -isystem "$CONTRACT_INC" -I"$1" -I"$strip/generated" -I"$(cd "$strip/../.." && pwd)/include" \
    -Itest/fx -include test/support/log10f_subst.h -o "$2" tools/strip-input-identity.c build/libomxdsp.a -lm
}

build "$strip" "$out/strip"
case "$mode" in
write) exec "$out/strip" --write ;;
check)
  "$out/strip" test/golden/trim_instance.sha256 && echo "strip-input-identity: omx_strip.h's input stage is the trim and eq faces, bit for bit" ;;
self)
  mkdir -p "$out/sabotaged"
  cp "$strip/omx_strip.h" "$out/sabotaged/"
  # the strip's trim one step (0.1 dB) off: the faces' digests must no longer be the strip's
  sed -i 's/s->trim_tgt = omx_db_to_lin(\(.*\));$/s->trim_tgt = omx_db_to_lin(\1 + 0.1f);/' "$out/sabotaged/omx_strip.h"
  ! cmp -s "$strip/omx_strip.h" "$out/sabotaged/omx_strip.h" || { echo "strip-input-identity self-test: the sabotage edited nothing" >&2; exit 1; }
  build "$out/sabotaged" "$out/strip-sabotaged"
  if "$out/strip-sabotaged" test/golden/trim_instance.sha256 >/dev/null 2>&1; then
    echo "strip-input-identity self-test: a strip trim 0.1 dB off still matched the faces" >&2; exit 1
  fi
  echo "strip-input-identity self-test: a strip trim 0.1 dB off moves the digests" ;;
esac
