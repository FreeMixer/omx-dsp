#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# note-map-sabotage.sh <dir> — the note map's suite arm, proven able to SEE each defect it guards.
# Each sabotage is applied to a COPY of omx_note_map.h placed ahead of the real include directory;
# the suite is built against it and must go red on its note_map arm:
#   soft, hard, fixed — a curve off its closed form (fixed: one below velocityFixed);
#   vel0      — a velocity-0 note on no longer read as a note off;
#   split     — the split's upper edge made exclusive (59 lost from the bass leg);
#   held      — a note off sent to the key transposed NOW, not the key its note on sounded;
#   retrigger — a retrigger no longer releases the key it held;
#   channel   — the channel filter off by one;
#   omni      — omni no longer passes every channel;
#   wrap      — an out-of-range key wrapped into 0..127 instead of dropped;
#   count     — an out-of-range drop not counted;
#   release   — release-all lists the held notes but does not forget them.
# The tree is never edited. A sed that matches nothing is a failure, not a skip, and a red that
# printed no FAIL line of the note_map arm (a compile error, a crash) is not a red arm.
# Usage: CC=… CFLAGS=… note-map-sabotage.sh <scratch-dir>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"
# the arm list into the scratch dir, never build/: a parallel make builds the suite from build/'s
bash "$PKG/tools/kernels-gen.sh" "$OUT" >/dev/null

sabotage() { # name sed-expression
  local name=$1 expr=$2 d="$OUT/$1" rc=0
  mkdir -p "$d/omxdsp"
  cp "$PKG"/include/omxdsp/*.h "$d/omxdsp/"
  sed -i "$expr" "$d/omxdsp/omx_note_map.h"
  if cmp -s "$PKG/include/omxdsp/omx_note_map.h" "$d/omxdsp/omx_note_map.h"; then
    echo "note-map-sabotage.sh: FAIL — sabotage '$name' changed nothing; the line it targets is gone"
    return 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a word list
  $CC $CFLAGS -ffp-contract=off -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -DOMX_CONTRACTS \
    -I"$OUT" -o "$d/suite" "$PKG/test/omxdsp_suite.c" "$PKG"/src/*.c -lm
  "$d/suite" > "$d/out" 2>&1 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "note-map-sabotage.sh: FAIL — sabotage '$name' left the suite GREEN; the arm cannot see the defect"
    return 1
  fi
  local n
  n=$(grep -c '^FAIL \[note_map\]' "$d/out" || true)
  if [ "$n" -eq 0 ]; then
    echo "note-map-sabotage.sh: FAIL — sabotage '$name' went red (rc=$rc) with no FAIL line of the note_map arm"
    tail -5 "$d/out"
    return 1
  fi
  echo "note-map-sabotage.sh: sabotage '$name': the note_map arm went red on $n checks"
}

sabotage soft 's|out = omx_note_isqrt_round(127 \* v);|out = omx_note_isqrt_round(126 * v);|'
sabotage hard 's|out = (2 \* v \* v + 127) / 254;|out = (2 * v * v) / 254;|'
sabotage fixed 's|case OMX_VELOCITY_FIXED: out = fixed;|case OMX_VELOCITY_FIXED: out = fixed > 1 ? fixed - 1 : fixed;|'
sabotage vel0 's|if (kind == 0x80 \|\| (kind == 0x90 \&\& in.data2 == 0u)) {|if (kind == 0x80) {|'
sabotage split 's|key > m->leg.key_high) return 0;|key >= m->leg.key_high) return 0;|'
sabotage held 's|out\[n\].data1 = m->out_key\[ch\]\[key\];|out[n].data1 = (uint8_t)(key + m->leg.transpose);|'
sabotage retrigger 's|    if (omx_note_map_is_held(m, ch, key)) {|    if (0) {|'
sabotage channel 's|m->leg.channel == ch + 1;|m->leg.channel == ch;|'
sabotage omni 's|return m->leg.channel == OMX_NOTE_CHANNEL_OMNI \|\| |return |'
sabotage wrap 's|if (to < 0 \|\| to > 127) {|if (0) {|'
sabotage count 's|      m->out_of_range++;|      (void)0;|'
sabotage release 's|      omx_note_map_mark(m, c, k, 0);|      (void)0;|'
echo "note-map-sabotage.sh: the note_map arm sees all twelve sabotages"
