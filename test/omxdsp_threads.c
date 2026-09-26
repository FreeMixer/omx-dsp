// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_threads — N workers, each over its own state for every stateful primitive (a biquad
 * cascade, a fractional line, an LFO, an oversampler), each calling omx_denormals_off() on entry
 * and running the same deterministic signal for 2000 blocks in uneven sizes with contracts ON;
 * every worker's output memcmp-equal to the single-threaded reference, the ledger's `checks`
 * grown by exactly N times the single-threaded count, its `count` 0
 * (docs/design/specs/2026-09-26-dsp-primitives.md §4.3 (c)). Against test/sabotage.sh's `ledger`
 * copy the count falls short and this arm is red.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKERS 8u
#define BLOCKS 2000u
#define MAXBLOCK 256u
#define RING 4096u
#define BANDS 3u
#define RATE 48000.0f

static const uint32_t BLOCK_SIZES[] = {64u, 37u, 128u, 1u, 256u, 100u, 17u, 192u};
#define NSIZES (sizeof BLOCK_SIZES / sizeof BLOCK_SIZES[0])

struct worker {
  float *out;
  float ring[RING];
  struct omx_fdelay line;
  struct omx_lfo lfo;
  struct omx_oversampler ovs;
  float bq[BANDS][4];
  int ok;
};

static uint32_t total_frames(void) {
  uint32_t t = 0u;
  for (uint32_t b = 0; b < BLOCKS; b++) t += BLOCK_SIZES[b % NSIZES];
  return t;
}

/* The one workflow every worker and the reference run: the chain over the same signal. */
static void *run(void *arg) {
  struct worker *w = arg;
  omx_denormals_off();
  static const float coeffs[BANDS][5] = {
    {0.25f, 0.5f, 0.25f, -0.1f, 0.05f},
    {0.9f, -1.2f, 0.4f, -0.3f, 0.2f},
    {0.5f, 0.1f, 0.3f, 0.2f, -0.1f},
  };
  static const uint8_t enabled[BANDS] = {1u, 1u, 1u};
  memset(w->ring, 0, sizeof w->ring);
  memset(w->bq, 0, sizeof w->bq);
  (void)omx_fdelay_init(&w->line, w->ring, RING, 3);
  w->lfo.phase = 0.0f;
  w->lfo.inc = omx_lfo_inc(0.7f, RATE);
  omx_oversampler_init(&w->ovs, 4u);
  uint32_t seed = 0x2f6e2b1du, done = 0u;
  float buf[MAXBLOCK], up[MAXBLOCK * 4u];
  for (uint32_t b = 0; b < BLOCKS; b++) {
    const uint32_t n = BLOCK_SIZES[b % NSIZES];
    for (uint32_t i = 0; i < n; i++) {
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      buf[i] = ((float)(seed >> 8) / 16777216.0f) - 0.5f;
    }
    omx_biquad_cascade(buf, n, BANDS, coeffs, enabled, w->bq);
    for (uint32_t i = 0; i < n; i++) {
      const float d = omx_lfo_sweep(20.0f, 8.0f, omx_lfo_at(&w->lfo, 0.0f));
      omx_lfo_advance(&w->lfo);
      buf[i] = omx_fdelay_tick(&w->line, buf[i], d);
    }
    omx_oversampler_up(&w->ovs, buf, n, up);
    omx_oversampler_down(&w->ovs, up, n, w->out + done);
    done += n;
  }
  w->ok = 1;
  return 0;
}

int main(void) {
  const uint32_t frames = total_frames();
  int failed = 0;
  struct worker *ref = calloc(1u, sizeof *ref);
  struct worker *ws = calloc(WORKERS, sizeof *ws);
  if (!ref || !ws) return 2;
  ref->out = calloc(frames, sizeof(float));
  for (uint32_t k = 0; k < WORKERS; k++) ws[k].out = calloc(frames, sizeof(float));
  omx_contract_reset();
  const uint32_t before_ref = omx_contract_log.checks;
  run(ref);
  const uint32_t single = omx_contract_log.checks - before_ref;
  const uint32_t before_workers = omx_contract_log.checks;
  pthread_t th[WORKERS];
  for (uint32_t k = 0; k < WORKERS; k++)
    if (pthread_create(&th[k], 0, run, &ws[k]) != 0) { printf("FAIL pthread_create %u\n", k); return 2; }
  for (uint32_t k = 0; k < WORKERS; k++) pthread_join(th[k], 0);
  const uint32_t grew = omx_contract_log.checks - before_workers;
  const uint32_t count = omx_contract_log.count;
  for (uint32_t k = 0; k < WORKERS; k++) {
    if (!ws[k].ok) { failed++; printf("FAIL worker %u did not finish\n", k); }
    if (memcmp(ws[k].out, ref->out, frames * sizeof(float)) != 0) { failed++; printf("FAIL worker %u's output differs from the reference\n", k); }
  }
  if (single < 1000u) { failed++; printf("FAIL the reference evaluated %u contracts — floor is 1000\n", single); }
  if (grew != WORKERS * single) { failed++; printf("FAIL the ledger grew by %u, %u workers x %u = %u expected\n", grew, WORKERS, single, WORKERS * single); }
  if (count != 0u) { failed++; printf("FAIL %u violations recorded\n", count); }
  printf("omxdsp_threads: %u workers x %u frames, single-threaded %u contracts, ledger grew %u, %u violations, %d failed\n",
         WORKERS, frames, single, grew, count, failed);
  return failed == 0 ? 0 : 1;
}
