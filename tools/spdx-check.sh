#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Every tracked source file names its licence in an SPDX line within its first five lines.
# Files that cannot carry a comment (the licence text, Debian's control files, the golden digests)
# are listed here and nowhere else.
set -euo pipefail
cd "$(dirname "$0")/.."
exempt='^(LICENSE|debian/(changelog|control|copyright|source/format|libomxdsp-dev\.docs)|test/golden/.*\.sha256)$'
missing=0
while IFS= read -r f; do
  [[ "$f" =~ $exempt ]] && continue
  if ! head -5 "$f" | grep -q 'SPDX-License-Identifier: GPL-3.0-or-later'; then
    echo "spdx-check: $f carries no SPDX line" >&2
    missing=1
  fi
done < <(git ls-files)
[[ $missing == 0 ]] && echo "spdx-check: every source file carries its SPDX line"
exit "$missing"
