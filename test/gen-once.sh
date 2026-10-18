#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# gen-once.sh <scratch-dir> — tools/kernels-gen.sh runs ONCE under `make -j8`, however many targets
# read build/kernels-*.inc.c (FreeMixer/omx-dsp#163).
#
# The Makefile's KERNELS_GEN is replaced by a wrapper that appends one line to a log and then runs the
# real generator; the racing targets (the suite binary and perturb, which both read the files) are
# built together in a scratch BUILD under -j8, and the log must hold exactly ONE line. The sabotage:
# a copy of the Makefile whose suite recipe calls the generator again (the bug) must hold MORE than
# one line, or this script fails — a count that cannot turn red proves nothing.
# Also asserts test-fx's per-kernel split runs every binary the old loop ran (the FX_BINS set).
# Usage: CC=… CFLAGS=… gen-once.sh <scratch-dir>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"
OUT="${1:?scratch dir}"
rm -rf "$OUT" && mkdir -p "$OUT"
cd "$PKG"

cat > "$OUT/gen-counted.sh" <<EOF
#!/usr/bin/env bash
echo "gen \$\$ \$*" >> "$OUT/count.log"
exec bash "$PKG/tools/kernels-gen.sh" "\$@"
EOF

# count_runs <makefile> <label> <tolerate>: a fresh build dir, the racing targets under -j8, the log's line count.
# A build failure is reported on stderr (stdout is the count). The sabotaged Makefile builds the very
# race the test counts: a second generator run rewrites the lists while the other target compiles them,
# so that build may fail to compile; only its count matters, so it passes <tolerate>=1.
count_runs() {
  local mk="$1" b="$OUT/b-$2" tolerate="${3:-0}"
  : > "$OUT/count.log"
  rm -rf "$b"
  if ! nice -n 10 make -f "$mk" -j8 BUILD="$b" KERNELS_GEN="bash $OUT/gen-counted.sh" \
    "$b/omxdsp_suite" perturb > "$OUT/$2.out" 2>&1 && [ "$tolerate" -eq 0 ]; then
    echo "gen-once.sh: FAIL — the $2 build failed:" >&2
    tail -20 "$OUT/$2.out" >&2
    exit 1
  fi
  wc -l < "$OUT/count.log"
}

n="$(count_runs Makefile real)"
[ "$n" -eq 1 ] || { echo "gen-once.sh: FAIL — the generator ran $n times under make -j8, wanted exactly 1"; exit 1; }

# Sabotage: re-add the generator call to the suite recipe, as the bug had it.
sed 's|^\t\$(CC) \$(TESTFLAGS) -I\$(BUILD) -o \$@ test/omxdsp_suite.c|\t$(KERNELS_GEN) $(BUILD)\n&|' Makefile > "$OUT/Makefile.sab"
cmp -s Makefile "$OUT/Makefile.sab" && { echo "gen-once.sh: FAIL — the sabotage changed nothing"; exit 1; }
s="$(count_runs "$OUT/Makefile.sab" sab 1)"
[ "$s" -gt 1 ] || { echo "gen-once.sh: FAIL — the sabotaged Makefile (generator re-added to a recipe) ran it $s time(s): the count is blind"; exit 1; }

# test-fx's split must run every binary of FX_BINS (the loops' set), each as a recipe command. Both
# reads say --no-print-directory: under a parent make (debian/rules, make test) the sub-make's
# 'Entering directory' lines would otherwise land in the lists.
want="$(make -s --no-print-directory --eval='print-fx-bins: ; @echo $(FX_BINS)' print-fx-bins | tr ' ' '\n' | sed 's|^build/||' | sort)"
got="$(make -n --no-print-directory test-fx 2>/dev/null | grep -oE '^echo "\./build/fx_[a-z0-9_]+' | sed 's|^echo "./build/||' | sort -u)"
missing="$(comm -23 <(echo "$want" | sort -u) <(echo "$got"))"
[ -z "$missing" ] || { echo "gen-once.sh: FAIL — test-fx does not run:"; echo "$missing"; exit 1; }
echo "gen-once.sh: the generator ran once under make -j8 ($n), the sabotaged Makefile ran it $s times, and test-fx runs every FX binary"
