#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-include-selftest.sh — holds tools/contract-include.sh to the omx-contract range the
# packaging declares (FreeMixer/omx-dsp#95, #107). It renders a scratch omx-contract package at
# several versions and runs the script against each: at or above the floor it is taken (a newer major
# too: the declared dependency is a floor, never a ceiling), below the floor and with nothing installed
# it is refused, and the refusal names the package, the floor and the version installed. The spec,
# debian/control and omxdsp.pc.in carry the floor and NO upper bound, and no tracked file may name a
# pins file again. No network is used.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
S="$(mktemp -d)"; trap 'rm -rf "$S"' EXIT
bad=0
fail() { echo "contract-include-selftest: FAIL — $*" >&2; bad=1; }

floor="$(sh "$HERE/tools/contract-include.sh" --floor)"
[ -n "$floor" ] || { echo "contract-include-selftest: no floor read from the spec" >&2; exit 1; }

# The spec, debian/control and omxdsp.pc.in say the same floor, and none of them an upper bound.
for field in Build-Depends Depends; do
  grep -q "^$field: .*libomx-contract-dev (>= $floor)" "$HERE/debian/control" \
    || fail "debian/control's $field does not carry libomx-contract-dev (>= $floor), the spec's floor"
done
grep -q "^Requires: omx-contract-devel >= $floor\$" "$HERE/packaging/omx-dsp.spec" \
  || fail "the spec's devel Requires is not its BuildRequires floor >= $floor"
grep -q '^Requires: omx-contract >= @CONTRACTFLOOR@$' "$HERE/omxdsp.pc.in" \
  || fail "omxdsp.pc.in does not require the spec's floor (@CONTRACTFLOOR@) alone"
# No ceiling anywhere a dependency on the contract is declared (operator ruling 2026-10-15: floors only).
ceilings() {  # tree: every upper bound or exact pin on the contract in the three declaring files
  { grep -E '^(Build)?Requires:.*omx-contract' "$1/packaging/omx-dsp.spec" || true
    grep -E '^(Build-Depends|Depends):' "$1/debian/control" | grep -oE 'libomx-contract-dev \([^)]*\)' || true
    grep -E '^Requires:.*omx-contract' "$1/omxdsp.pc.in" || true; } | sed -e 's/>=//g' -e 's/(>> *[0-9.]*)//g' | grep -E '<|=' || true
}
[ -z "$(ceilings "$HERE")" ] || fail "an upper bound on omx-contract is declared again: $(ceilings "$HERE")"
# Positive control: the scan sees a planted ceiling in each of the three files.
mkdir -p "$S/ceil/packaging" "$S/ceil/debian"
cp "$HERE/packaging/omx-dsp.spec" "$S/ceil/packaging/"; cp "$HERE/debian/control" "$S/ceil/debian/"; cp "$HERE/omxdsp.pc.in" "$S/ceil/"
printf 'BuildRequires: omx-contract-devel < 99\n' >> "$S/ceil/packaging/omx-dsp.spec"
[ -n "$(ceilings "$S/ceil")" ] || fail "the ceiling scan does not see a planted spec ceiling; it would prove nothing"
cp "$HERE/packaging/omx-dsp.spec" "$S/ceil/packaging/"; sed -i 's/(>= \([0-9.]*\)), pkgconf/(>= \1), libomx-contract-dev (<< 99), pkgconf/' "$S/ceil/debian/control"
[ -n "$(ceilings "$S/ceil")" ] || fail "the ceiling scan does not see a planted debian/control ceiling; it would prove nothing"
cp "$HERE/debian/control" "$S/ceil/debian/"; sed -i 's/^Requires: omx-contract >= @CONTRACTFLOOR@$/&, omx-contract < 99/' "$S/ceil/omxdsp.pc.in"
[ -n "$(ceilings "$S/ceil")" ] || fail "the ceiling scan does not see a planted omxdsp.pc.in ceiling; it would prove nothing"

# A scratch package. omx-contract.json carries one kernel, pretty-printed as the release ships it.
mkdir -p "$S/lib/pkgconfig" "$S/include/omxcontract" "$S/share/omx-contract"
: > "$S/include/omxcontract/omx_contract_limits.h"
printf '%s\n' '{' '  "name": "x",' '  "kernels": {' '    "gate": {' '      "controls": [' '        {' '          "name": "keySource",' \
  '          "kind": "choice"' '        }' '      ]' '    }' '  }' '}' > "$S/share/omx-contract/omx-contract.json"
