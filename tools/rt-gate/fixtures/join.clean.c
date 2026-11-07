/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* The clean twin of join.c: run oversamples (an audio function of a compiled unit, walked through
 * the shipped graph), work runs the detector. Without the shipped graphs both are unclassified
 * externals: the join is what judges them. */
// abi: lv2
// needs: graphs
// expect: 0
// without-graphs-red: RT-EXTERNAL omx_oversampler_up at fixtures/join.clean.c:18:3 via run[audio]
// without-graphs-red: RT-EXTERNAL omx_fbs_push at fixtures/join.clean.c:22:3 via work[worker]
#include <omxdsp/analysis/omx_fbs_detect.h>
#include <omxdsp/omx_oversampler.h>
#include "lv2-mini.h"
struct plug { float *out; struct omx_oversampler os; float up[256]; OmxFbsDetector det; double mags[64]; OmxFbsResult res; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void run(LV2_Handle h, uint32_t n) {
  struct plug *p = h;
  omx_oversampler_up(&p->os, p->out, n, p->up);
}
static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh, uint32_t size, const void *data) {
  struct plug *p = h; (void)respond; (void)rh; (void)size; (void)data;
  omx_fbs_push(&p->det, p->mags, 64, &p->res);
  return LV2_WORKER_SUCCESS;
}
static const LV2_Worker_Interface worker = {work, 0, 0};
static const void *ext(const char *uri) { (void)uri; return &worker; }
FIXTURE_DESCRIPTOR(run, connect_port, ext)
