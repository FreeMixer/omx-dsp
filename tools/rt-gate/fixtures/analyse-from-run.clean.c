/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* The clean twin of analyse-from-run.c: the pass runs in work, its result crosses by value. */
// abi: lv2
// tags: fixtures/fixture.tags
// expect: 0
#include <string.h>
#include "lv2-mini.h"
#include "fixture-dsp.h"
struct level { float value; uint32_t frames; };
struct plug { float *out; float level; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void run(LV2_Handle h, uint32_t n) { struct plug *p = h; p->out[0] = fixture_gain(p->out[0], (float)n); }
static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh, uint32_t size, const void *data) {
  struct plug *p = h; (void)size; (void)data;
  struct level l = {fixture_analyse(p->out, 64), 64};
  return respond(rh, sizeof l, &l);
}
static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void *body) {
  struct level l; (void)size;
  memcpy(&l, body, sizeof l);
  ((struct plug *)h)->level = l.value;
  return LV2_WORKER_SUCCESS;
}
static const LV2_Worker_Interface worker = {.work = work, .work_response = work_response};
static const void *ext(const char *uri) { (void)uri; return &worker; }
FIXTURE_DESCRIPTOR(run, connect_port, ext)
