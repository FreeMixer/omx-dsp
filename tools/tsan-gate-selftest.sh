#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# tsan-gate-selftest.sh — a red tsan-gate.sh run keeps the WHOLE ThreadSanitizer report: the
# `WARNING: ThreadSanitizer: data race` header, the access stack, the previous-access stack and the
# SUMMARY, in what it prints AND in the log it leaves. A batch-4 container run (2026-10-02) kept
# only the tail of a mix_split_identity report — the summary and the thread's creation — so the
# racing access and its stack were lost and the race had to be found again.
#
# A fake compiler stands in for cc: the probe it "links" exits 0, the test it "links" prints 80
# lines of ordinary output and then one report whose two stacks are 30 frames each, and exits 66.
# The printed `tsan-gate: report:` block is judged too: the FIRST report's header, kind and top frames,
# the report count, nothing for a clean log, and the 40-line / 200-character bound.
# Nothing is compiled; the gate's own logic is what is judged. Exits 0 when every part survives.
set -uo pipefail
GATE="$(cd "$(dirname "$0")" && pwd)/tsan-gate.sh"
T="$(mktemp -d "${TMPDIR:-/tmp}/tsan-gate-selftest.XXXXXX")" || exit 2
trap 'rm -rf "$T"' EXIT

cat > "$T/fakecc" << 'EOF'
#!/usr/bin/env bash
out=""; while [ $# -gt 0 ]; do [ "$1" = -o ] && { out="$2"; shift; }; shift; done
[ -n "$out" ] || exit 1
case "$out" in
  */probe) printf '#!/bin/sh\nexit 0\n' > "$out" ;;
  *) cat > "$out" << 'BIN'
#!/usr/bin/env bash
for i in $(seq 80); do echo "ok arm line $i"; done
n="${FAKE_REPORTS:-1}"; f="${FAKE_FRAMES:-30}"
for r in $(seq "$n"); do
echo "=================="
echo "WARNING: ThreadSanitizer: data race (pid=$r)"
echo "  Write of size 8 at 0x7f0000000000 by main thread:"
for i in $(seq 0 $((f - 1))); do echo "    #$i writer_frame_$i src/fake.c:$((100 + i))"; done
echo ""
[ -n "${FAKE_LONG:-}" ] && echo "    #99 long_frame $(printf 'y%.0s' $(seq 300))"
echo "  Previous read of size 8 at 0x7f0000000000 by thread T9:"
for i in $(seq 0 $((f - 1))); do echo "    #$i reader_frame_$i src/fake.c:$((200 + i))"; done
echo ""
echo "SUMMARY: ThreadSanitizer: data race src/fake.c:$((100 * r)) in writer_frame_0"
echo "=================="
done
[ "$n" -eq 0 ] && exit 0
exit 66
BIN
     ;;
esac
chmod +x "$out"
EOF
chmod +x "$T/fakecc"

out="$(CC="$T/fakecc" bash "$GATE" "$T/out" fake FAKE_SABOTAGE fake.c 2>&1)"; rc=$?
fails=0
need() { # <what> <fixed string> <text>
  if ! printf '%s\n' "$3" | grep -qF -- "$2"; then echo "tsan-gate-selftest: FAIL — $1 lost: '$2'"; fails=$((fails + 1)); fi
}
[ "$rc" -ne 0 ] || { echo "tsan-gate-selftest: FAIL — the gate passed a raced run (rc 0)"; fails=$((fails + 1)); }
for where in printed log; do
  if [ "$where" = log ]; then
    [ -f "$T/out/fake.log" ] || { echo "tsan-gate-selftest: FAIL — no log at <scratch>/fake.log"; fails=$((fails + 1)); continue; }
    text="$(cat "$T/out/fake.log")"
  else text="$out"; fi
  need "$where: the header" "WARNING: ThreadSanitizer: data race" "$text"
  need "$where: the access stack's top" "#0 writer_frame_0 src/fake.c:100" "$text"
  need "$where: the access stack's bottom" "#29 writer_frame_29 src/fake.c:129" "$text"
  need "$where: the previous access" "Previous read of size 8" "$text"
  need "$where: the previous access stack's top" "#0 reader_frame_0 src/fake.c:200" "$text"
  need "$where: the summary" "SUMMARY: ThreadSanitizer: data race src/fake.c:100" "$text"
