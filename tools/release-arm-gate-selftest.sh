#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# release-arm-gate-selftest.sh — release.yml's arch-matrix decides, per job, whether the arm64/aarch64
# side of the matrix builds. A tag push carries no `inputs` at all, so inputs.arm is null there, and
# null == false is true in a GitHub expression: a gate that asks only whether inputs.arm is false drops
# the arm leg on every tag. The gate tests the event first, so a push always builds the arm leg.
# This checks the gate release.yml carries, not a copy of it: each job's arch-matrix line must be the
# `(github.event_name != '<event>' && inputs.arm == false) && '<no-arm matrix>' || '<arm matrix>'` shape,
# the arm matrix must carry the job's arm architecture and the no-arm one must not, and the five
# contexts a real run can pass it — a tag push (no inputs at all), workflow_dispatch's default,
# workflow_dispatch opting in, the ci.yml dry run (workflow_call, arm: false) and a plain
# pull_request — must each resolve to the matrix this gate is meant to produce.
set -uo pipefail
cd "$(dirname "$0")/.."
REL=.github/workflows/release.yml
fails=0

extract() { # <job> -> sets EVENT_LIT, ARM_ARR, NOARM_ARR; fails if the line is not the gate shape
  local line
  line="$(awk -v job="$1" '
    $0 ~ "^  " job ":" { injob = 1 }
    injob && /^  [a-z]+:$/ && $0 !~ "^  " job ":" { injob = 0 }
    injob && /arch-matrix:/ { print; exit }
  ' "$REL")"
  if [[ "$line" =~ \(github\.event_name\ !=\ \'([^\']*)\'\ \&\&\ inputs\.arm\ ==\ false\)\ \&\&\ \'([^\']*)\'\ \|\|\ \'([^\']*)\' ]]; then
    EVENT_LIT="${BASH_REMATCH[1]}"; NOARM_ARR="${BASH_REMATCH[2]}"; ARM_ARR="${BASH_REMATCH[3]}"
    return 0
  fi
  EVENT_LIT=""; ARM_ARR=""; NOARM_ARR=""
  return 1
}

gate() { # <event> <inputs.arm: true|false|null> -> "arm" or "noarm", same rule the expression encodes
  [ "$1" = push ] && { echo arm; return; }
  [ "$2" = true ] && { echo arm; return; }
  echo noarm
}

check_job() { # <job> <arm architecture name>
  local job="$1" arch="$2"
  if ! extract "$job"; then
    echo "release-arm-gate-selftest: FAIL — $job's arch-matrix is not the (event!='push' && inputs.arm==false) gate"
    fails=$((fails + 1)); return
  fi
  [ "$EVENT_LIT" = push ] || {
    echo "release-arm-gate-selftest: FAIL — $job gates on event '$EVENT_LIT', not 'push'"
    fails=$((fails + 1))
  }
  [[ "$ARM_ARR" == *"$arch"* ]] || {
    echo "release-arm-gate-selftest: FAIL — $job's arm matrix '$ARM_ARR' lacks $arch"
    fails=$((fails + 1))
  }
  [[ "$NOARM_ARR" != *"$arch"* ]] || {
    echo "release-arm-gate-selftest: FAIL — $job's no-arm matrix '$NOARM_ARR' still carries $arch"
    fails=$((fails + 1))
  }

  while read -r event arm want; do
    got="$(gate "$event" "$arm")"
    if [ "$got" != "$want" ]; then
      echo "release-arm-gate-selftest: FAIL — $job: event=$event inputs.arm=$arm resolved $got, want $want"
      fails=$((fails + 1))
    fi
  done << 'EOF'
push null arm
workflow_dispatch false noarm
workflow_dispatch true arm
workflow_call false noarm
pull_request null noarm
EOF
}

check_job rpm aarch64
check_job deb arm64

if [ "$fails" -ne 0 ]; then
  echo "release-arm-gate-selftest: FAIL — $fails part(s) of the arm gate wrong"
  exit 1
fi
echo "release-arm-gate-selftest: ok — rpm and deb arch-matrix build arm on every tag push, opt-in only on dispatch"
