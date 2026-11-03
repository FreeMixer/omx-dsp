#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# fastmath-probe-selftest.sh — the build-time probe says nothing when libmvec is not really there.
#
#   tools/fastmath-probe-selftest.sh CC    three toolchains that each lack one thing the probe tests for, and the
#                                          probe must answer nothing for each; the real CC is the control and
#                                          must find at least one function when it can (then the sabotages mean something)
#
#   no-libmvec     a compiler that fails when asked to link -lmvec          (Debian 12 on arm64)
#   no-pragma      a compiler that ignores -fopenmp-simd                    (a toolchain with no OpenMP simd)
#   no-symbol      a compiler whose assembly names no _ZGV symbol           (a libmvec header the compiler does not read)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${1:-cc}"
T="$(mktemp -d "${TMPDIR:-/tmp}/fastmath-probe-selftest.XXXXXX")"
trap 'rm -rf "$T"' EXIT
real="$(command -v "$CC")"
cat > "$T/no-libmvec" <<SH
#!/bin/sh
for a in "\$@"; do [ "\$a" = -lmvec ] && exit 1; done
exec $real "\$@"
SH
cat > "$T/no-pragma" <<SH
#!/bin/sh
exec $real \$(for a in "\$@"; do [ "\$a" = -fopenmp-simd ] || printf '%s\n' "\$a"; done)
SH
cat > "$T/no-symbol" <<SH
#!/bin/sh
$real "\$@" || exit \$?
out=""; prev=""; asm=0
for a in "\$@"; do [ "\$prev" = -o ] && out="\$a"; [ "\$a" = -S ] && asm=1; prev="\$a"; done
[ "\$asm" = 1 ] && sed -i 's/_ZGV[A-Za-z0-9]*_/_nozgv_/' "\$out"
exit 0
SH
chmod +x "$T/no-libmvec" "$T/no-pragma" "$T/no-symbol"
control="$(sh "$HERE/fastmath-probe.sh" "$CC")"
rc=0
for s in no-libmvec no-pragma no-symbol; do
  got="$(sh "$HERE/fastmath-probe.sh" "$T/$s")"
  if [ -n "$control" ] && [ -n "$got" ]; then echo "fastmath-probe-selftest: $s: the probe still found [$got]"; rc=1; fi
done
if [ -z "$control" ]; then echo "fastmath-probe-selftest: this toolchain has no libmvec function: the plain path, nothing to sabotage"; exit 0; fi
[ "$rc" = 0 ] && echo "fastmath-probe-selftest: found [$control]; with no libmvec, no pragma, or no vector symbol the probe finds nothing"
exit $rc
