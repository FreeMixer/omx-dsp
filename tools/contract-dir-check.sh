#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-dir-check.sh — data/ read back through omx-contract's contract directory reader: this tree's
# definitions, installed the way `make install` installs them, beside omx-contract's own, read as one contract.
#
#   tools/contract-dir-check.sh <omx-contract checkout> <build dir>
#
# The checkout gives the reader (bin/omx-contract.mjs, its node_modules installed), the installer
# (tools/install-contract-dir.sh) and omx-contract's own data/, installed as the package omx-contract. Both
# packages go into <build dir>/dir; then, with that directory:
#   validate --needs omx-dsp@'>= <this version>'   every file found, hashed, floored, validated and merged
#   links --check --package omx-dsp                every source link resolves in this tree
#   fmt --check --data data                        the files are in the one formatting
# and three arms that must each be refused with their code: an installed file changed after the manifest
# (contract.manifest.mismatch), a file installed with no schema floor (contract.schema.floor-absent) and an
# impl link to a symbol this tree does not define (contract.source.symbol-absent).
# It reads only against a contract that already dropped this tree's files (no overlap: a definition has one
# installed home, and the reader refuses a second with contract.item.duplicate). A checkout that still carries one of
# them is an environment this check cannot read, exit 2, named, before anything runs.
# Exit 0 green; 1 a check or an arm failed; 2 usage or environment (no node, no checkout, a checkout that still
# carries this tree's definitions), never a pass.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
[ "$#" -eq 2 ] || { sed -n '8p' "$0" >&2; exit 2; }
OC="$(cd "$1" 2>/dev/null && pwd)" || { echo "contract-dir-check: no omx-contract checkout at $1" >&2; exit 2; }
out="$(mkdir -p "$2" && cd "$2" && pwd)"
tool="$OC/bin/omx-contract.mjs" installer="$OC/tools/install-contract-dir.sh"
command -v node >/dev/null 2>&1 || { echo "contract-dir-check: node is missing" >&2; exit 2; }
[ -f "$tool" ] && [ -f "$installer" ] && [ -d "$OC/node_modules" ] || { echo "contract-dir-check: $OC has no bin/omx-contract.mjs, tools/install-contract-dir.sh or node_modules (npm ci there)" >&2; exit 2; }
version="$(make -s -C "$HERE" version)"
major="$(sed -n 's/^  "x-omx-schema-version": "\([0-9]*\)\..*",$/\1/p' "$OC/schema/omx-contract.schema.json")"
ocversion="$(sed -n 's/^  "version": "\(.*\)",$/\1/p' "$OC/package.json")"
[ -n "$major" ] && [ -n "$ocversion" ] || { echo "contract-dir-check: $OC states no schema major or version" >&2; exit 2; }
oc() { (cd "$OC" && node bin/omx-contract.mjs "$@"); }

# install <dir> <omx-dsp data dir>: the two packages of the contract directory under <dir>
install_both() {
  rm -rf "$1"; mkdir -p "$1"
  sh "$installer" -x codes.json omx-contract "$ocversion" "$major" "$OC/data" "$1" data >/dev/null
  sh "$installer" omx-dsp "$version" "$major" "$2" "$1" data >/dev/null
}

# refused <code> <what> <command...>: the command exits 1 and names the code first on a line
refused() {
  local code="$1" what="$2" got rc=0
  shift 2
  got="$("$@" 2>&1)" || rc=$?
  [ "$rc" = 1 ] || { echo "contract-dir-check: $what: exit $rc, not 1" >&2; printf '%s\n' "$got" | tail -3 >&2; return 1; }
  grep -q "^$code " <<<"$got" || { echo "contract-dir-check: $what: refused, but not with $code" >&2; printf '%s\n' "$got" | tail -3 >&2; return 1; }
  echo "contract-dir-check: $what: refused with $code"
}

# the checkout must not hold a definition this tree holds: the same file, or the same item in another file
held="$(cd "$OC" && node --input-type=module -e '
const [oc, own] = process.argv.slice(1);
const { loadData } = await import(oc + "/lib/data.mjs");
const names = (d) => new Map([...loadData(d).items.values()].map((it) => [it.name, it.rel]));
const theirs = names(oc + "/data");
for (const [n, rel] of names(own)) if (theirs.has(n)) console.log(`${n} (${rel} here, ${theirs.get(n)} there)`);
' "$OC" "$HERE/data" 2>&1)" || { echo "contract-dir-check: cannot read the definitions of $OC or data/: $held" >&2; exit 2; }
if [ -n "$held" ]; then
  echo "contract-dir-check: $OC still carries $(wc -l <<<"$held") of this tree's definitions, e.g. $(head -1 <<<"$held"): this check reads against a contract that dropped them" >&2
  exit 2
fi

dir="$out/dir"
install_both "$dir" "$HERE/data"
oc validate --contract-dir "$dir" --needs "omx-dsp@>= $version"
oc links --check --root "$HERE" --contract-dir "$dir" --package omx-dsp
oc fmt --check --data "$HERE/data"

bad=0
# an installed file changed after its manifest was written
f="$(cd "$dir/$major/omx-dsp" && find . -name '*.json' ! -name MANIFEST.json | LC_ALL=C sort | head -1)"
sed -i '0,/"doc": "/s//"doc": "changed /' "$dir/$major/omx-dsp/$f"
refused contract.manifest.mismatch "an installed file changed after its manifest" oc validate --contract-dir "$dir" || bad=1

# a file installed without its schema floor
scratch="$out/no-floor"; rm -rf "$scratch"; cp -r "$HERE/data" "$scratch"
f="$(cd "$scratch" && find . -name '*.json' | LC_ALL=C sort | head -1)"
perl -0pi -e 's/\n  "requires": \{\n    "omx-contract-schema": "[^"]*"\n  \},//' "$scratch/$f"
install_both "$out/dir-no-floor" "$scratch"
refused contract.schema.floor-absent "a file with no schema floor" oc validate --contract-dir "$out/dir-no-floor" || bad=1

# an impl link to a symbol this tree does not define
scratch="$out/bad-link"; rm -rf "$scratch"; cp -r "$HERE/data" "$scratch"
f="$(cd "$scratch" && grep -l '"impl": "' -r . | LC_ALL=C sort | head -1)"
sed -i '0,/\("impl": "[^"#]*#\)\([A-Za-z_]*\)"/s//\1\2_absent"/' "$scratch/$f"
install_both "$out/dir-bad-link" "$scratch"
refused contract.source.symbol-absent "an impl link to an undefined symbol" oc links --check --root "$HERE" --contract-dir "$out/dir-bad-link" --package omx-dsp || bad=1

[ "$bad" = 0 ] || exit 1
echo "contract-dir-check: data/ installs as omx-dsp $version beside omx-contract $ocversion and reads back whole; three arms refused"
