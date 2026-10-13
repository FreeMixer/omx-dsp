#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# public-sources-check.sh — a public source states public facts and never points into a private
# repository (operator ruling 2026-10-13). Fails when a tracked file names:
#   FreeMixer/openmixer, openmixer-ops, openmixer#N, openmixer #N, any <name>-dev#N,
#   integration/<branch>, packages/<name>/ (a path in the private repository), a .vue file,
#   docs/design/specs|notes/ (its design documents).
# State the fact itself and cite a public repository (omx-dsp#N, omx-contract, omx-plugins).
#
#   tools/public-sources-check.sh              the check
#   tools/public-sources-check.sh --self-test  a private name in a header comment, a .md and a .sh
#                                              must be found; an allowlisted line must not be
#
# The allowlist is functional code only (a value, not prose): the make recipes and scripts that
# take an OpenMixer checkout and read its sources. Each entry is "path<TAB>text the line contains".
set -euo pipefail
cd "$(dirname "$0")/.."

PATTERN='FreeMixer/openmixer|openmixer-ops|openmixer ?#[0-9]|[A-Za-z0-9._]+-dev#[0-9]|integration/[A-Za-z0-9]|packages/[a-z-]+/|\.vue\b|docs/design/(specs|notes)/'

ALLOW=$'Makefile\t$(OPENMIXER)/packages/pipewire-native/src
tools/analysis-engine-identity.sh\t$(cd "$1" && pwd)/packages/pipewire-native/src
tools/ts-golden-hooks.mjs\t${data.openmixer}/packages/
tools/ts-golden.mjs\t${openmixer}/packages/'

# scan <root>: "file:line: text" for each private name outside the allowlist, over the files listed on stdin
scan() {
  local root="$1" f n text path frag ok
  while IFS= read -r f; do
    [ "$f" = tools/public-sources-check.sh ] && continue
    while IFS=: read -r n text; do
      ok=
      while IFS=$'\t' read -r path frag; do
        [ "$path" = "$f" ] && [[ "$text" == *"$frag"* ]] && ok=1
      done <<<"$ALLOW"
      [ -n "$ok" ] || printf '%s:%s:%s\n' "$f" "$n" "$text"
    done < <(cd "$root" && grep -nIE -- "$PATTERN" "$f" || true)
  done
}

if [ "${1:-}" = --self-test ]; then
  d="$(mktemp -d)"; trap 'rm -rf "$d"' EXIT
  mkdir -p "$d/include" "$d/tools"
  printf '/* see packages/core/src/eq.ts */\n' > "$d/include/a.h"
  printf 'Tracked in FreeMixer/openmixer#1580.\n' > "$d/b.md"
  printf '# moved from openmixer #1601\n' > "$d/tools/c.sh"
  printf '// branch integration/waves\n' > "$d/include/d.c"
  printf 'x "$(OPENMIXER)/packages/pipewire-native/src"\n' > "$d/Makefile"
  printf 'a clean line about OpenMixer, the product\n' > "$d/clean.md"
  found="$(printf '%s\n' include/a.h b.md tools/c.sh include/d.c Makefile clean.md | scan "$d")"
  for want in include/a.h b.md tools/c.sh include/d.c; do
    grep -q "^$want:" <<<"$found" || { echo "public-sources-check self-test: $want was NOT found" >&2; exit 1; }
  done
  if grep -qE '^(Makefile|clean\.md):' <<<"$found"; then
    echo "public-sources-check self-test: an allowlisted or clean line was reported" >&2; exit 1
  fi
  echo "public-sources-check self-test: a header comment, a .md, a .sh and a .c are found; the allowlist and a clean file are not"
  exit 0
fi

found="$(git ls-files | scan .)"
if [ -n "$found" ]; then
  echo "public-sources-check: these lines point at private repositories or paths; state the fact itself:" >&2
  printf '%s\n' "$found" >&2
  exit 1
fi
echo "public-sources-check: no tracked file points into a private repository"
