#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# gate-cost-ab.sh <base-rev> [rounds=5] [quantum=128] [blocks=2000] — the gate kernel's cost BEFORE
# against AFTER: tools/kernel-cost-ab.sh over tools/gate-cost-bench.c.
set -euo pipefail
exec "$(dirname "$0")/kernel-cost-ab.sh" gate-cost-bench "$@"
