#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# selftest.sh — omx-rt-gate judged over its planted fixtures, red first, then the clean twins green,
# then the load refusals.
#
#   tools/rt-gate/selftest.sh GATE TABLES [--graphs DIR --tags FILE] [--cflags "FLAGS"]
#
# Each fixture is built the way a distribution builds, -O2 -Werror -D_FORTIFY_SOURCE=2
# -fstack-protector-strong (the gate takes the optimisation, the fortification and the stack
# protector's guard call out of its input). Each fixtures/*.c names what it binds and what the
# gate must say about it:
#   // abi: lv2|clap|...          the --abi it binds
#   // tags: <file>               a thread-tag file the run reads
#   // needs: graphs              judged only when --graphs is given (omx-dsp's shipped graphs)
#   // expect: 0|1                the gate's exit code
#   // expect-red: <line>         every red the gate prints, exactly: no more, no fewer
#   // without-graphs-red: <line> the reds the same unit reads when the shipped graphs are left out
# A planted fixture judged green, a clean twin judged red, a red at another place or through another
# path, a missing red or an extra one: FAIL. Then every load refusal: a waiver-shaped row (a site, a
# field, a function of ours), a class outside the closed set (known-red, cannot-prove), a row without
# a reason or without the arm that measured it, a duplicate, a row for a forbidden symbol, an empty
# forbidden list and a per-consumer list beside the shipped ones must each exit 2 with REFUSED.
# Exit 0 when every case holds, 1 otherwise.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GATE="$1"
TABLES="$2"
shift 2
GRAPHS=""; TAGS=""; EXTRA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --graphs) GRAPHS="$2"; shift 2 ;;
    --tags) TAGS="$2"; shift 2 ;;
    --cflags) EXTRA="$2"; shift 2 ;;
    *) echo "selftest: unknown argument $1" >&2; exit 2 ;;
  esac
done
CC="${CC:-cc}"
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/omx-rt-gate-selftest.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT
fail=0
cases=0

meta() { sed -n "s|^// $1: ||p" "$2"; }

judge() { # judge <fixture> <graphs?> <want-exit> <want-reds-file> <label>
  local f="$1" g="$2" want="$3" reds="$4" label="$5" abi tags out rc args=()
  abi=$(meta abi "$HERE/$f")
  tags=$(meta tags "$HERE/$f")
  args=(--tables "$TABLES" --abi "$abi" --work "$SCRATCH/work.$cases")
  [ -n "$tags" ] && args+=(--tags "$tags")
  if [ "$g" = yes ]; then args+=(--graphs "$GRAPHS" --tags "$TAGS"); fi
  out="$SCRATCH/out.$cases"
  # shellcheck disable=SC2086 # EXTRA is a flag list
  (cd "$HERE" && "$GATE" "${args[@]}" -- "$CC" -Wall -Wextra -Werror -O2 -Wp,-D_FORTIFY_SOURCE=2 -fstack-protector-strong -Ifixtures $EXTRA "$f") \
    > "$out" 2> "$out.err"
  rc=$?
  cases=$((cases + 1))
  grep '^RT-' "$out" | sort > "$out.got"
  sort "$reds" > "$out.want"
  if [ "$rc" != "$want" ] || ! cmp -s "$out.got" "$out.want"; then
    echo "selftest: FAIL $label: exit $rc (want $want)"
    diff "$out.want" "$out.got" | sed 's/^/selftest:   /'
    grep -v '^RT-\|^SUMMARY' "$out" | head -5 | sed 's/^/selftest:   /'
    tail -3 "$out.err" | sed 's/^/selftest:   /'
    fail=1
  else
    echo "selftest: ok   $label: exit $rc, $(wc -l < "$out.got") red"
  fi
}

# the planted fixtures first, then their clean twins
for f in $(cd "$HERE" && ls fixtures/*.c | grep -v '\.clean\.c$') $(cd "$HERE" && ls fixtures/*.clean.c); do
  needs=$(meta needs "$HERE/$f")
  if [ "$needs" = graphs ] && [ -z "$GRAPHS" ]; then continue; fi
  meta expect-red "$HERE/$f" > "$SCRATCH/reds"
  judge "$f" "$([ "$needs" = graphs ] && echo yes || echo no)" "$(meta expect "$HERE/$f")" "$SCRATCH/reds" "$f"
  if [ "$needs" = graphs ] && grep -q '^// without-graphs-red: ' "$HERE/$f"; then
    meta without-graphs-red "$HERE/$f" > "$SCRATCH/reds"
    judge "$f" no 1 "$SCRATCH/reds" "$f without the shipped graphs"
  fi
done

# the load refusals: the shipped tables load; each planted row is refused, exit 2
refused() { # refused <label> <file> <row>
  local t="$SCRATCH/tables.$cases" rc
  cp -r "$TABLES" "$t"
  if [ "$2" = forbidden.tbl ] && [ "$3" = EMPTY ]; then : > "$t/forbidden.tbl"; else printf '%s\n' "$3" >> "$t/$2"; fi
  "$GATE" --tables "$t" --check-tables > "$t.out" 2>&1
  rc=$?
  cases=$((cases + 1))
  if [ "$rc" = 2 ] && grep -q '^REFUSED ' "$t.out"; then
    echo "selftest: ok   refused at load: $1 ($(grep -m1 '^REFUSED ' "$t.out" | sed 's/^REFUSED [^ ]*: //'))"
  else
    echo "selftest: FAIL $1 was not refused at load (exit $rc)"
    sed 's/^/selftest:   /' "$t.out" | head -5
    fail=1
  fi
}
if "$GATE" --tables "$TABLES" --check-tables > "$SCRATCH/tables.out" 2>&1; then
  echo "selftest: ok   the shipped tables load"
else
  echo "selftest: FAIL the shipped tables do not load"; sed 's/^/selftest:   /' "$SCRATCH/tables.out"; fail=1
fi
refused "a waiver naming a site" classes.tbl "fixtures/alloc-run.c:15 maths externals-arm:sinf planted"
refused "a waiver naming a field" classes.tbl "p->on_block maths externals-arm:sinf planted"
refused "a waiver naming a file" classes.tbl "alloc-run.c maths externals-arm:sinf planted"
refused "a waiver naming a function of ours" classes.tbl "omx_biquad_run maths externals-arm:sinf planted"
refused "a known-red row" classes.tbl "snprintf known-red externals-arm:snprintf planted"
refused "a cannot-prove row" classes.tbl "snprintf cannot-prove externals-arm:snprintf planted"
refused "a row without a reason" classes.tbl "cosf maths externals-arm:cosf"
refused "a row without the arm that measured it" classes.tbl "cosf maths"
refused "a duplicate row" classes.tbl "$(grep -m1 '^[a-z_]* maths ' "$TABLES/classes.tbl")"
refused "a row for a forbidden symbol" classes.tbl "malloc bulk-memory externals-arm:malloc planted"
refused "a forbidden row outside the closed codes" forbidden.tbl "snprintf RT-MAYBE planted"
refused "an empty forbidden list" forbidden.tbl EMPTY
refused "a per-consumer list" consumer.tbl "snprintf maths externals-arm:snprintf a consumer's own"
refused "a duplicate ABI role" lv2.abi "plugin LV2_Descriptor run audio"

if [ "$fail" = 0 ]; then
  echo "selftest: green, $cases cases"
else
  echo "selftest: RED"
fi
exit "$fail"
