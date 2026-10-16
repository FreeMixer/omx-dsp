// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * gate-cost-bench.c — the gate kernel's own cost (omx_dynamics in BELOW mode, knee 0, peak detector,
 * oversampling auto, contracts off): a sustained 1 kHz sine at -6 dBFS, phase-continuous across
 * blocks; every block timed on its own; the first 64 blocks discarded; median and p95 of ns per
 * sample per lane, at every declared rate. A block is LANES mono gate lanes, each on its own state,
 * so the timer's own cost is spread over LANES x quantum samples.
 *
 * Three settings, each holding the tone in one region of the gain computer:
 *   default  threshold -37 dB, ratio 16, range -90, attack 1 ms, release 100 ms: the tone sits
 *            above the threshold, the gate OPEN on every sample;
 *   slope    threshold 0 dB, ratio 2: the tone's envelope (about -6 dB) sits on the expansion
 *            slope, the one region whose gain moves and takes the dB path every sample;
 *   closed   threshold 0 dB, ratio 16: the tone held on the range floor.
 *
 * Built against whichever include/ comes first on the path, so tools/gate-cost-ab.sh builds a base
 * revision's kernel and this tree's from this one source and interleaves them. The checksum column
 * is the output: two builds compare only when it is the same. RELATIVE figures only.
 *
 * Usage: gate-cost-bench [quantum=128] [blocks=2000]
 */
#include <omxdsp/omxdsp.h>
#include <omxdsp/omx_dyn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LANES 16u
#define WARMUP 64u

static int cmp_ll(const void *a, const void *b) {
  const long long x = *(const long long *)a, y = *(const long long *)b;
  return (x > y) - (x < y);
}

static long long now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void cell(const char *setting, float thr, float ratio, float sr, uint32_t n, uint32_t blocks) {
  static struct omx_dyn p;
  static struct omx_dyn_state st[LANES];
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.gc.mode = OMX_DYN_BELOW;
  p.gc.thresh_db = thr;
  p.gc.ratio = ratio;
  p.gc.knee_db = 0.0f;
  p.gc.range_db = -90.0f;
  p.gc.makeup_lin = 1.0f;
  p.detect = OMX_DETECT_PEAK;
  p.attack_ms = 1.0f;
  p.attack_coeff = omx_pole_from_time_ms(1.0f, sr);
  p.release_coeff = omx_pole_from_time_ms(100.0f, sr);
  p.ovs_mode = OMX_DYN_OVS_AUTO;
  for (uint32_t k = 0; k < LANES; k++) omx_dyn_state_init(&st[k], omx_dyn_oversample_factor(&p));
  float *buf = calloc((size_t)LANES * n, sizeof(float));
  long long *t = calloc(blocks, sizeof(long long));
  const double w = 2.0 * M_PI * 1000.0 / sr;
  const float amp = 0.5011872f; /* -6 dBFS */
  uint64_t ph = 0;
  double sum = 0.0;
  for (uint32_t b = 0; b < WARMUP + blocks; b++) {
    for (uint32_t k = 0; k < LANES; k++)
      for (uint32_t i = 0; i < n; i++) buf[k * n + i] = amp * (float)sin(w * (double)(ph + i));
    ph += n;
    const long long t0 = now_ns();
    for (uint32_t k = 0; k < LANES; k++) omx_dynamics(buf + k * n, NULL, n, &p, &st[k]);
    const long long t1 = now_ns();
    for (uint32_t i = 0; i < n; i++) sum += buf[i];
    if (b >= WARMUP) t[b - WARMUP] = t1 - t0;
  }
  qsort(t, blocks, sizeof *t, cmp_ll);
  const double per = (double)LANES * n;
  printf("%s\t%.0f\t%u\t%.3f\t%.3f\t%.9e\n", setting, (double)sr, n, (double)t[blocks / 2] / per,
         (double)t[(blocks * 95u) / 100u] / per, sum);
  free(buf);
  free(t);
}

int main(int argc, char **argv) {
  const uint32_t n = argc > 1 ? (uint32_t)atoi(argv[1]) : 128u;
  const uint32_t blocks = argc > 2 ? (uint32_t)atoi(argv[2]) : 2000u;
  if (n == 0u || n > 8192u || blocks == 0u) { fprintf(stderr, "usage: gate-cost-bench [quantum] [blocks]\n"); return 2; }
  printf("setting\trate\tquantum\tp50_ns_per_sample_lane\tp95_ns_per_sample_lane\tchecksum\n");
  for (uint32_t r = 0; r < OMX_DECLARED_RATE_COUNT; r++) {
    const float sr = OMX_DECLARED_RATES[r];
    cell("default", -37.0f, 16.0f, sr, n, blocks);
    cell("slope", 0.0f, 2.0f, sr, n, blocks);
    cell("closed", 0.0f, 16.0f, sr, n, blocks);
  }
  return 0;
}
