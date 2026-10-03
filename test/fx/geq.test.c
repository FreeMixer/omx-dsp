// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The 31-band graphic EQ kernel's oracle (omx_geq.h): identity, block, leg, denormal, thread and
 * cost arms — the engine's spec docs/design/specs/2026-09-26-graphic-eq-31.md §5, at every
 * declared rate (OMX_DECLARED_RATES). Contracts are compiled in and the last line asserts the
 * ledger came out empty.
 *
 *   F  flat: every section gain 0 (subnormal input words included) is memcmp-identical to bypass;
 *      one band engaged afterwards equals a section whose history was primed as the identity's.
 *   H  no subnormal history word after a decay with FTZ off; one block vs random splits
 *      memcmp-identical; L == R for identical legs and a NULL right leg.
 *   T  N-thread identity: THREADS threads, each on its own state, memcmp-identical to one.
 *   $  cost: the enabled kernel, all 31 sections live, per-sample p95 ns at every declared rate.
 *
 * These are the F, H, T and $ arms of the engine's mix_geq.test.c, moved with the kernel; the
 * closed-form arms are test/fx/geq_math.test.c.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <omxdsp/omx_contract_limits.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/fx/omx_geq.h>

#include "fx_rates.h"

#define THREADS 4
#define SPLIT_FRAMES 8192u
#define COST_BLOCK 256u
#define COST_WARM 64
#define COST_BLOCKS 2000

static int g_checks, g_failed;

static void ok(int cond, const char *what, double got, double want) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s: got %.6g want %.6g\n", what, got, want);
  }
}

static uint32_t g_rng = 0x9e3779b9u;
static float rnd01(void) {
  g_rng = g_rng * 1664525u + 1013904223u;
  return (float)(g_rng >> 8) / 16777216.0f;
}

static double centre_hz(int k) { return 1000.0 * pow(2.0, (double)(k - 17) / 3.0); }
static double band_q(void) { return pow(2.0, 1.0 / 6.0) / (pow(2.0, 1.0 / 3.0) - 1.0); }

/** @brief Resolve an atom from section gains: the channel EQ's matched-Z bell, kept in double. */
static void resolve(struct omx_geq *p, const float gains[OMX_GEQ_BANDS], double sr) {
  double c[OMX_GEQ_BANDS][5];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) omx_eq_design(OMX_EQ_PEAKING, centre_hz(k), band_q(), gains[k], sr, c[k]);
  omx_geq_set(p, 1, (const double(*)[5])c, gains);
}

static void fill_noise(float *x, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) x[i] = 0.5f * (rnd01() - 0.5f);
}

static void arm_f_flat(void) {
  for (unsigned ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    enum { N = 1024 };
    float in[N], l[N], r[N];
    fill_noise(in, N);
    in[3] = 1e-39f;
    in[7] = -1e-41f;
    memcpy(l, in, sizeof l);
    memcpy(r, in, sizeof r);
    float flat[OMX_GEQ_BANDS] = {0};
    struct omx_geq p;
    struct omx_geq_state st;
    resolve(&p, flat, sr);
    omx_geq_state_init(&st);
    omx_geq_process(l, r, N, &p, &st);
    ok(memcmp(l, in, sizeof l) == 0 && memcmp(r, in, sizeof r) == 0,
       "F: every section at 0 dB is memcmp-identical to bypass", 0.0, 0.0);
    /* Engage band 17 after the flat block: its input is the raw input (every section before it is
     * skipped), and its history must be the identity's — the flat block's last two input samples. */
    float gains[OMX_GEQ_BANDS] = {0};
    gains[17] = 6.0f;
    resolve(&p, gains, sr);
    float in2[64], y[64], ref[64];
    double s[4] = {in[N - 1], in[N - 2], in[N - 1], in[N - 2]};
    for (int j = 0; j < 4; j++) s[j] = omx_flush_d(s[j]);
    fill_noise(in2, 64);
    memcpy(y, in2, sizeof y);
    omx_geq_process(y, NULL, 64, &p, &st);
    for (int i = 0; i < 64; i++) ref[i] = omx_biquad_d(in2[i], p.c[17], s);
    ok(memcmp(y, ref, sizeof y) == 0, "F: an engaged band starts from the identity's history", 0.0, 0.0);
  }
  printf("F flat: %d rates, 0 dB memcmp-identical to bypass, engage primed as identity\n",
         (int)OMX_DECLARED_RATE_COUNT);
}

static int any_subnormal(const struct omx_geq_state *st) {
  const double *w = &st->l[0][0];
  for (size_t i = 0; i < sizeof *st / sizeof(double); i++)
    if (fpclassify(w[i]) == FP_SUBNORMAL) return 1;
  return 0;
}

