#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# kernel-cost-ab.sh — one effect kernel's cost, BASE vs TREE, interleaved on one core.
#
#   bash tools/kernel-cost-ab.sh <base-ref> [part=all] [rounds=5]      (make cost-kernel-ab)
#
# Builds the tree's tools/kernel-cost.c twice with the same compiler and flags: against
# <base-ref>'s include/ and src/ (git archive, so the base is exactly that commit) and against the
# working tree's. The bench source is the tree's on both sides, so the headers and the compiled
# unit are the only difference. Runs the two alternately, `rounds` times each, pinned to one CPU
# where taskset exists, so a governor or a neighbour that drifts across the run moves both columns
# alike. Prints every run's rows and, per part, setting and rate, the median-of-rounds p50 of each
# side, the ratio base/tree and whether the two rendered the same bits (the output digest).
#
# The ratio is the number to quote, labelled with the machine that ran it; an absolute figure from
# one machine is not another's. Exits 1 when any cell rendered different bits: a kernel change
# that claims to be bit-identical and is not shows here before its golden digest is rewritten.
set -euo pipefail
base=${1:?usage: kernel-cost-ab.sh <base-ref> [part] [rounds]}
part=${2:-all}
rounds=${3:-5}
root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
read -r -a cflags <<<"${CFLAGS:--Wall -Wextra -Werror -O2}"
inc=${CONTRACT_INC:-$(sh "$root/tools/contract-include.sh")}
out="$root/build/kcost"
rm -rf "$out" && mkdir -p "$out/base"
git -C "$root" archive "$base" include src | tar -x -C "$out/base"

build() { # <tree-root> <bin>
  "$cc" "${cflags[@]}" -ffp-contract=off -I"$1/include" -isystem "$inc" -o "$2" \
    "$root/tools/kernel-cost.c" "$1"/src/*.c -lm
}
build "$out/base" "$out/bench-base"
build "$root" "$out/bench-tree"

pin=()
if command -v taskset >/dev/null; then pin=(taskset -c "$(taskset -pc $$ | sed 's/.*: //; s/[,-].*//')"); fi
echo "# kernel-cost-ab part=$part base=$(git -C "$root" rev-parse --short "$base") tree=$(git -C "$root" rev-parse --short HEAD)$(git -C "$root" diff --quiet -- include src || echo +dirty) cpu=\"$(sed -n 's/^model name\s*: //p' /proc/cpuinfo | head -1)\" rounds=$rounds"
: >"$out/runs.tsv"
for ((i = 0; i < rounds; i++)); do
  for side in base tree; do
    "${pin[@]}" "$out/bench-$side" "$part" | sed '1d' | sed "s/^/$side\t/" | tee -a "$out/runs.tsv"
  done
done
awk -F'\t' '
  { k = $2 "\t" $3 "\t" $4; v[$1, k, ++n[$1, k]] = $7; h[$1, k] = $9; keys[k] = 1 }
  function med(side, k,   m, i, j, a, t) {
    m = n[side, k]; for (i = 1; i <= m; i++) a[i] = v[side, k, i]
    for (i = 1; i <= m; i++) for (j = i + 1; j <= m; j++) if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
    return a[int((m + 1) / 2)]
  }
  END {
    bad = 0
    for (k in keys) {
      split(k, f, "\t"); b = med("base", k); t = med("tree", k)
      same = (h["base", k] == h["tree", k]) ? "yes" : "NO"
      if (same == "NO") bad = 1
      printf "KCOST part=%s setting=%s rate=%s base_p50_ns_per_sample=%.3f tree_p50_ns_per_sample=%.3f ratio_base_over_tree=%.3f same_bits=%s\n",
        f[1], f[2], f[3], b, t, (t > 0 ? b / t : 0), same
    }
    exit bad
  }' "$out/runs.tsv" | sort -t= -k2,2 -k3,3 -k4,4n
status=${PIPESTATUS[0]}
if [ "$status" -ne 0 ]; then
  echo "kernel-cost-ab: the tree renders different bits from $base in a cell marked same_bits=NO" >&2
  exit 1
fi
