// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* The precision limiter's oracle (omx_limiter.h, formerly mix_limiter.test.c) at the nine RME rates — docs/design/specs/2026-09-27-precision-limiter.md §3.
 * Contracts on; after each rate no violation the rate does not explain (fx_rates.h). */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <omxdsp/fx/omx_limiter.h>

#include "fx_rates.h"

#define MAXN 200000u
#define THREADS 4
#define RING 1200u

static int g_fail = 0, g_checks = 0;
static float g_x[2][MAXN], g_y[2][MAXN], g_z[2][MAXN];
static float g_mem[THREADS + 1][4 * RING];
static uint32_t g_idx[THREADS + 1][RING];
static uint32_t g_seed = 0x6c1d3a5bu;

static float rnd(float lo, float hi) {
  g_seed = g_seed * 1664525u + 1013904223u;
  return lo + (hi - lo) * (float)(g_seed >> 8) / 16777216.0f;
}

static void check(int cond, const char *what, double sr, double measured) {
  g_checks++;
  if (!cond) {
    g_fail++;
    printf("  FAIL @ %.0f Hz: %s (measured %.9g)\n", sr, what, measured);
  }
}

static void arm(struct omx_limiter_state *st, int slot, float sr) {
  const uint32_t cap = omx_limiter_cap(sr);
  omx_limiter_init(st, OMX_LIMITER_LOOKAHEAD_MS_DEFAULT, sr, g_mem[slot], g_idx[slot], cap);
}

static const struct omx_limiter DEF = {1, OMX_LIMITER_CEILING_DB_DEFAULT, OMX_LIMITER_RELEASE_MS_DEFAULT};

/* The ledger is read after every call on the main thread: the release pole is resolved once per
 * block, so at a rate outside the declaration each block adds a rate-is-declared precondition, and
 * a whole run would hold more than the ledger stores. */
static float g_sr;
static uint32_t g_unexplained;
static void settle_ledger(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }

static void run_blocks(float *l, float *r, uint32_t n, struct omx_limiter_state *st, int uneven, int drain) {
  const uint32_t blocks[] = {64u, 37u, 128u, 1u, 256u, 100u};
  for (uint32_t done = 0, bi = 0; done < n; bi++) {
    uint32_t k = uneven ? blocks[bi % 6u] : 128u;
    if (k > n - done) k = n - done;
    omx_limiter_process(l + done, r ? r + done : NULL, k, &DEF, st);
    if (drain) settle_ledger();
    done += k;
  }
}

struct job {
  int slot;
  float sr;
  uint32_t n;
  float l[2][MAXN / 4];
};
static struct job g_jobs[THREADS];

static void *worker(void *arg) {
  struct job *j = arg;
  struct omx_limiter_state st;
  arm(&st, j->slot, j->sr);
  run_blocks(j->l[0], j->l[1], j->n, &st, 1, 0);
  return NULL;
}

