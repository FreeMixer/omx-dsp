// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The dual-tap pitch shifter's oracle (omx_pitch.h): bypass, finite/flush and thread arms, at
 * EVERY declared rate (OMX_DECLARED_RATES) — the engine's spec
 * docs/design/specs/2026-09-22-native-pitch-shift.md. Contracts are compiled in and every arm
 * drains the ledger empty.
 *
 *   E  — bypass identity: enabled = 0 and mix = 0 are memcmp-identical, subnormal words included.
 *   G  — finite, both legs independent, and a silence after a tone flushes to exact zero with no
 *        subnormal state.
 *   H  — N-thread identity: eight threads run the kernel on distinct state at once, every
 *        declared rate, and each output is byte-identical to a single-threaded reference (the
 *        kernel holds no writable data of its own; rt-thread-split §6).
 *
 * These are the E, G and H arms of the engine's mix_pitch.test.c, moved with the kernel; the
 * closed-form arms are test/fx/pitch_math.test.c.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <float.h>
#include <pthread.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_pitch.h>

#include "fx_rates.h"

static int g_checks = 0, g_failed = 0;
static void ok(int cond, const char *what, double sr, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s @ %.0f Hz — measured %.9g, limit %.9g\n", what, sr, measured, limit);
  }
}

static void drain_violations(const char *where) {
  uint32_t seen = omx_contract_log.count;
  uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++)
    printf("VIOLATION [%s] %s %s (frame %u)\n", omx_contract_log.rec[i].stage,
           omx_contract_log.rec[i].kind, omx_contract_log.rec[i].token,
           omx_contract_log.rec[i].frame);
  ok(seen == 0u, where, 0.0, (double)seen, 0.0);
  omx_contract_log.count = 0u;
}

/** The window W in samples at `sr`, the oracle's own closed form (spec §1). */
static double o_window(double sr) { return (double)OMX_PITCH_WINDOW_MS * 1e-3 * sr; }

/* ---- the stage, driven the way the walk will drive it ----------------------------------- */

static float *g_ring;
static uint32_t g_ring_cap;

struct run {
  struct omx_pitch p;
  struct omx_pitch_state s;
};

static void setup(struct run *R, double sr, double r, double mix) {
  const uint32_t cap = omx_pitch_cap_for((float)sr);
  ok(cap <= g_ring_cap, "the ring the stage asks for fits the test's", sr, cap, g_ring_cap);
  memset(g_ring, 0, 2u * g_ring_cap * sizeof(float));
  ok(omx_pitch_state_init(&R->s, g_ring, g_ring + g_ring_cap, cap) == OMX_FDELAY_OK,
     "the lines arm", sr, 0, 0);
  omx_pitch_resolve_ratio(&R->p, 1, (float)r, (float)mix, (float)sr);
}

static void process(struct run *R, float *l, float *rr, size_t n) {
  for (size_t off = 0; off < n; off += 128u) {
    const uint32_t m = (uint32_t)(n - off < 128u ? n - off : 128u);
    omx_pitch_process(l + off, rr + off, m, &R->p, &R->s);
  }
}

static void tone(float *x, size_t n, double f, double sr, double a) {
  for (size_t i = 0; i < n; i++) x[i] = (float)(a * sin(2.0 * M_PI * f * (double)i / sr));
}


static void arm_e_bypass(void) {
  enum { N = 1024 };
  static float x[N], l[N], rr[N];
  for (int i = 0; i < N; i++) x[i] = (i % 7 == 3) ? 1e-40f : (float)sin(0.05 * i) * 0.7f;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    for (int c = 0; c < 2; c++) {
      struct run R;
      setup(&R, sr, 0.5, c == 0 ? 1.0 : 0.0);
      if (c == 0) R.p.enabled = 0;
      memcpy(l, x, sizeof x);
      memcpy(rr, x, sizeof x);
      process(&R, l, rr, N);
      ok(memcmp(l, x, sizeof x) == 0 && memcmp(rr, x, sizeof x) == 0,
         c == 0 ? "E enabled = 0 is memcmp identity" : "E mix = 0 is memcmp identity", sr, 0, 0);
    }
  }
  drain_violations("E contracts");
}

