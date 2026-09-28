// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * prel-cost.c — the cost row of omx_env_program_release (compressor-models §4 L8): ns per sample
 * at every declared rate, contracts off, over a programme that alternates reduction and release
 * so both branches and the slow pole's expf run. Prints one TSV row per rate: rate, p50, p95.
 */
#include <omxdsp/omxdsp.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N 4096u
#define REPS 400u

static int cmp(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(void) {
  static float target[N];
  static double ns[REPS];
  uint32_t seed = 0x1234567u;
  for (uint32_t i = 0; i < N; i++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    target[i] = ((i / 512u) % 2u) ? 0.0f : -12.0f * (float)(seed >> 8) / 16777216.0f;
  }
  printf("rate\tns_per_sample_p50\tns_per_sample_p95\n");
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    struct omx_env_program_release_params p;
    omx_env_program_release_opto(&p, sr, 1u);
    struct omx_env_program_release st = {0.0f, 0.0f, 0.0f};
    volatile float sink = 0.0f;
    for (uint32_t r = 0; r < REPS; r++) {
      struct timespec a, b;
      clock_gettime(CLOCK_MONOTONIC, &a);
      float acc = 0.0f;
      for (uint32_t i = 0; i < N; i++) acc += omx_env_program_release(&st, &p, target[i]);
      clock_gettime(CLOCK_MONOTONIC, &b);
      sink += acc;
      ns[r] = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / N;
    }
    qsort(ns, REPS, sizeof ns[0], cmp);
    printf("%.0f\t%.2f\t%.2f\n", sr, ns[REPS / 2u], ns[(REPS * 95u) / 100u]);
    (void)sink;
  }
  return 0;
}