render() {
  printf 'prefix=%s\nincludedir=${prefix}/include\ndatadir=${prefix}/share/omx-contract\nName: omx-contract\nDescription: scratch\nVersion: %s\nCflags: -I${includedir}\n' \
    "$S" "$1" > "$S/lib/pkgconfig/omx-contract.pc"
}
# $1: the tree whose tools/contract-include.sh runs; the rest are its arguments.
run() { t="$1"; shift; env -u OMX_CONTRACT_INC PKG_CONFIG_PATH="$S/lib/pkgconfig" PKG_CONFIG_LIBDIR="$S/lib/pkgconfig" \
        http_proxy=http://127.0.0.1:9 https_proxy=http://127.0.0.1:9 sh "$t/tools/contract-include.sh" "$@"; }

# A scratch tree carrying a chosen floor, so the bound is tested whatever the spec says today.
tree() {
  rm -rf "$S/tree"; mkdir -p "$S/tree/tools" "$S/tree/packaging"
  cp "$HERE/tools/contract-include.sh" "$S/tree/tools/"
  printf 'Name: omx-dsp\nBuildRequires: omx-contract-devel >= %s\n' "$1" > "$S/tree/packaging/omx-dsp.spec"
}
taken() {  # floor version...
  f="$1"; shift; tree "$f"
  for v in "$@"; do
    render "$v"
    out="$(run "$S/tree" 2>&1)" || { fail "$v is at or above >= $f and was refused: $out"; continue; }
    [ "$out" = "$S/include" ] || fail "$v printed '$out', not $S/include"
  done
}
refused() {  # floor version...
  f="$1"; shift; tree "$f"
  for v in "$@"; do
    render "$v"
    if out="$(run "$S/tree" 2>&1)"; then fail "$v is below >= $f and was taken: $out"; continue; fi
    for word in omx-contract-devel libomx-contract-dev "$f" "$v"; do
      case "$out" in *"$word"*) ;; *) fail "the refusal at $v does not name '$word': $out" ;; esac
    done
  done
}
taken 2.10.1 2.10.1 2.10.2 2.10.10 2.11.0 2.12.0 2.100.0 3.0.0
refused 2.10.1 2.6.0 2.10.0 2.9.9
taken 2.11.0 2.11.0 2.11.4 2.12.0 2.13.0 3.0.0
refused 2.11.0 2.10.1 2.10.10 2.6.0
taken 2.14.0 2.14.0 2.14.3 2.15.0 3.0.0
refused 2.14.0 2.13.0 2.13.9 2.11.0 2.6.0

# The tree's own floor is taken.
render "$floor"
out="$(run "$HERE" 2>&1)" || fail "the spec's own floor $floor was refused: $out"

# Nothing installed.
rm -f "$S/lib/pkgconfig/omx-contract.pc"
if out="$(run "$HERE" 2>&1)"; then fail "no package installed and the script answered: $out"
else for word in omx-contract-devel libomx-contract-dev "$floor" 'not installed'; do
  case "$out" in *"$word"*) ;; *) fail "the refusal with nothing installed does not name '$word': $out" ;; esac
done; fi

# debian/changelog is rewritten from the commit subjects at build time, and history may name what it removed, so the scan leaves changelogs out.
# The removed paths stay removed: no download, no exact version, no pins file anywhere in the tree.
if grep -nE 'curl|wget|codeload|release\.tgz|exact-version' "$HERE/tools/contract-include.sh" | grep -v '^[0-9]*:#'; then
  fail "tools/contract-include.sh carries a download or exact-version path again"
fi
pins() { grep -rIl --exclude-dir=.git --exclude-dir=build --exclude=contract-include-selftest.sh --exclude=changelog -e 'pins\.txt' "$1" || true; }
mkdir -p "$S/plant/.github"; cp "$HERE/Makefile" "$S/plant/"; echo "X := \$(shell cat .github/pins.txt)" >> "$S/plant/Makefile"
[ -n "$(pins "$S/plant")" ] || fail "the pins-file scan does not see a planted line; it would prove nothing"
found="$(pins "$HERE")"; [ -z "$found" ] || fail "these files name a pins file: $found"
[ ! -e "$HERE/.github/pins.txt" ] || fail ".github/pins.txt is back"

[ "$bad" = 0 ] || exit 1
echo "contract-include-selftest: >= $floor agrees in the spec, debian/control and omxdsp.pc.in with no ceiling; floor and newer taken, below the floor and nothing refused naming the package; no download path, no pins file"
