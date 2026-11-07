/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* The clean twin of result-by-pointer.c: a fixed-size, pointer-free result, copied once inside
 * work_response by dereference. */
// abi: lv2
// expect: 0
#include "lv2-mini.h"
struct level { float value; uint32_t frames[4]; };
struct plug { float *out; float level; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void run(LV2_Handle h, uint32_t n) { (void)h; (void)n; }
static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh, uint32_t size, const void *data) {
  (void)h; (void)size; (void)data;
  struct level l = {1.0f, {0, 1, 2, 3}};
  return respond(rh, sizeof l, &l);
}
static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void *body) {
  struct plug *p = h; (void)size;
  struct level l = *(const struct level *)body;
  p->level = l.value;
  return LV2_WORKER_SUCCESS;
}
static const LV2_Worker_Interface worker = {work, work_response, 0};
static const void *ext(const char *uri) { (void)uri; return &worker; }
FIXTURE_DESCRIPTOR(run, connect_port, ext)
