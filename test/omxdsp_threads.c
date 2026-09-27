// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_threads.c — N threads, each over its own state, contracts ON, every output byte-identical
 * to the single-threaded reference, the ledger empty (docs/design/specs/2026-09-26-dsp-primitives.md
 * §4.3 (c)). Built with -pthread; `make test-tsan` runs it under ThreadSanitizer (§4.3 (e)), and
 * again with OMXDSP_THREADS_SABOTAGE_SHARED_STATE — every worker handed ONE fractional line — which
 * TSan must report as a race.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 8
#define BLOCKS 2000
#define MAXN 256
#define OUT_SAMPLES (BLOCKS * 64)

struct worker {
  float ring[1024];
  float *out; /* OUT_SAMPLES floats, the worker's own */
  uint32_t rate_index;
};

static const uint32_t BLOCK_SIZES[] = {64u, 37u, 128u, 1u, 256u, 100u, 64u, 9u};

#ifdef OMXDSP_THREADS_SABOTAGE_SHARED_STATE
static struct omx_fdelay g_shared_line;
static float g_shared_ring[1024];
#endif

/* The one signal every worker processes, and the one chain of primitives it runs it through. */
static void run_chain(struct worker *w) {
  const float sr = OMX_DECLARED_RATES[w->rate_index % OMX_DECLARED_RATE_COUNT];
  omx_denormals_off();
#ifdef OMXDSP_THREADS_SABOTAGE_SHARED_STATE
#define line g_shared_line
  omx_fdelay_init(&line, g_shared_ring, 1024u, 3);
#else
  struct omx_fdelay line;
  omx_fdelay_init(&line, w->ring, 1024u, 3);
  memset(w->ring, 0, sizeof w->ring);
#endif
  struct omx_lfo lfo = {0.0f, omx_lfo_inc(1.5f, sr), 0.0f};
  float coeffs[2][5];
  omx_eq_design_f(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, coeffs[0]);
  omx_eq_design_f(OMX_EQ_HIGHPASS, 80.0, M_SQRT1_2, 0.0, sr, coeffs[1]);
  float eq_state[2][4] = {{0}};
  float eq2_l[2][4] = {{0}}, eq2_r[2][4] = {{0}}; /* the two-leg cascade's own state */
  struct omx_oversampler ovs;
  omx_oversampler_init(&ovs, 4u);
  float env = 0.0f;
  struct omx_divider div;
  omx_divider_init(&div);
  const float pole = omx_pole_from_time_ms(10.0f, sr);
  const struct omx_env_params ep = {omx_pole_from_time_ms(2.0f, sr), omx_pole_from_time_ms(80.0f, sr), OMX_DETECT_RMS};
  const struct omx_gaincomp_params gc = {OMX_DYN_ABOVE, -24.0f, 4.0f, 6.0f, 0.0f, 1.5f};
  struct omx_env det;
  memset(&det, 0, sizeof det);
  float ac, rc;
  omx_env_stage_poles(&ep, 1u, &ac, &rc);
  const struct omx_env_params fe = {omx_pole_from_time_ms(OMX_TRANSIENT_FAST_ATTACK_MS, sr),
                                    omx_pole_from_time_ms(OMX_TRANSIENT_FAST_RELEASE_MS, sr), OMX_DETECT_PEAK};
  const struct omx_env_params se = {omx_pole_from_time_ms(OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, sr), fe.release_pole, OMX_DETECT_PEAK};
  struct omx_envdiff_poles dp;
  omx_env_stage_poles(&fe, 1u, &dp.fast_attack, &dp.fast_release);
  omx_env_stage_poles(&se, 1u, &dp.slow_attack, &dp.slow_release);
  struct omx_env dfast, dslow;
  memset(&dfast, 0, sizeof dfast);
  memset(&dslow, 0, sizeof dslow);
  uint32_t seed = 0x9e3779b9u ^ w->rate_index;
  uint32_t written = 0u;
  float block[MAXN], side[MAXN], up[MAXN * 4], down[MAXN];
  for (uint32_t b = 0; b < BLOCKS && written < OUT_SAMPLES; b++) {
    uint32_t n = BLOCK_SIZES[b % 8u];
    if (n > OUT_SAMPLES - written) n = OUT_SAMPLES - written;
    for (uint32_t i = 0; i < n; i++) {
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      block[i] = 0.5f * ((float)(seed >> 8) / 16777216.0f * 2.0f - 1.0f);
    }
    omx_biquad_cascade(block, n, 2u, coeffs, NULL, eq_state);
    for (uint32_t i = 0; i < n; i++) side[i] = 0.5f * block[(i * 7u) % n];
    omx_biquad_cascade_stereo(block, side, n, 2u, coeffs, NULL, eq2_l, eq2_r);
    for (uint32_t i = 0; i < n; i++) block[i] = 0.5f * (block[i] + side[i]);
    omx_oversampler_up(&ovs, block, n, up);
    omx_oversampler_down(&ovs, up, n, down);
    for (uint32_t i = 0; i < n; i++) {
      const float d = omx_lfo_sweep(8.0f, 4.0f, omx_lfo_at(&lfo, 0.0f));
      omx_lfo_advance(&lfo);
      const float y = omx_fdelay_tick(&line, down[i], d);
      omx_onepole(&env, fabsf(y), pole);
      const float fl = omx_env_step(&dfast, &fe, fabsf(y), dp.fast_attack, dp.fast_release);
      const float onset = omx_envdiff_step(&dslow, &fe, fabsf(y), fl, &dp, OMX_TRANSIENT_FLOOR_LIN);
      const float g = omx_gaincomp_gain(&gc, omx_env_step(&det, &ep, y * y, ac, rc)) * omx_db_to_lin_poly(0.5f * onset);
      const float sub = omx_divider_step(&div, y, 0.05f);
      w->out[written + i] = omx_flush(g * (y + 0.5f * sub) * omx_db_to_lin(omx_lin_to_db(1.0f + env) - omx_lin_to_db(1.0f + env)));
    }
    written += n;
  }
#undef line
}

