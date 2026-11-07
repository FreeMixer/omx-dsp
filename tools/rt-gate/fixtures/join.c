/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* A consumer joined to omx-dsp's shipped graph: run reaches the feedback detector, a compiled unit's
 * worker function, through a helper of its own. Red. */
// abi: lv2
// needs: graphs
// expect: 1
// expect-red: RT-THREAD omx_fbs_push at fixtures/join.c:16:3 via run[audio] -> detect
#include <omxdsp/analysis/omx_fbs_detect.h>
#include <omxdsp/omx_oversampler.h>
#include "lv2-mini.h"
struct plug { float *out; struct omx_oversampler os; float up[256]; OmxFbsDetector det; double mags[64]; OmxFbsResult res; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void detect(struct plug *p, uint32_t n) {
  omx_oversampler_up(&p->os, p->out, n, p->up);
  omx_fbs_push(&p->det, p->mags, 64, &p->res);
}
static void run(LV2_Handle h, uint32_t n) { detect(h, n); }
FIXTURE_DESCRIPTOR(run, connect_port, 0)
