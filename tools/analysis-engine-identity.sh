#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# analysis-engine-identity.sh — the RTA ring and spectrum, the HRP render and the HRP live core give
# the same bits as the engine's own copies they moved from: the engine's
# mix_dsp.h, hrp_render.h and hrp_lv2.h.
#
#   tools/analysis-engine-identity.sh <openmixer checkout>              the check
#   tools/analysis-engine-identity.sh --write <openmixer checkout>      print the engine's digests
#                                                                       (test/golden/analysis.sha256)
#   tools/analysis-engine-identity.sh --self-test <openmixer checkout>  a sabotaged engine copy must go red
#
# test/analysis/golden.test.c is compiled with <omxdsp/analysis/omx_rta.h>, omx_hrp_render.h and
# omx_hrp_lv2.h answered by the engine's headers, and its digests are compared with
# test/golden/analysis.sha256, the file make test-analysis holds this tree's headers to. The other
# analysis headers the engine already reads from omx-dsp. A desk check (the engine is another
# repository), not part of make test; it has nothing to compare once the engine drops its copies.
set -euo pipefail
: "${CONTRACT_INC:=$(sh "$(dirname "$0")/contract-include.sh")}"
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
mode=check
case "${1:-}" in --write) mode=write; shift ;; --self-test) mode=self; shift ;; esac
[ -f build/libomxdsp.a ] || { echo "analysis-engine-identity: build/libomxdsp.a is missing: make lib first" >&2; exit 2; }
[ $# -eq 1 ] || { echo "usage: $0 [--write|--self-test] <openmixer checkout>" >&2; exit 2; }
engine="$(cd "$1" && pwd)/packages/pipewire-native/src"
for h in mix_dsp.h hrp_render.h hrp_lv2.h; do
  [ -f "$engine/$h" ] || { echo "analysis-engine-identity: $engine has no $h" >&2; exit 2; }
done
out="build/analysis-engine-identity"
rm -rf "$out" && mkdir -p "$out/shim/omxdsp/analysis"
printf '#include "mix_dsp.h"\n' > "$out/shim/omxdsp/analysis/omx_rta.h"
printf '#include "hrp_render.h"\n' > "$out/shim/omxdsp/analysis/omx_hrp_render.h"
printf '#include "hrp_lv2.h"\n' > "$out/shim/omxdsp/analysis/omx_hrp_lv2.h"

build() { # $1 = directory holding hrp_render.h and hrp_lv2.h, $2 = binary
  $CC $CFLAGS -I"$out/shim" -I"$1" -I"$engine" -Iinclude -isystem "$CONTRACT_INC" -Itest/analysis -Itest/fx \
    -Wl,--wrap=log10f -o "$2" test/analysis/golden.test.c build/libomxdsp.a -lm
}

build "$engine" "$out/golden"
case "$mode" in
write) exec "$out/golden" --write ;;
check)
  "$out/golden" test/golden/analysis.sha256 \
    && echo "analysis-engine-identity: the engine's RTA, HRP render and HRP live core give the golden digests, bit for bit" ;;
self)
  mkdir -p "$out/sabotaged"
  cp "$engine/hrp_render.h" "$engine/hrp_lv2.h" "$out/sabotaged/"
  # the live core's ramp a tenth of a dB faster: the engine's digests must no longer be the golden ones
  sed -i 's/  const float step = OMX_HRP_GAIN_STEP_DB;$/  const float step = OMX_HRP_GAIN_STEP_DB + 0.1f;/' "$out/sabotaged/hrp_lv2.h"
  ! cmp -s "$engine/hrp_lv2.h" "$out/sabotaged/hrp_lv2.h" || { echo "analysis-engine-identity self-test: the sabotage edited nothing" >&2; exit 1; }
  build "$out/sabotaged" "$out/golden-sabotaged"
  if "$out/golden-sabotaged" test/golden/analysis.sha256 >/dev/null 2>&1; then
    echo "analysis-engine-identity self-test: a live ramp 0.1 dB faster still matched the golden digests" >&2; exit 1
  fi
  echo "analysis-engine-identity self-test: a live ramp 0.1 dB faster moves the digests" ;;
esac