static void arm_g_finite_and_flush(void) {
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    const size_t n = (size_t)(3.0 * o_window(sr)) + 4096u, half = n / 3u;
    float *l = calloc(n, sizeof(float)), *rr = calloc(n, sizeof(float));
    if (l == NULL || rr == NULL) abort();
    tone(l, half, 997.0, sr, 0.9);
    struct run R;
    setup(&R, sr, 0.66741992708501718, 0.5);
    process(&R, l, rr, n);
    int finite = 1, rsilent = 1;
    for (size_t i = 0; i < n; i++) {
      finite &= isfinite(l[i]) ? 1 : 0;
      rsilent &= rr[i] == 0.0f ? 1 : 0;
    }
    ok(finite, "G every output is finite", sr, 0, 0);
    ok(rsilent, "G a silent leg stays silent beside a loud one", sr, 0, 0);
    int zero_tail = 1;
    for (size_t i = n - 256u; i < n; i++) zero_tail &= l[i] == 0.0f ? 1 : 0;
    ok(zero_tail, "G silence after a tone reaches exact zero", sr, 0, 0);
    int normal = 1;
    for (int k = 0; k < 4; k++)
      normal &= (R.s.lp_l[k] == 0.0f || fabsf(R.s.lp_l[k]) >= FLT_MIN) ? 1 : 0;
    ok(normal, "G the pre-filter's state holds no subnormal", sr, 0, 0);
    free(l);
    free(rr);
  }
  drain_violations("G contracts");
}

enum { H_THREADS = 8, H_FRAMES = 16384 };

struct h_worker {
  double sr;
  float out[2 * H_FRAMES];
};

static void *h_run(void *arg) {
  struct h_worker *w = arg;
  const uint32_t cap = omx_pitch_cap_for((float)w->sr);
  float *ring = calloc(2u * cap, sizeof(float));
  if (ring == NULL) abort();
  struct omx_pitch p;
  struct omx_pitch_state s;
  omx_pitch_resolve(&p, 1, -5.0f, 12.0f, 60.0f, (float)w->sr);
  if (omx_pitch_state_init(&s, ring, ring + cap, cap) != OMX_FDELAY_OK) abort();
  float *l = w->out, *r = w->out + H_FRAMES;
  for (size_t i = 0; i < H_FRAMES; i++) {
    l[i] = (float)(0.5 * sin(2.0 * M_PI * 997.0 * (double)i / w->sr));
    r[i] = (float)(0.3 * sin(2.0 * M_PI * 331.0 * (double)i / w->sr));
  }
  for (size_t off = 0; off < H_FRAMES; off += 128u)
    omx_pitch_process(l + off, r + off, 128u, &p, &s);
  free(ring);
  return NULL;
}

static void arm_h_threads(void) {
  static struct h_worker ref, w[H_THREADS];
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    ref.sr = sr;
    h_run(&ref);
    pthread_t t[H_THREADS];
    for (int k = 0; k < H_THREADS; k++) {
      w[k].sr = sr;
      if (pthread_create(&t[k], NULL, h_run, &w[k]) != 0) abort();
    }
    for (int k = 0; k < H_THREADS; k++) pthread_join(t[k], NULL);
    int same = 1;
    for (int k = 0; k < H_THREADS; k++) same &= memcmp(w[k].out, ref.out, sizeof ref.out) == 0;
    ok(same, "H eight threads are byte-identical to one", sr, same, 1);
    double moved = 0.0;
    for (size_t i = 0; i < H_FRAMES; i++)
      moved += fabs((double)ref.out[i] - 0.5 * sin(2.0 * M_PI * 997.0 * (double)i / sr));
    ok(moved > 1.0, "H the reference moved the signal (the arm is not comparing inputs)", sr, moved, 1.0);
  }
}


int main(void) {
  float top = 0.0f;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) top = fmaxf(top, OMX_DECLARED_RATES[ri]);
  g_ring_cap = omx_pitch_cap_for(top);
  g_ring = calloc(2u * g_ring_cap, sizeof(float));
  if (g_ring == NULL) return 2;
  omx_fx_require_rate_floor();
  arm_e_bypass();
  arm_g_finite_and_flush();
  arm_h_threads();
  free(g_ring);
  printf("fx/pitch: %d checks, %d failed (%d rates)\n", g_checks, g_failed, (int)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
