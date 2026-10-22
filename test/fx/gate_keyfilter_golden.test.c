// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The gate's key filter as golden digests at the nine oracle rates: the four renders of
 * gate_instance_golden.test.c with the key filter's edges in —
 *   1  keyed, the key band 120 Hz .. 6 kHz;
 *   2  the key connected but SELF chosen: the self key (l + r) / 2 high-passed at 1 kHz;
 *   3  no key, bypass toggled every other host block: the self key low-passed at 300 Hz, its
 *      history held across the bypassed blocks;
 *   4  every port outside its travel, the high-pass corner past the travel's roof (held at
 *      20 kHz, then at 0.999 Nyquist where that is lower) and the low-pass NaN (out).
 *
 *   make test-fx                                compare
 *   build/fx_gate_keyfilter_golden --write      print the lines test/golden/gate_keyfilter.sha256 holds
 */
#include <math.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1 /* held at the nine oracle rates */
static const float key_hp[] = {120.0f, 1000.0f, 0.0f, 30000.0f};
static const float key_lp[] = {6000.0f, 0.0f, 300.0f, NAN};
#define GATE_GOLDEN_HP(which) (key_hp[(which)])
#define GATE_GOLDEN_LP(which) (key_lp[(which)])
#define GATE_GOLDEN_NAME "gate_keyfilter"
#include "gate_instance_golden.test.c"