done
# the report that travels: `tsan-gate: report:` lines in the gate's own stdout, first report only
rep="$(printf '%s\n' "$out" | grep '^tsan-gate: report: ')"
nrep="$(printf '%s\n' "$rep" | wc -l)"
[ "$nrep" -le 41 ] || { echo "tsan-gate-selftest: FAIL — the report block is $nrep lines, the bound is 40 + the count"; fails=$((fails + 1)); }
need "long report: the count line" "tsan-gate: report: 1 ThreadSanitizer report(s)" "$rep"
need "long report: the header" "tsan-gate: report: WARNING: ThreadSanitizer: data race (pid=1)" "$rep"
need "long report: the top frame" "tsan-gate: report:     #0 writer_frame_0 src/fake.c:100" "$rep"
if printf '%s\n' "$rep" | grep -qF "#29 reader_frame_29"; then echo "tsan-gate-selftest: FAIL — a report longer than the bound was not cut"; fails=$((fails + 1)); fi
if printf '%s\n' "$out" | grep -nB1 -m1 '^tsan-gate: FAIL' | grep -q 'report: '; then :; else
  echo "tsan-gate-selftest: FAIL — the report block does not sit immediately before the FAIL line"; fails=$((fails + 1)); fi

out2="$(FAKE_REPORTS=2 FAKE_FRAMES=3 CC="$T/fakecc" bash "$GATE" "$T/out2" fake FAKE_SABOTAGE fake.c 2>&1)"
rep2="$(printf '%s\n' "$out2" | grep '^tsan-gate: report: ')"
need "two reports: the count" "tsan-gate: report: 2 ThreadSanitizer report(s)" "$rep2"
need "two reports: the first header" "tsan-gate: report: WARNING: ThreadSanitizer: data race (pid=1)" "$rep2"
need "two reports: the first write frame" "report:     #0 writer_frame_0 src/fake.c:100" "$rep2"
need "two reports: the first read frame" "report:     #0 reader_frame_0 src/fake.c:200" "$rep2"
need "two reports: the first summary" "tsan-gate: report: SUMMARY: ThreadSanitizer: data race src/fake.c:100" "$rep2"
if printf '%s\n' "$rep2" | grep -qF "pid=2"; then echo "tsan-gate-selftest: FAIL — the second report was printed"; fails=$((fails + 1)); fi

out3="$(FAKE_REPORTS=1 FAKE_FRAMES=1 CC="$T/fakecc" bash "$GATE" "$T/out3" fake FAKE_SABOTAGE fake.c 2>&1)"
need "sabotage arm: raced and reported" "tsan-gate: report: 1 ThreadSanitizer report(s)" "$out3"

out5="$(FAKE_LONG=1 FAKE_FRAMES=2 CC="$T/fakecc" bash "$GATE" "$T/out5" fake FAKE_SABOTAGE fake.c 2>&1)"
widest="$(printf '%s\n' "$out5" | grep '^tsan-gate: report: ' | awk '{ if (length($0) > m) m = length($0) } END { print m + 0 }')"
[ "$widest" -le $((200 + 19)) ] && [ "$widest" -gt 100 ] || { echo "tsan-gate-selftest: FAIL — a 300-character line is $widest wide in the block, the bound is 200 + prefix"; fails=$((fails + 1)); }

out4="$(FAKE_REPORTS=0 CC="$T/fakecc" bash "$GATE" "$T/out4" fake FAKE_SABOTAGE fake.c 2>&1)"
if printf '%s\n' "$out4" | grep -q '^tsan-gate: report:'; then echo "tsan-gate-selftest: FAIL — a report-free log printed a report line"; fails=$((fails + 1)); fi
if [ "$fails" -ne 0 ]; then echo "tsan-gate-selftest: FAIL — $fails part(s) of the report lost"; exit 1; fi
echo "tsan-gate-selftest: ok — a raced run fails the gate and keeps the whole report, printed and logged, and its first block travels bounded"
