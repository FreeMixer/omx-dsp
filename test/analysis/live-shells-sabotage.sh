#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# live-shells-sabotage.sh <dir> — one sabotage per arm of test/analysis/live_shells.test.c: a SABOTAGED COPY of
# omx_fbs_lv2.h or omx_hrp_lv2.h is placed ahead of the real one, the test is built against it, and it must fail
# on the check that arm makes. Then the real-time arm: a copy of the tree whose run() calls the worker's pass, and
# omx-rt-gate's self-run over it must read it red by its thread tags. The tree is never edited, and a sabotage
# that matches nothing is a failure, not a skip.
#
#   fbs-report-cascade  report still runs the cascade
#   fbs-analysing       a shell that never had a worker says it is analysing
#   fbs-mode-default    a non-finite mode word reads as report
#   fbs-compact         the report is written in the cascade's order, not by slot
#   fbs-clock           the placement's clock is the frames written (the host's block), not the frames consumed
#   fbs-adopt           adopt never installs the coefficients
#   hrp-report-cascade  report still runs the cascade
#   hrp-compact         the report is written in the cascade's order, not by slot
#   hrp-analysing       adopt never says the shell is analysing
#   hrp-q               the set carries no Q
#   rt-fbs, rt-hrp      run() calls the worker's analysis
#
# CC, CFLAGS and CONTRACT_INC from the environment (make exports CONTRACT_INC).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
OUT="$(mkdir -p "${1:?scratch dir}" && cd "$1" && pwd)"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:?CONTRACT_INC is not set}"
cd "$PKG"
LIB=build/libomxdsp.a
[ -f "$LIB" ] || { echo "live-shells-sabotage.sh: no $LIB (make lib)"; exit 1; }
rm -rf "${OUT:?}"/* 

# edit <file> <sed script>: GNU sed, extended expressions, over the whole file as one buffer (-z), so a
# pattern may span lines; the C sources hold no NUL byte.
edit() { sed -z -E -i "$2" "$1"; }

# sabotage <arm> <header> <check text the arm must fail> <sed script>
sabotage() {
  local arm="$1" hdr="include/omxdsp/analysis/$2" want="$3" dir="$OUT/$1"
  mkdir -p "$dir/omxdsp/analysis"
  local copy="$dir/omxdsp/analysis/$2"
  cp "$hdr" "$copy"
  edit "$copy" "$4"
  if cmp -s "$hdr" "$copy"; then
    echo "live-shells-sabotage.sh: FAIL — $arm: the code the sabotage targets in $hdr is gone; it would test nothing"
    exit 1
  fi
  $CC $CFLAGS -I"$dir" -Iinclude -isystem "$CONTRACT_INC" -Itest/analysis -o "$dir/oracle" test/analysis/live_shells.test.c "$LIB" -lm
  if "$dir/oracle" > "$dir/log" 2>&1; then
    echo "live-shells-sabotage.sh: FAIL — $arm: the shells pass over a sabotaged $hdr"
    exit 1
  fi
  if ! grep -qF "$want" "$dir/log"; then
    echo "live-shells-sabotage.sh: FAIL — $arm: failed, but not on '$want'"
    cat "$dir/log"
    exit 1
  fi
  echo "live-shells-sabotage.sh: $arm: '$want' fails"
}

F=omx_fbs_lv2.h
H=omx_hrp_lv2.h
sabotage fbs-report-cascade $F 'omx-fbs in report passes the audio bit-exact' 's/omx_live_out\(&s->live, s->mode == OMX_FBS_LV2_REPORT,/omx_live_out(\&s->live, 0,/'
sabotage fbs-analysing $F 'omx-fbs reports analysing 0' 's/(  s->mode = OMX_FBS_MODES_DEFAULT;)/\1\n  s->analysing = 1;/'
sabotage fbs-mode-default $F 'omx-fbs reads the mode word back as declared' 's/OMX_FBS_MODES_COUNT - 1, OMX_FBS_MODES_DEFAULT\)/OMX_FBS_MODES_COUNT - 1, OMX_FBS_MODES_REPORT)/'
sabotage fbs-compact $F 'omx-fbs keeps the second ring in slot 1' 's/    out->report\[k\] = r;\n    if \(!\(r\.on > 0\.5f\)\) continue;/    { static uint32_t nk; if (k == 0u) nk = 0u; if (!(r.on > 0.5f)) continue; out->report[nk++] = r; }/'
sabotage fbs-clock $F 'the timeline is the same bit for bit at every quantum' 's/\(double\)s->elapsed \* 1000\.0/(double)atomic_load_explicit(\&s->feed.written, memory_order_acquire) * 1000.0/'
sabotage fbs-adopt $F 'the cut measured on the audio at 1 kHz is the reported depth' 's/    memcpy\(s->live\.coeffs\[k\], set->coeffs\[k\], 5u \* sizeof\(float\)\);\n//'
sabotage hrp-report-cascade $H 'omx-hrp in report passes the audio bit-exact' 's/omx_live_out\(&s->live, s->mode == OMX_HRP_LV2_REPORT,/omx_live_out(\&s->live, 0,/'
sabotage hrp-compact $H "omx-hrp keeps slot 1's six words in slot 1" 's/out->report\[i\]\.([a-z_]+) =/out->report[b].\1 =/g'
sabotage hrp-analysing $H 'omx-hrp reports analysing once a pass is adopted' 's/  s->analysing = 1;\n//'
sabotage hrp-q $H "omx-hrp's set carries the band's Q" 's/out->q\[b\] = \(float\)OMX_HRP_BAND_Q;/out->q[b] = 0.0f;/'

# the real-time arm: run() reaching the worker's pass is read red by the thread tags (omx-rt-gate's self-run)
rt() {
  local arm="$1" hdr="include/omxdsp/analysis/$2" want="$4" tree="$OUT/$1/tree"
  mkdir -p "$tree"
  git ls-files -z --cached --others --exclude-standard | xargs -0 tar -cf - | tar -xf - -C "$tree"
  cp "$hdr" "$tree/$hdr"
  edit "$tree/$hdr" "$3"
  if cmp -s "$hdr" "$tree/$hdr"; then
    echo "live-shells-sabotage.sh: FAIL — $arm: the code the sabotage targets in $hdr is gone"
    exit 1
  fi
  if OMX_CONTRACT_INC="$CONTRACT_INC" make -C "$tree" --no-print-directory rt-gate-selfrun-plain > "$OUT/$1/log" 2>&1; then
    echo "live-shells-sabotage.sh: FAIL — $arm: the rt-gate self-run passes a run() that calls the worker's pass"
    exit 1
  fi
  if ! grep -q "RT-THREAD $want" "$tree/build/rt-gate/selfrun-plain.out" "$OUT/$1/log" 2>/dev/null; then
    echo "live-shells-sabotage.sh: FAIL — $arm: the self-run failed, but not with RT-THREAD $want"
    tail -20 "$OUT/$1/log"
    exit 1
  fi
  echo "live-shells-sabotage.sh: $arm: the self-run reads RT-THREAD $want"
}
rt rt-fbs $F 's#(/\*\*\n \* @brief The audio callback.s whole share)#/** @thread worker */\nstatic inline void omx_fbs_lv2_answer(OmxFbsLv2 *s, const OmxFbsLv2Ask *a, OmxFbsLv2Set *out);\n\1#; s#(  omx_live_out\(&s->live, s->mode == OMX_FBS_LV2_REPORT, in, out, n\);)#  { static OmxFbsLv2Set rt_sabotage; omx_fbs_lv2_answer(s, NULL, \&rt_sabotage); }\n\1#' omx_fbs_lv2_answer
rt rt-hrp $H 's#(/\*\*\n \* THE AUDIO CALLBACK.S WHOLE SHARE OF HRP)#/** @thread worker */\nstatic inline void omx_hrp_lv2_analyse(OmxHrpLv2 *s, int bypass, OmxHrpLv2Set *out);\n\1#; s#(  omx_live_out\(&s->live, s->mode == OMX_HRP_LV2_REPORT, in, out, n\);)#  { static OmxHrpLv2Set rt_sabotage; omx_hrp_lv2_analyse(s, 0, \&rt_sabotage); }\n\1#' omx_hrp_lv2_analyse
echo "live-shells-sabotage.sh: every arm fails its check"
