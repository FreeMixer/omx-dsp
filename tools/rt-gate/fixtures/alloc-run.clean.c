/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* The clean twin of alloc-run.c: the buffer is the host's, the maths classified. */
// abi: lv2
// expect: 0
#include <math.h>
#include <string.h>
#include "lv2-mini.h"
struct plug { float *out; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void fill(struct plug *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) p->out[i] = sinf((float)i);
  memset(p->out, 0, n * sizeof *p->out);
}
static void run(LV2_Handle h, uint32_t n) { fill(h, n); }
FIXTURE_DESCRIPTOR(run, connect_port, 0)
