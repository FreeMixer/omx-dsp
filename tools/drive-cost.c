// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * drive-cost.c — the drive stage's own cost: omx_drive_process at the console's come-up atom (soft
 * curve, full band, 0 dB, character 0, mix 100 %, auto-gain on and linked, no HF roll-off, 4x),
 * stereo, and the two halves that explain it. A non-silent input (a -12 dBFS two-tone plus a small
 * noise floor, so no denormal path and no "silence is cheap" branch), phase-continuous across
 * blocks; every block timed on its own; the first 64 blocks discarded; median and p95 of ns per
 * sample at every declared rate.
 *
 * Three settings:
 *   drive  omx_drive_process, both legs;
 *   ovs    the shared oversampler alone, 4x up then down, one leg;
 *   shape  the soft shaper alone over 4n samples, one leg.
 * `drive` is what the stage costs; `ovs` and `shape` attribute it.
 *
 * Output: the gate bench's layout, so tools/kernel-cost-ab.sh compares two builds of it: a header
 * line, then setting, rate, quantum, p50, p95, checksum. The checksum is an FNV-1a of the rendered
 * output: a faster kernel that renders other bits is not the same kernel. RELATIVE figures only.
 *
 * Usage: drive-cost [quantum=512] [blocks=2000]
 */
#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/omx_oversampler.h>
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

static int cmp_ll(const void *a, const void *b) {
  const long long x = *(const long long *)a, y = *(const long long *)b;
  return (x > y) - (x < y);
}

static long long now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static uint64_t fnv(uint64_t h, const float *x, uint32_t n) {
  const unsigned char *p = (const unsigned char *)x;
  for (size_t i = 0; i < (size_t)n * sizeof(float); i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

static void identity5(float *c) { c[0] = 1.0f; c[1] = c[2] = c[3] = c[4] = 0.0f; }

static void fill(float *l, float *r, uint32_t n, uint64_t *phase, float sr, uint32_t *seed) {
  for (uint32_t i = 0; i < n; i++) {
    const double t = (double)(*phase + i) / (double)sr;
    *seed = *seed * 1664525u + 1013904223u;
    const float noise = ((float)(*seed >> 8) / 16777216.0f - 0.5f) * 1e-3f;
    l[i] = 0.25f * (float)sin(2.0 * M_PI * 997.0 * t) + 0.125f * (float)sin(2.0 * M_PI * 3001.0 * t) + noise;
    r[i] = 0.25f * (float)sin(2.0 * M_PI * 1499.0 * t) + 0.125f * (float)sin(2.0 * M_PI * 5003.0 * t) - noise;
  }
  *phase += n;
}

static void cell(const char *setting, float sr, uint32_t n, uint32_t blocks) {
  static float l[MAXQ], r[MAXQ], up[MAXQ * OMX_OVS_MAX_FACTOR];
  struct omx_drive p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.curve = OMX_DRIVE_SOFT;
  p.band = OMX_DRIVE_BAND_FULL;
  p.drive_lin = 1.0f;
  p.even_w = 0.5f;
  p.mix = 1.0f;
  p.trim_lin = 1.0f;
  p.auto_gain = 1;
  p.stereo_link = 1;
  p.hf_on = 0;
  p.os_factor = omx_drive_factor_of(4);
  identity5(p.band_c);
  identity5(p.tilt_c);
  identity5(p.tilt_inv_c);
  identity5(p.hf_c);
  omx_drive_time_constants(&p, sr);
  struct omx_drive_state *st = calloc(1, sizeof *st);
  struct omx_oversampler os;
  long long *t = calloc(blocks, sizeof *t);
  if (st == NULL || t == NULL) exit(2);
  omx_drive_state_init(st, p.os_factor);
  omx_oversampler_init(&os, 4u);
  uint64_t phase = 0, h = 1469598103934665603ull;
  uint32_t seed = 12345u;
  for (uint32_t b = 0; b < WARMUP + blocks; b++) {
    fill(l, r, n, &phase, sr, &seed);
    long long t0 = now_ns();
    if (!strcmp(setting, "ovs")) {
      omx_oversampler_up(&os, l, n, up);
      omx_oversampler_down(&os, up, n, l);
    } else if (!strcmp(setting, "shape")) {
      for (uint32_t i = 0; i < n * 4u; i++) up[i] = 0.25f * l[i % n] * (float)(i & 3u);
      t0 = now_ns();
      for (uint32_t i = 0; i < n * 4u; i++) up[i] = omx_drive_shape(OMX_DRIVE_SOFT, up[i]);
      memcpy(l, up, n * sizeof l[0]);
    } else {
      omx_drive_process(l, r, n, &p, st);
    }
    const long long t1 = now_ns();
    if (b >= WARMUP) {
      t[b - WARMUP] = t1 - t0;
      h = fnv(fnv(h, l, n), r, n);
    }
  }
  qsort(t, blocks, sizeof *t, cmp_ll);
  printf("%s\t%.0f\t%u\t%.3f\t%.3f\t%016llx\n", setting, (double)sr, n, (double)t[blocks / 2] / n,
         (double)t[(blocks * 95u) / 100u] / n, (unsigned long long)h);
  free(st);
  free(t);
}

int main(int argc, char **argv) {
  const uint32_t n = argc > 1 ? (uint32_t)atoi(argv[1]) : 512u;
  const uint32_t blocks = argc > 2 ? (uint32_t)atoi(argv[2]) : 2000u;
  if (n == 0u || n > MAXQ || blocks == 0u) { fprintf(stderr, "usage: drive-cost [quantum] [blocks]\n"); return 2; }
  printf("setting\trate\tquantum\tp50_ns_per_sample\tp95_ns_per_sample\tchecksum\n");
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++) {
    cell("drive", OMX_DECLARED_RATES[i], n, blocks);
    cell("ovs", OMX_DECLARED_RATES[i], n, blocks);
    cell("shape", OMX_DECLARED_RATES[i], n, blocks);
  }
  return 0;
}
