#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-include-selftest.sh — holds tools/contract-include.sh to the omx-contract range the
# packaging declares (FreeMixer/omx-dsp#95, #107). It renders a scratch omx-contract package at
# several versions and runs the script against each: inside the spec's range it is taken, below the
# floor, at or above the ceiling and with nothing installed it is refused, and the refusal names the
# package, the floor and the version installed. debian/control must carry the spec's range, and no
# tracked file may name a pins file again. No network is used.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
S="$(mktemp -d)"; trap 'rm -rf "$S"' EXIT
bad=0
fail() { echo "contract-include-selftest: FAIL — $*" >&2; bad=1; }

range="$(sh "$HERE/tools/contract-include.sh" --range)"
floor="${range% *}"; ceiling="${range#* }"
[ -n "$floor" ] && [ -n "$ceiling" ] && [ "$floor" != "$ceiling" ] || { echo "contract-include-selftest: no range read from the spec: '$range'" >&2; exit 1; }

# The spec, debian/control and omxdsp.pc.in say the same range.
for field in Build-Depends Depends; do
  grep -q "^$field: .*libomx-contract-dev (>= $floor), libomx-contract-dev (<< $ceiling)" "$HERE/debian/control" \
    || fail "debian/control's $field does not carry libomx-contract-dev (>= $floor), (<< $ceiling), the spec's range"
done
grep -q "^Requires: omx-contract-devel >= $floor\$" "$HERE/packaging/omx-dsp.spec" && grep -q "^Requires: omx-contract-devel < $ceiling\$" "$HERE/packaging/omx-dsp.spec" \
  || fail "the spec's devel Requires is not its BuildRequires range >= $floor, < $ceiling"
grep -q '^Requires: omx-contract >= @CONTRACTFLOOR@, omx-contract < @CONTRACTCEILING@$' "$HERE/omxdsp.pc.in" \
  || fail "omxdsp.pc.in does not require the spec's range (@CONTRACTFLOOR@, @CONTRACTCEILING@)"

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

# A scratch tree carrying a chosen range, so the bounds are tested whatever the spec says today.
tree() {
  rm -rf "$S/tree"; mkdir -p "$S/tree/tools" "$S/tree/packaging"
  cp "$HERE/tools/contract-include.sh" "$S/tree/tools/"
  printf 'Name: omx-dsp\nBuildRequires: omx-contract-devel >= %s\nBuildRequires: omx-contract-devel < %s\n' "$1" "$2" > "$S/tree/packaging/omx-dsp.spec"
}
taken() {  # floor ceiling version...
  f="$1" c="$2"; shift 2; tree "$f" "$c"
  for v in "$@"; do
    render "$v"
    out="$(run "$S/tree" 2>&1)" || { fail "$v is inside >= $f, < $c and was refused: $out"; continue; }
    [ "$out" = "$S/include" ] || fail "$v printed '$out', not $S/include"
  done
}
refused() {  # floor ceiling version...
  f="$1" c="$2"; shift 2; tree "$f" "$c"
  for v in "$@"; do
    render "$v"
    if out="$(run "$S/tree" 2>&1)"; then fail "$v is outside >= $f, < $c and was taken: $out"; continue; fi
    for word in omx-contract-devel libomx-contract-dev "$f" "$c" "$v"; do
      case "$out" in *"$word"*) ;; *) fail "the refusal at $v does not name '$word': $out" ;; esac
    done
  done
}
taken 2.10.1 2.11 2.10.1 2.10.2 2.10.10
refused 2.10.1 2.11 2.6.0 2.10.0 2.9.9 2.11.0 2.12.0 2.100.0 3.0.0
taken 2.11.0 2.12 2.11.0 2.11.4
refused 2.11.0 2.12 2.10.1 2.12.0 2.13.0 3.0.0

# The tree's own range: its floor is taken, the version below it and the ceiling are refused.
render "$floor"
out="$(run "$HERE" 2>&1)" || fail "the spec's own floor $floor was refused: $out"
render "$ceiling.0"
if out="$(run "$HERE" 2>&1)"; then fail "the spec's own ceiling $ceiling.0 was taken"; fi

# --data cuts each kernel's controls out of omx-contract.json, in the layout face-conformance.sh reads.
render "$floor"
if out="$(run "$HERE" --data 2>&1)"; then
  grep -q '^      "name": "keySource",$' "$out/gate.json" 2>/dev/null || fail "--data's gate.json is not in the layout face-conformance.sh reads: $out"
  rm -rf "$out"
else fail "--data refused a package with omx-contract.json: $out"; fi
mv "$S/share/omx-contract/omx-contract.json" "$S/omx-contract.json.off"
if out="$(run "$HERE" --data 2>&1)"; then fail "--data answered a package with no omx-contract.json: $out"
else case "$out" in *omx-contract*) ;; *) fail "the --data refusal names no package: $out" ;; esac; fi
mv "$S/omx-contract.json.off" "$S/share/omx-contract/omx-contract.json"

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
echo "contract-include-selftest: >= $floor, < $ceiling agrees in the spec, debian/control and omxdsp.pc.in; in range taken, below the floor, at the ceiling and nothing refused naming the package; no download path, no pins file"
