#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Renders the generated headers again from an OpenMixer checkout and compares them byte for byte
# with the ones committed here. The declaration is OpenMixer's, so only its generators may write
# these files; a difference means the files were edited by hand or the pin moved without a new
# export.
#
#   tools/render-check.sh <openmixer checkout>
#
# The checkout must have its generators' inputs built: pnpm install, then
# pnpm --filter "@freemixer/plugin-qualify..." build (core, declarations and plugin-qualify).
#
# Writes nothing into either tree: the renders go to a scratch directory.
set -euo pipefail
OPENMIXER="${1:?usage: tools/render-check.sh <openmixer checkout>}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

pin="$(awk '$1 == "openmixer" { print $3 }' "$HERE/.github/pins.txt")"
have="$(git -C "$OPENMIXER" rev-parse HEAD)"
if [[ -n "$pin" && "$have" != "$pin" ]]; then
  echo "render-check: $OPENMIXER is at $have, the pin is $pin" >&2
  exit 2
fi

(cd "$OPENMIXER" && node harness/contract-limits-gen.mjs --emit-stdout) > "$OUT/omx_contract_limits.h"
(cd "$OPENMIXER" && node packages/omx-plugins/tools/params-gen.mjs --out-dir "$OUT/params" --ttl-out "$OUT/lv2" >/dev/null)

fail=0
compare() {
  if cmp -s "$1" "$2"; then
    echo "render-check: $2 is the render"
  else
    echo "render-check: $2 differs from the render of $have" >&2
    diff -u "$1" "$2" | head -20 >&2 || true
    fail=1
  fi
}
compare "$OUT/omx_contract_limits.h" "$HERE/include/omxdsp/omx_contract_limits.h"
for h in "$HERE"/include/omxdsp/params/omx_*_params.h; do
  compare "$OUT/params/$(basename "$h")" "$h"
done
exit "$fail"
