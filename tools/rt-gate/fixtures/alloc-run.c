/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* A planted malloc in an LV2 run, one call below it, behind a macro: red. */
// abi: lv2
// expect: 1
// expect-red: RT-ALLOC malloc at fixtures/alloc-run.c:15:16 via run[audio] -> grow
// expect-red: RT-ALLOC free at fixtures/alloc-run.c:15:42 via run[audio] -> grow
#include <stdlib.h>
#include "lv2-mini.h"
#define SCRATCH(n) malloc(n)
struct plug { float *out; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void grow(struct plug *p, uint32_t n) {
  (void)p;
  float *tmp = SCRATCH(n * sizeof *tmp); free(tmp);
}
static void run(LV2_Handle h, uint32_t n) { grow(h, n); }
FIXTURE_DESCRIPTOR(run, connect_port, 0)