int main(void) {
  const float c = omx_db_to_lin(OMX_LIMITER_CEILING_DB_DEFAULT);
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    g_sr = sr;
    const uint32_t unexplained_before = g_unexplained;
    const uint32_t n = (uint32_t)(sr * 0.5f) < MAXN ? (uint32_t)(sr * 0.5f) : MAXN;
    check(omx_limiter_cap(sr) <= RING, "the oracle's rings hold the declared look-ahead at this rate", sr,
          omx_limiter_cap(sr));
    if (omx_limiter_cap(sr) > RING) { settle_ledger(); continue; }
    struct omx_limiter_state st;
    arm(&st, THREADS, sr);
    const uint32_t T = omx_limiter_latency(&st);
    check(T == omx_truepeak_delay() + (uint32_t)lrintf(OMX_LIMITER_LOOKAHEAD_MS_DEFAULT * sr / 1000.0f),
          "L5 the declared latency is U + round(lookahead * rate)", sr, T);

    /* L1 identity + L5 latency: below the ceiling the output is the input T frames late, bit for bit */
    for (uint32_t i = 0; i < n; i++) {
      g_x[0][i] = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / sr) + rnd(-0.05f, 0.05f);
      g_x[1][i] = 0.4f * sinf(2.0f * (float)M_PI * 660.0f * (float)i / sr) + rnd(-0.05f, 0.05f);
    }
    g_x[0][100] = 0.8f;
    memcpy(g_y, g_x, sizeof g_y);
    run_blocks(g_y[0], g_y[1], n, &st, 1, 1);
    int exact = 1;
    for (uint32_t i = 0; i < n; i++) {
      const float wl = i >= T ? g_x[0][i - T] : 0.0f, wr = i >= T ? g_x[1][i - T] : 0.0f;
      exact &= memcmp(&g_y[0][i], &wl, sizeof wl) == 0 && memcmp(&g_y[1][i], &wr, sizeof wr) == 0;
    }
    check(exact, "L1 below the ceiling the output is the input delayed by T, bit for bit", sr, 0.0);
    check(g_y[0][100 + T] == 0.8f, "L5 the marked sample lands at T", sr, g_y[0][100 + T]);

    /* L2 ceiling: +12 dBFS sine, +18 dBFS noise, the rate/4 inter-sample crest at +6 dBFS */
    for (int sig = 0; sig < 3; sig++) {
      for (uint32_t i = 0; i < n; i++) {
        g_x[0][i] = sig == 0 ? 4.0f * sinf(2.0f * (float)M_PI * 1000.0f * (float)i / sr)
                  : sig == 1 ? rnd(-8.0f, 8.0f)
                             : 2.0f * sinf((float)(M_PI / 2.0) * (float)(i % 4u) + (float)(M_PI / 4.0));
        g_x[1][i] = sig == 1 ? rnd(-8.0f, 8.0f) : g_x[0][i];
      }
      arm(&st, THREADS, sr);
      memcpy(g_y, g_x, sizeof g_y);
      run_blocks(g_y[0], g_y[1], n, &st, 1, 1);
      float sp = 0.0f;
      for (uint32_t i = 0; i < n; i++) sp = fmaxf(sp, fmaxf(fabsf(g_y[0][i]), fabsf(g_y[1][i])));
      check(sp <= c, "L2 the output's sample peak never passes the ceiling", sr, sp);
      struct omx_truepeak tp;
      omx_truepeak_init(&tp);
      const float tpk = omx_truepeak_block(&tp, g_y[0], n, g_z[0]);
      printf("  %.0f Hz L2 signal %d: sample peak %.3f dBFS, true peak %.3f dBTP (ceiling %.1f)\n", sr, sig,
             20.0 * log10(sp), 20.0 * log10(tpk), OMX_LIMITER_CEILING_DB_DEFAULT);
      if (sig != 1) check(tpk <= c * 1.0023f, "L2 a steady over's true peak stays within 0.02 dB of the ceiling", sr, tpk / c);
      if (sig == 2) {
        float lo = 1e9f;
        for (uint32_t i = n / 2; i < n; i++) lo = fminf(lo, g_z[0][i]);
        check(fabsf(20.0f * log10f(tpk / c)) < 0.05f, "L3 the inter-sample crest settles at the ceiling, true peak", sr, 20.0 * log10(tpk / c));
      }
      if (sig == 0) {
        float late = 0.0f;
        for (uint32_t i = n / 2; i < n; i++) late = fmaxf(late, fabsf(g_y[0][i]));
        check(fabsf(20.0f * log10f(late / c)) < 0.05f, "L3 a steady +12 dBFS sine settles at the ceiling within 0.05 dB", sr, 20.0 * log10(late / c));
      }
    }

    /* L2 onset: isolated +12 dBFS spikes over a quiet tone at the travel's fastest release — the
     * hold keeps the gain down for the whole look-ahead window, so no spike reaches the clamp */
    for (uint32_t i = 0; i < n; i++) {
      g_x[0][i] = g_x[1][i] = 0.1f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / sr);
      if (i % (n / 8u) == n / 16u) g_x[0][i] = g_x[1][i] = 4.0f;
    }
    {
      const struct omx_limiter fast = {1, OMX_LIMITER_CEILING_DB_DEFAULT, OMX_LIMITER_RELEASE_MS_MIN};
      arm(&st, THREADS, sr);
      memcpy(g_y, g_x, sizeof g_y);
      for (uint32_t done = 0; done < n; done += 128u) {
        omx_limiter_process(g_y[0] + done, g_y[1] + done, n - done < 128u ? n - done : 128u, &fast, &st);
        settle_ledger();
      }
      uint32_t clamped = 0u;
      for (uint32_t i = 0; i < n; i++) clamped += fabsf(g_y[0][i]) >= c;
      check(clamped == 0u, "L2 no isolated spike reaches the clamp: the hold, not the clamp, limits", sr, clamped);
    }

    /* L4 release: after the over stops the gain recovers monotonically and reaches unity */
    const uint32_t burst = n / 5u;
    for (uint32_t i = 0; i < n; i++) {
      const float a = i < burst ? 2.0f : 0.1f;
      g_x[0][i] = g_x[1][i] = a * sinf(2.0f * (float)M_PI * 100.0f * (float)i / sr);
    }
    arm(&st, THREADS, sr);
    memcpy(g_y, g_x, sizeof g_y);
    run_blocks(g_y[0], g_y[1], n, &st, 1, 1);
    float prev = 0.0f;
    int mono = 1;
    for (uint32_t i = burst + T + st.d + 4u; i < n; i++) {
      const float x = g_x[0][i - T];
      if (fabsf(x) < 0.02f) continue;
      const float g = g_y[0][i] / x;
      mono &= g >= prev - 1e-6f;
      prev = g;
    }
    check(mono, "L4 the gain only rises after the over stops", sr, prev);
    const uint32_t settled = burst + T + st.d + (uint32_t)(10.0f * OMX_LIMITER_RELEASE_MS_DEFAULT * sr / 1000.0f);
    int unity = 1;
    for (uint32_t i = settled; i < n; i++) unity &= g_y[0][i] == g_x[0][i - T];
    check(unity, "L4 ten release constants later the gain is exactly 1 again", sr, prev);

    /* block size invariance and bypass identity */
    for (uint32_t i = 0; i < n; i++) g_x[0][i] = g_x[1][i] = rnd(-4.0f, 4.0f);
    memcpy(g_y, g_x, sizeof g_y);
    memcpy(g_z, g_x, sizeof g_z);
    arm(&st, THREADS, sr);
    run_blocks(g_y[0], g_y[1], n, &st, 1, 1);
    arm(&st, THREADS, sr);
    run_blocks(g_z[0], g_z[1], n, &st, 0, 1);
    check(memcmp(g_y, g_z, sizeof g_y) == 0, "the block size does not touch a bit of the output", sr, 0.0);
    struct omx_limiter_state before = st;
    const struct omx_limiter off = {0, OMX_LIMITER_CEILING_DB_DEFAULT, OMX_LIMITER_RELEASE_MS_DEFAULT};
    memcpy(g_z, g_x, sizeof g_z);
    omx_limiter_process(g_z[0], g_z[1], n, &off, &st);
    check(memcmp(g_z, g_x, sizeof g_z) == 0 && memcmp(&before, &st, sizeof st) == 0,
          "disabled: no sample and no state word moves", sr, 0.0);

    /* N-thread identity: THREADS states on THREADS threads reproduce the one-thread bytes */
    const uint32_t tn = n / 4u < MAXN / 4u ? n / 4u : MAXN / 4u;
    for (int t = 0; t < THREADS; t++) {
      g_jobs[t].slot = t;
      g_jobs[t].sr = sr;
      g_jobs[t].n = tn;
      memcpy(g_jobs[t].l[0], g_x[0], tn * sizeof(float));
      memcpy(g_jobs[t].l[1], g_x[1], tn * sizeof(float));
    }
    pthread_t th[THREADS];
    for (int t = 0; t < THREADS; t++) pthread_create(&th[t], NULL, worker, &g_jobs[t]);
    for (int t = 0; t < THREADS; t++) pthread_join(th[t], NULL);
    /* Each thread replays the first tn frames of the one-thread run above, whose ledger was read
     * block by block, and the bytes below show it took the same path. At a declared rate its
     * ledger must still be empty; outside the declaration it holds one rate-is-declared per block,
     * more than the ledger stores, and is let go. */
    if (omx_rate_is_declared(sr)) settle_ledger();
    else omx_contract_reset();
    int same = 1;
    for (int t = 0; t < THREADS; t++)
      same &= memcmp(g_jobs[t].l[0], g_y[0], tn * sizeof(float)) == 0 && memcmp(g_jobs[t].l[1], g_y[1], tn * sizeof(float)) == 0;
    check(same, "N threads on distinct state produce the one-thread bytes", sr, 0.0);

    /* cost: ns per stereo frame at the default travels, limiting noise at +12 dBFS, at the top rate */
    if (ri + 1u == OMX_FX_RME_RATE_COUNT) {
      struct timespec a, b;
      arm(&st, THREADS, sr);
      memcpy(g_y, g_x, sizeof g_y);
      clock_gettime(CLOCK_MONOTONIC, &a);
      run_blocks(g_y[0], g_y[1], n, &st, 0, 1);
      clock_gettime(CLOCK_MONOTONIC, &b);
      const double ns = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / n;
      printf("  COST limiter stereo @%.0f Hz, 128-frame blocks, contracts %s: %.1f ns/frame\n", sr,
#ifdef OMX_CONTRACTS
             "on",
#else
             "off",
#endif
             ns);
    }
    settle_ledger();
    check(g_unexplained == unexplained_before, "no contract violation the rate does not explain", sr,
          g_unexplained - unexplained_before);
  }
  printf("mix_limiter: %d checks, %d failed, %u rates; %u contracts evaluated, %u violations the rate does not explain\n",
         g_checks, g_fail, OMX_FX_RME_RATE_COUNT, omx_contract_log.checks, g_unexplained);
  return g_fail == 0 && g_unexplained == 0u ? 0 : 1;
}
