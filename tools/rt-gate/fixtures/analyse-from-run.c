/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* The analysis pass, a worker function, reached from run: red. */
// abi: lv2
// tags: fixtures/fixture.tags
// expect: 1
// expect-red: RT-THREAD fixture_analyse at fixtures/analyse-from-run.c:14:14 via run[audio] -> step
#include "lv2-mini.h"
#include "fixture-dsp.h"
struct plug { float *out; float level; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void step(struct plug *p, uint32_t n) {
  p->out[0] = fixture_gain(p->out[0], 0.5f);
  p->level = fixture_analyse(p->out, n);
}
static void run(LV2_Handle h, uint32_t n) { step(h, n); }
FIXTURE_DESCRIPTOR(run, connect_port, 0)
