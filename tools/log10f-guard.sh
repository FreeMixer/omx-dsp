#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# log10f-guard — omx_log10f lives in the tests only (operator ruling 2026-10-08, FreeMixer/omx-dsp#17):
# production calls the libm's log10f. Fails when the name appears in any file under the directories
# named on the command line (make lint passes include and src).
set -u
[ $# -gt 0 ] || { echo "usage: log10f-guard.sh <dir>..." >&2; exit 2; }
hits=$(grep -rn --include='*.h' --include='*.c' --include='*.inc' -w 'omx_log10f' "$@" 2>/dev/null)
if [ -n "$hits" ]; then
  echo "log10f-guard: omx_log10f is test support only; production calls the libm's log10f:" >&2
  echo "$hits" >&2
  exit 1
fi
echo "log10f-guard: no omx_log10f under $*"
