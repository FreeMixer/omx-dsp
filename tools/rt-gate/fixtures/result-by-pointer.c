/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* The worker's result passed by pointer: work hands the address of a struct carrying a pointer,
 * work_response keeps the body pointer, and reads it twice. Red, each. */
// abi: lv2
// expect: 1
// expect-red: RT-RESULT &l at fixtures/result-by-pointer.c:18:32 via work[worker]
// expect-red: RT-RESULT body at fixtures/result-by-pointer.c:22:13 via work_response[audio]
// expect-red: RT-RESULT body at fixtures/result-by-pointer.c:23:37 via work_response[audio]
#include "lv2-mini.h"
struct level { float value; const float *frames; };
struct plug { float *out; const void *last; float level; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void run(LV2_Handle h, uint32_t n) { (void)h; (void)n; }
static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh, uint32_t size, const void *data) {
  struct plug *p = h; (void)size; (void)data;
  struct level l = {1.0f, p->out};
  return respond(rh, sizeof l, &l);
}
static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void *body) {
  struct plug *p = h; (void)size;
  p->last = body;
  p->level = ((const struct level *)body)->value;
  return LV2_WORKER_SUCCESS;
}
static const LV2_Worker_Interface worker = {work, work_response, 0};
static const void *ext(const char *uri) { (void)uri; return &worker; }
FIXTURE_DESCRIPTOR(run, connect_port, ext)
