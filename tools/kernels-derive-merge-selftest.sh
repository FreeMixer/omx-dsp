#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Proves the point of tools/kernels-gen.sh: two lanes that each add ONE new kernel — a suite arm
# (test/kernels/<k>.c) and a perturb case (test/kernels/<k>.perturb.c) — never touch
# omxdsp_suite.c, omxdsp_perturb.c, perturb.sh or the Makefile, so merging both onto the same
# base is conflict-free. Before this lane, both files' main() were hand-appended call lists every
# kernel lane edited on the same lines.
#
# Runs against a disposable pair of branches off the CURRENT HEAD; never pushes, never touches
# origin, deletes its branches on exit.
set -euo pipefail
REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"
PKG=.

BASE_SHA="$(git rev-parse HEAD)"
BASE_REF="$(git symbolic-ref -q --short HEAD || echo "$BASE_SHA")"
BR_A="selftest/omxdsp-kernel-fake-a"
BR_B="selftest/omxdsp-kernel-fake-b"
BR_MERGE="selftest/omxdsp-kernel-merge"

cleanup() {
  git checkout -q "$BASE_REF" 2>/dev/null || true
  git branch -D "$BR_A" "$BR_B" "$BR_MERGE" >/dev/null 2>&1 || true
}
trap cleanup EXIT

git checkout -q -b "$BR_A" "$BASE_SHA"
cat > "$PKG/test/kernels/fake_a.c" << 'EOF'
static void arm_fake_a(void) {
  g_arm = "fake_a";
  ok(1, "selftest fixture always passes", 1.0, 1.0);
  expect_clean();
}
EOF
git add "$PKG/test/kernels/fake_a.c"
git commit -q -m "selftest: fake kernel a suite arm"

git checkout -q -b "$BR_B" "$BASE_SHA"
cat > "$PKG/test/kernels/fake_b.perturb.c" << 'EOF'
static int perturb_fake_b(int failed) {
  return failed;
}
EOF
git add "$PKG/test/kernels/fake_b.perturb.c"
git commit -q -m "selftest: fake kernel b perturb case"

git checkout -q -b "$BR_MERGE" "$BASE_SHA"
if ! git merge -q --no-ff --no-edit "$BR_A" > /tmp/selftest-omxdsp-merge-a.log 2>&1; then
  echo "FAIL: lane A did not merge cleanly onto base" >&2
  cat /tmp/selftest-omxdsp-merge-a.log >&2
  exit 1
fi
if ! git merge -q --no-ff --no-edit "$BR_B" > /tmp/selftest-omxdsp-merge-b.log 2>&1; then
  echo "FAIL: lane B conflicted merging onto lane A - derive-not-append broken" >&2
  cat /tmp/selftest-omxdsp-merge-b.log >&2
  git status --short >&2
  exit 1
fi

for f in "$PKG/test/omxdsp_suite.c" "$PKG/test/omxdsp_perturb.c" "$PKG/test/perturb.sh" "$PKG/Makefile"; do
  if ! git diff --quiet "$BASE_SHA" "$BR_MERGE" -- "$f"; then
    echo "FAIL: $f differs from base after both fake-kernel lanes merged" >&2
    exit 1
  fi
done

echo "PASS: both fake-kernel lanes merged with zero conflicts, and none of the four shared files changed"
git log --oneline "$BASE_SHA..$BR_MERGE"