static void *worker_main(void *arg) {
  run_chain((struct worker *)arg);
  return NULL;
}

int main(void) {
  static struct worker reference, workers[THREADS];
  static float ref_out[OUT_SAMPLES], outs[THREADS][OUT_SAMPLES];
  omx_contract_reset();
  int failed = 0;

  /* the single-threaded reference per rate, and the contracts it evaluates */
  reference.out = ref_out;
  const uint32_t checks_before = omx_contract_log.checks;
  reference.rate_index = 0u;
  run_chain(&reference);
  const uint32_t checks_one = omx_contract_log.checks - checks_before;
  if (omx_contract_log.count != 0u) { printf("FAIL the reference recorded %u violations\n", omx_contract_log.count); failed++; }
  if (checks_one < 100000u) { printf("FAIL the reference evaluated only %u contracts\n", checks_one); failed++; }

  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    omx_contract_reset();
    const uint32_t before = omx_contract_log.checks;
    reference.rate_index = ri;
    run_chain(&reference);
    pthread_t tid[THREADS];
    for (int t = 0; t < THREADS; t++) {
      workers[t].out = outs[t];
      workers[t].rate_index = ri;
      if (pthread_create(&tid[t], NULL, worker_main, &workers[t]) != 0) { perror("pthread_create"); return 2; }
    }
    for (int t = 0; t < THREADS; t++) pthread_join(tid[t], NULL);
    for (int t = 0; t < THREADS; t++) {
      if (memcmp(outs[t], ref_out, sizeof ref_out) != 0) {
        printf("FAIL rate %.0f: worker %d's output differs from the single-threaded reference\n",
               OMX_DECLARED_RATES[ri], t);
        failed++;
      }
    }
    const uint32_t grew = omx_contract_log.checks - before;
    if (grew < (THREADS + 1u) * checks_one / 2u) {
      printf("FAIL rate %.0f: the ledger counted %u contracts for %d workers; the reference alone evaluated %u\n",
             OMX_DECLARED_RATES[ri], grew, THREADS + 1, checks_one);
      failed++;
    }
    if (omx_contract_log.count != 0u) {
      printf("FAIL rate %.0f: %u violations recorded with contracts on across %d threads\n",
             OMX_DECLARED_RATES[ri], omx_contract_log.count, THREADS);
      failed++;
    }
  }
  printf("omxdsp_threads: %d threads x %u rates, %u output samples each, byte-identical to the reference; "
         "%u contracts evaluated per chain, ledger empty; %d failed\n",
         THREADS, OMX_DECLARED_RATE_COUNT, (unsigned)OUT_SAMPLES, checks_one, failed);
  return failed == 0 ? 0 : 1;
}
