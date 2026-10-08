#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# transient-perturb.sh — omx_transient.h reads its travels from the generated
# omx_contract_limits.h, never a copy (R-094; transient spec §3). A PERTURBED header (the slow
# attack's floor moved from its declared value to 20 ms) is written into a copy of the include
# directory, and test/fx/transient.test.c is built against it: its declared-travel arm must now
# RECORD a 10 ms slow attack and clamp it to 20 ms. The same source against the real header is the
# control and records nothing. Usage: CC=… CFLAGS=… transient-perturb.sh <scratch-dir>
set -euo pipefail
PKG="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?scratch dir}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -O2 -ffp-contract=off}"
INC="$PKG/include"
: "${CONTRACT_INC:=$(sh "$PKG/tools/contract-include.sh")}"   # make exports it
rm -rf "$OUT" && mkdir -p "$OUT/omxcontract"
cp "$CONTRACT_INC"/omxcontract/*.h "$OUT/omxcontract/"
sed -i 's|^#define OMX_TRANSIENT_ATTACK_TIME_MS_MIN .*$|#define OMX_TRANSIENT_ATTACK_TIME_MS_MIN 20.0f|' "$OUT/omxcontract/omx_contract_limits.h"
grep -q '^#define OMX_TRANSIENT_ATTACK_TIME_MS_MIN 20.0f$' "$OUT/omxcontract/omx_contract_limits.h" || { echo "transient-perturb: FAIL — the attack-time floor line was not found"; exit 1; }
if cmp -s "$CONTRACT_INC/omxcontract/omx_contract_limits.h" "$OUT/omxcontract/omx_contract_limits.h"; then echo "transient-perturb: FAIL — the perturbation changed nothing"; exit 1; fi
# shellcheck disable=SC2086 # CFLAGS is a flag list
$CC -I"$OUT" -I"$INC" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread -o "$OUT/perturbed" "$PKG/test/fx/transient.test.c" -lm
# shellcheck disable=SC2086
$CC -I"$INC" -isystem "$CONTRACT_INC" -I"$PKG/test/fx" $CFLAGS -DOMX_CONTRACTS -pthread -o "$OUT/control" "$PKG/test/fx/transient.test.c" -lm
# The perturbed build's other arms also record their now-below-floor times: its exit is not the verdict.
p="$({ "$OUT/perturbed" || true; } | grep '^fx/transient: declared travel')"
"$OUT/control" >"$OUT/control.log" || { cat "$OUT/control.log"; echo "transient-perturb: FAIL — the control run is red"; exit 1; }
c="$(grep '^fx/transient: declared travel' "$OUT/control.log")"
echo "perturbed: $p"
echo "control:   $c"
[[ "$p" == *"20.0 ms, 10 ms RECORDED and clamped" ]] || { echo "transient-perturb: FAIL — the kernel did not follow the moved travel"; exit 1; }
[[ "$c" == *"2.0 ms, 10 ms inside" ]] || { echo "transient-perturb: FAIL — the control moved"; exit 1; }
echo "transient-perturb: the kernel followed the perturbed declaration and the control followed the real one"
