/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* A planted malloc in work_response and in end_run (audio), and a printf in work (the worker takes
 * the audio role's forbidden set on another thread): red, all three. */
// abi: lv2
// expect: 1
// expect-red: RT-ALLOC malloc at fixtures/worker-alloc.c:23:13 via work_response[audio]
// expect-red: RT-ALLOC free at fixtures/worker-alloc.c:27:3 via end_run[audio] -> drain
// expect-red: RT-ALLOC malloc at fixtures/worker-alloc.c:28:13 via end_run[audio] -> drain
// expect-red: RT-LOG printf at fixtures/worker-alloc.c:18:3 via work[worker]
#include <stdio.h>
#include <stdlib.h>
#include "lv2-mini.h"
struct plug { float *out; void *keep; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void run(LV2_Handle h, uint32_t n) { (void)h; (void)n; }
static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh, uint32_t size, const void *data) {
  printf("analysed %u\n", size);
  (void)h; (void)respond; (void)rh; (void)data; return LV2_WORKER_SUCCESS;
}
static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void *body) {
  struct plug *p = h; (void)body;
  p->keep = malloc(size);
  return LV2_WORKER_SUCCESS;
}
static void drain(struct plug *p) {
  free(p->keep);
  p->keep = malloc(1);
}
static LV2_Worker_Status end_run(LV2_Handle h) { drain(h); return LV2_WORKER_SUCCESS; }
static const LV2_Worker_Interface worker = {work, work_response, end_run};
static const void *ext(const char *uri) { (void)uri; return &worker; }
FIXTURE_DESCRIPTOR(run, connect_port, ext)