static void arm_h_blocks_legs_denormals(void) {
  static float in[SPLIT_FRAMES], a[SPLIT_FRAMES], b[SPLIT_FRAMES], m[SPLIT_FRAMES], z[4096];
  for (unsigned ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    float gains[OMX_GEQ_BANDS];
    for (int k = 0; k < OMX_GEQ_BANDS; k++) gains[k] = (k % 3) ? 0.5f * floorf(rnd01() * 61.0f) - 15.0f : 0.0f;
    struct omx_geq p;
    resolve(&p, gains, sr);
    fill_noise(in, SPLIT_FRAMES);
    struct omx_geq_state s1, s2, s3;
    omx_geq_state_init(&s1);
    omx_geq_state_init(&s2);
    omx_geq_state_init(&s3);
    memcpy(a, in, sizeof a);
    memcpy(b, in, sizeof b);
    memcpy(m, in, sizeof m);
    omx_geq_process(a, b, SPLIT_FRAMES, &p, &s1);
    ok(memcmp(a, b, sizeof a) == 0, "H: identical legs give L == R", 0.0, 0.0);
    omx_geq_process(m, NULL, SPLIT_FRAMES, &p, &s3);
    ok(memcmp(a, m, sizeof a) == 0, "H: a NULL right leg runs the left as the stereo pair does", 0.0, 0.0);
    memcpy(b, in, sizeof b);
    for (uint32_t off = 0; off < SPLIT_FRAMES;) {
      uint32_t len = 1u + (uint32_t)(rnd01() * 4096.0f);
      if (len > SPLIT_FRAMES - off) len = SPLIT_FRAMES - off;
      omx_geq_process(b + off, NULL, len, &p, &s2);
      off += len;
    }
    ok(memcmp(a, b, sizeof a) == 0, "H: random splits 1..4096 are memcmp-identical to one block", 0.0, 0.0);
    int sub = 0;
    for (int blk = 0; blk < (int)(4.0 * sr / 4096.0) + 1; blk++) {
      memset(z, 0, sizeof z);
      omx_geq_process(z, NULL, 4096u, &p, &s2);
      sub |= any_subnormal(&s2);
    }
    ok(!sub, "H: no subnormal history word through a 4 s decay (FTZ off)", (double)sub, 0.0);
  }
  printf("H: %d rates, L==R, mono==stereo, splits identical, no subnormal state after decay\n",
         (int)OMX_DECLARED_RATE_COUNT);
}

struct worker {
  const struct omx_geq *p;
  const float *in;
  float out[2][SPLIT_FRAMES];
};

static void *worker_main(void *arg) {
  struct worker *w = arg;
  struct omx_geq_state st;
  omx_geq_state_init(&st);
  memcpy(w->out[0], w->in, sizeof w->out[0]);
  memcpy(w->out[1], w->in, sizeof w->out[1]);
  for (uint32_t off = 0; off < SPLIT_FRAMES; off += COST_BLOCK)
    omx_geq_process(w->out[0] + off, w->out[1] + off, COST_BLOCK, w->p, &st);
  return NULL;
}

static void arm_t_threads(void) {
  static float in[SPLIT_FRAMES];
  static struct worker ref, ws[THREADS];
  for (unsigned ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    float gains[OMX_GEQ_BANDS];
    for (int k = 0; k < OMX_GEQ_BANDS; k++) gains[k] = 0.5f * floorf(rnd01() * 61.0f) - 15.0f;
    struct omx_geq p;
    resolve(&p, gains, sr);
    fill_noise(in, SPLIT_FRAMES);
    ref.p = &p;
    ref.in = in;
    worker_main(&ref);
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) {
      ws[i].p = &p;
      ws[i].in = in;
      if (pthread_create(&t[i], NULL, worker_main, &ws[i]) != 0) abort();
    }
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    for (int i = 0; i < THREADS; i++)
      ok(memcmp(ws[i].out, ref.out, sizeof ref.out) == 0, "T: every thread's output equals one thread's", 0.0, 0.0);
  }
  printf("T: %d threads x %d rates, memcmp-identical to the single-thread run\n", THREADS,
         (int)OMX_DECLARED_RATE_COUNT);
}

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static void arm_cost(void) {
  char cpu[128] = "unknown";
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof line, f))
      if (strncmp(line, "model name", 10) == 0) {
        const char *c = strchr(line, ':');
        if (c) snprintf(cpu, sizeof cpu, "%s", c + 2);
        cpu[strcspn(cpu, "\n")] = 0;
        break;
      }
    fclose(f);
  }
  static double ns[COST_BLOCKS];
  static float l[COST_BLOCK], r[COST_BLOCK];
  for (unsigned ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    float gains[OMX_GEQ_BANDS];
    for (int k = 0; k < OMX_GEQ_BANDS; k++) gains[k] = (k & 1) ? -6.0f : 6.0f;
    struct omx_geq p;
    struct omx_geq_state st;
    resolve(&p, gains, sr);
    omx_geq_state_init(&st);
    for (int b = 0; b < COST_WARM + COST_BLOCKS; b++) {
      for (uint32_t i = 0; i < COST_BLOCK; i++) l[i] = r[i] = 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * (float)(b * COST_BLOCK + i) / (float)sr);
      struct timespec t0, t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      omx_geq_process(l, r, COST_BLOCK, &p, &st);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (b >= COST_WARM)
        ns[b - COST_WARM] = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / COST_BLOCK;
    }
    qsort(ns, COST_BLOCKS, sizeof ns[0], cmp_double);
    printf("COST omx_geq.h rate=%.0f sections=31 legs=2 ns_per_sample_p50=%.1f p95=%.1f cpu=\"%s\"\n", sr,
           ns[COST_BLOCKS / 2], ns[(COST_BLOCKS * 95) / 100], cpu);
    ok(isfinite(ns[COST_BLOCKS / 2]), "$: the cost is measured", ns[COST_BLOCKS / 2], 0.0);
  }
}

int main(void) {
  omx_fx_require_rate_floor();
  printf("fx/geq: %d declared rates, %d sections, Q %.4f\n", (int)OMX_DECLARED_RATE_COUNT, OMX_GEQ_BANDS, band_q());
  arm_f_flat();
  arm_h_blocks_legs_denormals();
  arm_t_threads();
  arm_cost();
  printf("fx/geq: %d checks, %d failed; %u contracts evaluated, %u violations\n", g_checks, g_failed,
         omx_contract_log.checks, omx_contract_log.count);
  if (omx_contract_log.count != 0u) g_failed++;
  return g_failed == 0 ? 0 : 1;
}
