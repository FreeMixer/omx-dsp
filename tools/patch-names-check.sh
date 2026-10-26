#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# patch-names-check.sh — a patch release adds no public name (FreeMixer/openmixer-ops#274). v0.18.1 added
# omx_eq_lowpass_nyquist_handover() and two OMX_EQ_LOWPASS_HANDOVER_* defines, v0.20.1 added
# omx_eq_lowpass_nyquist_raise(); a new public name goes in a minor. When include/omxdsp/omxdsp.h says patch > 0,
# every `#define NAME` and `static inline ... name(` under include/ must already exist at the tag vMAJOR.MINOR.0.
# Patch 0, or no such tag, is a minor: nothing to compare. Part of `make lint`.
#
#   tools/patch-names-check.sh [REPO]   the check in REPO (default this checkout)
#   tools/patch-names-check.sh --self-test   a new define and a new inline in a patch are named, a minor's are not
set -euo pipefail

names() { # <repo> <rev-or-WORKTREE>: one public name per line
  local repo=$1 rev=$2 f
  { if [ "$rev" = WORKTREE ]; then cat "$repo"/include/omxdsp/*.h
    else for f in $(git -C "$repo" ls-tree -r --name-only "$rev" include | grep '\.h$'); do git -C "$repo" show "$rev:$f"; done; fi; } |
    sed -nE 's/^[[:space:]]*#[[:space:]]*define[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/p;
             s/^static inline[^(]*[[:space:]*]([A-Za-z_][A-Za-z0-9_]*)\(.*/\1/p' | sort -u
}

check() {
  local repo=$1 h=$1/include/omxdsp/omxdsp.h maj min pat base added
  maj=$(sed -nE 's/^#define OMXDSP_VERSION_MAJOR ([0-9]+).*/\1/p' "$h")
  min=$(sed -nE 's/^#define OMXDSP_VERSION_MINOR ([0-9]+).*/\1/p' "$h")
  pat=$(sed -nE 's/^#define OMXDSP_VERSION_PATCH ([0-9]+).*/\1/p' "$h")
  [ -n "$maj$min$pat" ] || { echo "patch-names-check: no version in $h" >&2; return 1; }
  [ "$pat" -gt 0 ] || { echo "patch-names-check: $maj.$min.$pat is a minor, nothing to compare"; return 0; }
  base=v$maj.$min.0
  git -C "$repo" rev-parse -q --verify "refs/tags/$base" >/dev/null || { echo "patch-names-check: no tag $base, nothing to compare"; return 0; }
  added=$(comm -13 <(names "$repo" "$base") <(names "$repo" WORKTREE))
  if [ -n "$added" ]; then
    echo "patch-names-check: $maj.$min.$pat adds public names to $base; they go in a minor:" >&2
    echo "$added" >&2
    return 1
  fi
  echo "patch-names-check: $maj.$min.$pat adds no public name over $base"
}

if [ "${1:-}" = "--self-test" ]; then
  T=$(mktemp -d "${TMPDIR:-/tmp}/patch-names-check.XXXXXX"); trap 'rm -rf "$T"' EXIT
  mkdir -p "$T/include/omxdsp"; cd "$T"; git init -q .
  printf '#define OMXDSP_VERSION_MAJOR 0\n#define OMXDSP_VERSION_MINOR 1\n#define OMXDSP_VERSION_PATCH 0\n#define OMX_OLD 1\nstatic inline double omx_old(double q) { return q; }\n' > include/omxdsp/omxdsp.h
  git add -A; git -c user.name=t -c user.email=t@t commit -qm base; git tag v0.1.0
  fail=0
  sed -i 's/PATCH 0/PATCH 1/' include/omxdsp/omxdsp.h
  check "$T" >/dev/null 2>&1 || { echo "patch-names-check --self-test: FAIL, an unchanged patch is named"; fail=1; }
  echo '#define OMX_NEW 2' >> include/omxdsp/omxdsp.h
  ! check "$T" >/dev/null 2>&1 || { echo "patch-names-check --self-test: FAIL, a new define in a patch is not named"; fail=1; }
  sed -i '$d' include/omxdsp/omxdsp.h; echo 'static inline double omx_new(double q) { return q; }' >> include/omxdsp/omxdsp.h
  ! check "$T" >/dev/null 2>&1 || { echo "patch-names-check --self-test: FAIL, a new inline in a patch is not named"; fail=1; }
  sed -i 's/PATCH 1/PATCH 0/' include/omxdsp/omxdsp.h
  check "$T" >/dev/null 2>&1 || { echo "patch-names-check --self-test: FAIL, a minor's new name is named"; fail=1; }
  [ "$fail" -eq 0 ] && echo "patch-names-check --self-test: a new define or inline in a patch is named; an unchanged patch and a minor are not"
  exit "$fail"
fi
check "$(cd "${1:-$(dirname "$0")/..}" && pwd)"
