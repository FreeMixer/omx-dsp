// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * deesser-cost.c — the de-esser's cost row, ns per sample per lane at every declared rate, by the
 * plugin cost method: a sustained 1 kHz sine at −6 dBFS on every input, phase-continuous across
 * blocks, each block of 512 frames timed on its own, the first 64 blocks discarded as warm-up, the
 * distribution of the rest reported. Contracts off, release flags. Two settings: `default` (the
 * contract's default controls, mono, split) and `max` (the same controls on a stereo pair, the
 * most the stage computes per frame). Prints one TSV row per rate and setting: rate, setting,
 * p50, p95 (ns per frame).
 */
#include <omxdsp/omxdsp_definitions.h>
#include <omxdsp/fx/omx_deesser.h>
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLOCK 512u
#define WARMUP 64u
#define BLOCKS (WARMUP + 2048u)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int cmp(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static void atom(struct omx_deess *p, float sr) {
  memset(p, 0, sizeof *p);
  p->enabled = 1;
  p->mode = OMX_DEESS_SPLIT;
  p->dyn.enabled = 1;
  p->dyn.gc.mode = OMX_DYN_ABOVE;
  p->dyn.detect = OMX_DETECT_PEAK;
  p->dyn.gc.thresh_db = (float)OMX_DEESS_THRESHOLD_RANGE_DEFAULT;
  p->dyn.gc.ratio = (float)OMX_DEESS_RATIO_RANGE_DEFAULT;
  p->dyn.gc.knee_db = (float)OMX_DEESS_KNEE_DB;
  p->dyn.gc.range_db = (float)OMX_DEESS_RANGE_RANGE_DEFAULT;
  p->dyn.gc.makeup_lin = 1.0f;
  p->dyn.ovs_mode = OMX_DYN_OVS_OFF;
  p->dyn.attack_ms = (float)OMX_DEESS_ATTACK_RANGE_DEFAULT;
  p->dyn.attack_coeff = omx_pole_from_time_ms((float)OMX_DEESS_ATTACK_RANGE_DEFAULT, sr);
  p->dyn.release_coeff = omx_pole_from_time_ms((float)OMX_DEESS_RELEASE_RANGE_DEFAULT, sr);
  /* The cookbook bandpass at the default centre, one octave wide (the band the control thread designs). */
  const double f0 = (double)OMX_DEESS_FREQ_RANGE_DEFAULT, w0 = 2.0 * M_PI * f0 / (double)sr;
  const double q = 1.0 / (2.0 * sinh(log(2.0) / 2.0 * (double)OMX_DEESS_WIDTH_RANGE_DEFAULT * w0 / sin(w0)));
  const double alpha = sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
  p->bp_c[0] = (float)(alpha / a0);
  p->bp_c[1] = 0.0f;
  p->bp_c[2] = (float)(-alpha / a0);
  p->bp_c[3] = (float)(-2.0 * cos(w0) / a0);
  p->bp_c[4] = (float)((1.0 - alpha) / a0);
}

int main(void) {
  static float l[BLOCK], r[BLOCK];
  static double ns[BLOCKS];
  printf("rate\tsetting\tns_per_frame_p50\tns_per_frame_p95\n");
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (int stereo = 0; stereo <= 1; stereo++) {
      struct omx_deess p;
      atom(&p, sr);
      struct omx_deess_state st;
      omx_deess_state_init(&st);
      const double a = pow(10.0, -6.0 / 20.0), w = 2.0 * M_PI * 1000.0 / (double)sr;
      uint64_t t = 0;
      volatile float sink = 0.0f;
      for (uint32_t b = 0; b < BLOCKS; b++) {
        for (uint32_t i = 0; i < BLOCK; i++, t++) l[i] = r[i] = (float)(a * sin(w * (double)(t % (uint64_t)sr)));
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        omx_deess_process(l, stereo ? r : NULL, BLOCK, &p, &st);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        sink += l[BLOCK - 1u];
        ns[b] = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / BLOCK;
      }
      (void)sink;
      qsort(ns + WARMUP, BLOCKS - WARMUP, sizeof ns[0], cmp);
      const uint32_t m = BLOCKS - WARMUP;
      printf("%.0f\t%s\t%.2f\t%.2f\n", (double)sr, stereo ? "max" : "default", ns[WARMUP + m / 2u],
             ns[WARMUP + (m * 95u) / 100u]);
    }
  }
  return 0;
}
