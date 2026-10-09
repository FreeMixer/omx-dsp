// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The gate's knee range at its neutral setting, as golden digests: the four renders of
 * gate_instance_golden.test.c with the knee start equal to its end somewhere other than the
 * threshold — at the travel's floor, at its roof, between, and past the roof (clamped onto it) —
 * at every golden rate. test/golden/gate_knee.sha256 is a link to gate_instance.sha256, the
 * digests the gate had before it took a knee range: a zero-width knee is that gate, bit for bit,
 * wherever the pair sits.
 *
 *   make test-fx                                compare
 */
static const float knee_at[] = {-80.0f, 0.0f, -12.5f, 37.0f};
#define GATE_GOLDEN_KNEE(which, t) ((void)(t), knee_at[(which)])
#define GATE_GOLDEN_NAME "gate_knee"
#include "gate_instance_golden.test.c"
