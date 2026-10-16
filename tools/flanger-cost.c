// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * flanger-cost.c — the flanger kernel's own cost: one omx_flanger_process per block, both legs, a
 * sustained -6 dBFS 1 kHz tone on every input, phase-continuous across blocks (never silence); the
 * first 64 blocks discarded, every other block timed on its own, the stimulus fill outside the
 * timed region. Median and p95 of ns per sample at every declared rate.
 *
 * Two settings:
 *   default  0.25 Hz, depth 2 ms, feedback +0.6, mix 0.5;
 *   max      10 Hz, the deepest sweep, the feedback clamp, mix 1.
 *
 * Output: the gate bench's layout, so tools/kernel-cost-ab.sh compares two builds of it: a header
 * line, then setting, rate, quantum, p50, p95, checksum (the sum of the rendered output; two builds
 * compare only when it is the same). RELATIVE figures only.
 *
 * Usage: flanger-cost [quantum=512] [blocks=2000]
 */
#include <omxdsp/fx/omx_flanger.h>
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WARMUP 64u
#define MAXQ 8192u

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float g_ring_l[OMX_FLANGER_CAP], g_ring_r[OMX_FLANGER_CAP];

static int cmp_ll(const void *a, const void *b) {
  const long long x = *(const long long *)a, y = *(const long long *)b;
  return (x > y) - (x < y);
}

static long long now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void cell(const char *setting, int max, float sr, uint32_t n, uint32_t blocks) {
  static float l[MAXQ], r[MAXQ];
  struct omx_flanger p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.base_samples = (float)OMX_FLANGER_BASE_MS * 0.001f * sr;
  p.depth_samples = (max ? OMX_FLANGER_MAX_DEPTH_MS : 2.0f) * 0.001f * sr;
  p.lfo_inc = omx_lfo_inc(max ? 10.0f : 0.25f, sr);
  p.feedback = max ? OMX_FLANGER_FB_MAX : 0.6f;
  p.mix = max ? 1.0f : 0.5f;
  struct omx_flanger_state s;
  memset(g_ring_l, 0, sizeof g_ring_l);
  memset(g_ring_r, 0, sizeof g_ring_r);
  if (omx_flanger_state_init(&s, g_ring_l, g_ring_r, OMX_FLANGER_CAP) != OMX_FDELAY_OK) exit(2);
  long long *t = calloc(blocks, sizeof *t);
  if (t == NULL) exit(2);
  const double w = 2.0 * M_PI * 1000.0 / (double)sr;
  double ph = 0.0, sum = 0.0;
  for (uint32_t b = 0; b < WARMUP + blocks; b++) {
    for (uint32_t i = 0; i < n; i++) {
      l[i] = r[i] = (float)(0.5011872336272722 * sin(ph));
      ph += w;
      if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
    }
    const long long t0 = now_ns();
    omx_flanger_process(l, r, n, &p, &s);
    const long long t1 = now_ns();
    for (uint32_t i = 0; i < n; i++) sum += (double)l[i] + (double)r[i];
    if (b >= WARMUP) t[b - WARMUP] = t1 - t0;
  }
  qsort(t, blocks, sizeof *t, cmp_ll);
  printf("%s\t%.0f\t%u\t%.3f\t%.3f\t%.9e\n", setting, (double)sr, n, (double)t[blocks / 2] / n,
         (double)t[(blocks * 95u) / 100u] / n, sum);
  free(t);
}

int main(int argc, char **argv) {
  const uint32_t n = argc > 1 ? (uint32_t)atoi(argv[1]) : 512u;
  const uint32_t blocks = argc > 2 ? (uint32_t)atoi(argv[2]) : 2000u;
  if (n == 0u || n > MAXQ || blocks == 0u) { fprintf(stderr, "usage: flanger-cost [quantum] [blocks]\n"); return 2; }
  printf("setting\trate\tquantum\tp50_ns_per_sample\tp95_ns_per_sample\tchecksum\n");
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++) {
    cell("default", 0, OMX_DECLARED_RATES[i], n, blocks);
    cell("max", 1, OMX_DECLARED_RATES[i], n, blocks);
  }
  return 0;
}
