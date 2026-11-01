#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# clipper_instance-perturb.sh — test/fx/clipper_instance.test.c sees each moved declaration of the clipper face and each
# defect of its word. Each sabotage is applied to a COPY of the include directory placed ahead of the real
# one, and the oracle built against it must go red in the named arm:
#   span — CLIPPER_KNEE_SPAN moved from 0.5 to 0.4: arm C;
#   hard — the straight line ends early: arm B;
#   bend — the hyperbola at the wrong rate: arm C;
#   sign — the negative side loses its sign: arm B C;
#   drive — the drive is not applied: arm B;
#   readout — the readout reads half: arm D;
#   bypass — the bypass is ignored: arm A;
# The tree is never edited; a sed that matches nothing is a failure, not a skip.
# Usage: CC=… CFLAGS=… clipper_instance-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT"
pids=()

sabotage() { # <name> <header under include/omxdsp> <sed expression> <arm>...
  local name="$1" hdr="$2" expr="$3" d="$OUT/$1"
  shift 3
  mkdir -p "$d"
  cp -r "$PKG/include/omxdsp" "$d/"
  sed -i "$expr" "$d/omxdsp/$hdr"
  if cmp -s "$PKG/include/omxdsp/$hdr" "$d/omxdsp/$hdr"; then
    echo "clipper_instance-perturb: FAIL — sabotage '$name' matched nothing in $hdr"; exit 1
  fi
  # shellcheck disable=SC2086 # CFLAGS is a flag list
  $CC -I"$d" -I"$PKG/include" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread \
    -o "$d/oracle" "$PKG/test/fx/clipper_instance.test.c" "$PKG"/src/*.c -lm \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
  if "$d/oracle" >"$d/oracle.log" 2>&1; then echo "clipper_instance-perturb: FAIL — sabotage '$name' left the oracle green"; exit 1; fi
  local arm n
  for arm in "$@"; do
    n=$(grep -c "^FAIL \[$arm @" "$d/oracle.log" || true)
    [ "$n" -gt 0 ] || { echo "clipper_instance-perturb: FAIL — sabotage '$name' did not turn arm $arm red"; head -5 "$d/oracle.log"; exit 1; }
    echo "clipper_instance-perturb: sabotage '$name' turned arm $arm red ($n checks)"
  done
}

sabotage span omxdsp_definitions.h 's|^#define OMX_CLIPPER_KNEE_SPAN 0.5f$|#define OMX_CLIPPER_KNEE_SPAN 0.4f|' C &
pids+=($!)
sabotage hard fx/omx_clipper_instance.h 's|  if (m <= a) return u;|  if (m <= a * 0.9f) return u;|' B &
pids+=($!)
sabotage bend fx/omx_clipper_instance.h 's|a + room \* d / (d + room) : c;|a + room * d / (d + 2.0f * room) : c;|' C &
pids+=($!)
sabotage sign fx/omx_clipper_instance.h 's|  return u < 0.0f ? -y : y;|  return y;|' B C &
pids+=($!)
sabotage drive fx/omx_clipper_instance.h 's|  const float g = s->drive_lin, c = s->ceiling_lin|  const float g = 1.0f, c = s->ceiling_lin|' B &
pids+=($!)
sabotage readout fx/omx_clipper_instance.h 's|omx_lin_to_db(kept / over) : 0.0f|0.5f * omx_lin_to_db(kept / over) : 0.0f|' D &
pids+=($!)
sabotage bypass fx/omx_clipper_instance.h 's#  s->enabled = !bypass;#  s->enabled = !bypass || 1;#' A &
pids+=($!)
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || exit 1
echo "clipper_instance-perturb: the oracle sees all 7 sabotages"
