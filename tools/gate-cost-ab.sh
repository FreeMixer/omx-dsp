#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# gate-cost-ab.sh <base-rev> [rounds=5] [quantum=128] [blocks=2000] — the gate kernel's cost BEFORE
# (<base-rev>'s include/ and src/) against AFTER (this tree's), through tools/gate-cost-bench.c. The
# two builds run INTERLEAVED, `rounds` times, so drift on a shared host lands on both; each cell
# reports the median over rounds of each build's p50 and p95 and the ratio after/before. The output
# checksums must match between the builds, or the ratio means nothing: exit 1 when any differs.
# RELATIVE only: label the host.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
base="${1:?base revision}"; rounds="${2:-5}"; q="${3:-128}"; blocks="${4:-2000}"
nice_cmd=(); [ -z "${OMX_POD_CPUS:-}" ] && nice_cmd=(nice -n 19)
: "${CONTRACT_INC:=$(sh "$here/tools/contract-include.sh")}"
CC="${CC:-cc}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
mkdir -p "$work/base"
git -C "$here" archive "$base" include src | tar -x -C "$work/base"
flags=(-Wall -Wextra -Werror -O2 -ffp-contract=off -isystem "$CONTRACT_INC")
"${nice_cmd[@]}" "$CC" "${flags[@]}" -I"$work/base/include" -o "$work/before" "$here/tools/gate-cost-bench.c" \
  "$work/base"/src/*.c -lm
"${nice_cmd[@]}" "$CC" "${flags[@]}" -I"$here/include" -o "$work/after" "$here/tools/gate-cost-bench.c" "$here"/src/*.c -lm
for _ in $(seq 1 "$rounds"); do
  "${nice_cmd[@]}" "$work/before" "$q" "$blocks" | tail -n +2 | sed 's/^/before\t/' >> "$work/runs.tsv"
  "${nice_cmd[@]}" "$work/after" "$q" "$blocks" | tail -n +2 | sed 's/^/after\t/' >> "$work/runs.tsv"
done
echo "# host=$(hostname) pod_cpus=${OMX_POD_CPUS:-} base=$(git -C "$here" rev-parse --short "$base") after=working-tree rounds=$rounds quantum=$q blocks=$blocks (RELATIVE)"
awk -F'\t' '
  function med(s,   a, k, m, i, j, t) { m = split(s, a, " ");
    for (i = 2; i <= m; i++) { t = a[i]; for (j = i - 1; j >= 1 && a[j] + 0 > t + 0; j--) a[j + 1] = a[j]; a[j + 1] = t }
    return a[int((m + 1) / 2)] }
  { key = $2 "\t" $3; if (!(key in seen)) { seen[key] = 1; order[++nk] = key }
    p50[$1, key] = p50[$1, key] " " $5; p95[$1, key] = p95[$1, key] " " $6; ck[$1, key] = $7 }
  END {
    printf "setting\trate\tbefore_p50\tafter_p50\tratio_p50\tbefore_p95\tafter_p95\tratio_p95\toutput\n"
    bad = 0
    for (i = 1; i <= nk; i++) { k = order[i]
      b50 = med(p50["before", k]); a50 = med(p50["after", k]); b95 = med(p95["before", k]); a95 = med(p95["after", k])
      same = (ck["before", k] == ck["after", k]) ? "same" : "DIFFERS"; if (same != "same") bad = 1
      printf "%s\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%s\n", k, b50, a50, a50 / b50, b95, a95, a95 / b95, same }
    exit bad }' "$work/runs.tsv"
