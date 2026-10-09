// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * phaser-cost.c — omx_phaser_process's cost alone, ns per sample, at every declared rate and at the
 * two settings a stage-cost cell carries: `default` (the contract's come-up values: 0.5 Hz, base
 * 200 Hz, depth 4 oct, 6 sections, feedback +0.4, mix 50 %) and `max` (the costliest legal setting:
 * OMX_PHASER_MAX_STAGES sections, the deepest and fastest sweep, feedback at the clamp, fully wet).
 * Contracts off. Timed 512-sample blocks of stereo noise, 200 warm-up blocks discarded, 9 runs x
 * 2000 blocks, the median run kept. Kernel alone, not an engine's walk.
 *
 * Prints one TSV row per rate: rate, default, max. Exits 1 when an output sample is not finite, or
 * when the max setting does not cost more than the default at some rate (the section count is the
 * one control that moves the cost, so a max that is not dearer measured the wrong setting).
 *
 * Measured 2026-10-01 by this method, release build, one pinned P-core of an Intel Core Ultra 9
 * 285K (governor powersave): 17.435 / 28.364 at 44 100, 17.033 / 27.960 at
 * 48 000, 15.610 / 26.380 at 88 200, 15.547 / 26.299 at 96 000, 14.786 / 25.621 at 176 400 and
 * 14.732 / 25.558 at 192 000 (default / max). The cost falls slightly with the rate: the control
 * interval is sr/6000, so the per-control-point tan and exp2f are spread over more samples.
 */
#include <omxdsp/omxdsp.h>
#include <omxdsp/fx/omx_phaser.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define Q 512u
#define SRC_BLOCKS 64u
#define WARMUP 200
#define RUNS 9
#define BLOCKS 2000

static float g_src_l[Q * SRC_BLOCKS], g_src_r[Q * SRC_BLOCKS];

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void noise(void) {
  uint32_t x = 0x12345678u;
  for (uint32_t i = 0; i < Q * SRC_BLOCKS; i++) {
    x = x * 1664525u + 1013904223u;
    g_src_l[i] = 0.5f * ((float)(x >> 8) / 8388608.0f - 1.0f);
    x = x * 1664525u + 1013904223u;
    g_src_r[i] = 0.5f * ((float)(x >> 8) / 8388608.0f - 1.0f);
  }
}

static int cmp(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/* The median run's ns/sample at `sr`; *finite goes 0 when an output sample is not finite. */
static double median_ns(int max, float sr, int *finite) {
  static float l[Q], r[Q];
  const struct omx_phaser p = {
    .enabled = 1,
    .stages = max ? OMX_PHASER_MAX_STAGES : OMX_PHASER_STAGES_RANGE_DEFAULT,
    .base_hz = (float)OMX_PHASER_BASE_RANGE_DEFAULT,
    .depth_oct = max ? (float)OMX_PHASER_DEPTH_RANGE_MAX : (float)OMX_PHASER_DEPTH_RANGE_DEFAULT,
    .lfo_inc = omx_lfo_inc(max ? (float)OMX_PHASER_RATE_RANGE_MAX : OMX_PHASER_RATE_RANGE_DEFAULT, sr),
    .feedback = max ? OMX_PHASER_FB_MAX : OMX_PHASER_FEEDBACK_RANGE_DEFAULT,
    .mix = max ? 1.0f : (float)OMX_PHASER_MIX_RANGE_DEFAULT / 100.0f,
  };
  struct omx_phaser_state st;
  omx_phaser_state_init(&st);
  double runs[RUNS];
  for (int b = -WARMUP; b < RUNS * BLOCKS; b++) {
    const uint32_t at = Q * ((uint32_t)(b + WARMUP) % SRC_BLOCKS);
    memcpy(l, g_src_l + at, sizeof l);
    memcpy(r, g_src_r + at, sizeof r);
    const double t0 = now_ns();
    omx_phaser_process(l, r, Q, &p, &st, sr);
    const double dt = now_ns() - t0;
    if (!isfinite(l[Q - 1]) || !isfinite(r[Q - 1])) *finite = 0;
    if (b >= 0) {
      if (b % BLOCKS == 0) runs[b / BLOCKS] = 0.0;
      runs[b / BLOCKS] += dt / ((double)BLOCKS * Q);
    }
  }
  qsort(runs, RUNS, sizeof runs[0], cmp);
  return runs[RUNS / 2];
}

int main(void) {
  noise();
  int ok = 1;
  printf("# phaser-cost: quantum %u, %d runs x %d blocks, median ns/sample\n", Q, RUNS, BLOCKS);
  printf("rate\tdefault\tmax\n");
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    int finite = 1;
    const double d = median_ns(0, sr, &finite), m = median_ns(1, sr, &finite);
    printf("%.0f\t%.3f\t%.3f\n", sr, d, m);
    if (!finite) {
      fprintf(stderr, "phaser-cost: a non-finite output sample at %.0f Hz\n", sr);
      ok = 0;
    }
    if (!(m > d)) {
      fprintf(stderr, "phaser-cost: max (%.3f) is not dearer than default (%.3f) at %.0f Hz\n", m, d, sr);
      ok = 0;
    }
  }
  return ok ? 0 : 1;
}
