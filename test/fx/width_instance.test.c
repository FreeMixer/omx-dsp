// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The width face's instance oracle (omx_width_instance.h over omx_width.h, FX-11) at each of the nine
// oracle rates (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  width 100 % with no pseudo-stereo and no bass mono is the input bit for bit, at any delay and
//      corner; so are a fresh instance and a bypassed one;
//   B  the M/S law: L = M + w S, R = M - w S, M = (L + R)/2, S = (L - R)/2, w = widthPct/100, written
//      out in double (4 ulp of the block's peak); width 0 is mono, both legs the same bits;
//   C  the complementary comb: a mono impulse comes out 1 then +g on the left and -g on the right at
//      d = round(pseudoMs rate / 1000), g WIDTH_COMB_GAIN (0.5), exactly; on noise the legs sum to twice
//      the mid (4 ulp);
//   D  Haas: a right-leg impulse comes out on the right d samples late, exactly, the left silent;
//   E  bass mono: a side impulse comes out as the LR4 high band, |H| = W^4/(1 + W^4), W = tan(pi f/rate)
//      / tan(pi fc/rate), on each leg with opposite signs (1e-3 dB where it is above -40 dB, past which a
//      float impulse response's rounding is a measurable share); a mono impulse comes out through the
//      all-pass, |H| = 1 (1e-4 dB), the same on both legs;
//   F  hostile words land on the declared ends and defaults, read back; nothing allocates; random
//      blocks are one block; a rate outside the oracle set is refused; the ledger is clean.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_width_instance.h>

#include "instance_oracle.h"

#include <complex.h>
#include <float.h>
#include <stdlib.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
static int g_counting = 0;
static long g_allocs = 0;
void *__wrap_malloc(size_t n) {
  if (g_counting) g_allocs++;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t sz) {
  if (g_counting) g_allocs++;
  return __real_calloc(n, sz);
}
void *__wrap_realloc(void *p, size_t n) {
  if (g_counting) g_allocs++;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (g_counting) g_allocs++;
  __real_free(p);
}

/* The declarations, stated here so a moved one turns its arm red (width_instance-perturb.sh). */
#define GAIN 0.5
static const float DEFAULTS[5] = {100, 0, 12, 0, 120};
static const float TOP[5] = {200, 0, 30, 0, 300};
static const float FOOT[5] = {0, 0, 0, 0, 20};

#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxWidthInstance g_w;

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static void setup(float w, int pseudo, float ms, int bass, float hz) {
  omx_width_instance_init(&g_w, g_sr);
  omx_width_instance_resolve(&g_w, 0, w, pseudo, ms, bass, hz);
  drain();
}
static void run(OmxWidthInstance *s, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) {
    omx_width_instance_run(s, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
    drain();
  }
}
static int same(const float *a, const float *b, uint32_t n) { return memcmp(a, b, n * sizeof(float)) == 0; }

/* ---- A: the come-up values ------------------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "A";
  const uint32_t n = (uint32_t)(0.2f * g_sr);
  stimulus(g_l, g_r, n, 0x1d1du, 2000u);
  int id = 1;
  static const float MS[3] = {0.0f, 7.3f, 30.0f};
  static const float HZ[3] = {20.0f, 120.0f, 300.0f};
  for (int k = 0; k < 3; k++) {
    setup(100.0f, 0, MS[k], 0, HZ[k]);
    run(&g_w, g_l, g_r, g_ol, g_or, n);
    id &= same(g_ol, g_l, n) && same(g_or, g_r, n);
  }
  ok(id, "100 %, no pseudo-stereo, no bass mono is the input bit for bit", id, 1);
  omx_width_instance_init(&g_w, g_sr);
  run(&g_w, g_l, g_r, g_ol, g_or, n);
  ok(same(g_ol, g_l, n) && same(g_or, g_r, n), "a fresh instance is the input bit for bit", 0, 0);
  omx_width_instance_init(&g_w, g_sr);
  omx_width_instance_resolve(&g_w, 1, 180.0f, 1, 9.0f, 1, 150.0f);
  run(&g_w, g_l, g_r, g_ol, g_or, n);
  ok(same(g_ol, g_l, n) && same(g_or, g_r, n), "a bypassed instance is the input bit for bit", 0, 0);
  clean();
}

/* ---- B: the M/S law -------------------------------------------------------------------------- */
static void arm_ms(void) {
  g_arm = "B";
  const uint32_t n = (uint32_t)(0.1f * g_sr);
  stimulus(g_l, g_r, n, 0xabcdu, 1000u);
  for (uint32_t i = 0; i < n; i += 3u) g_r[i] = 0.3f * g_l[(i * 7u) % n]; /* a side that is not -L/2 */
  static const float WS[4] = {0.0f, 37.0f, 150.0f, 200.0f};
  double peak = 0.0;
  for (uint32_t i = 0; i < n; i++) peak = fmax(peak, fmax(fabs(g_l[i]), fabs(g_r[i])));
  for (int k = 0; k < 4; k++) {
    setup(WS[k], 0, 12.0f, 0, 120.0f);
    run(&g_w, g_l, g_r, g_ol, g_or, n);
    const double w = WS[k] / 100.0;
    double worst = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const double m = 0.5 * ((double)g_l[i] + g_r[i]), s = 0.5 * ((double)g_l[i] - g_r[i]);
      worst = fmax(worst, fmax(fabs(g_ol[i] - (m + w * s)), fabs(g_or[i] - (m - w * s))));
    }
    ok(worst <= 4.0 * FLT_EPSILON * peak * (1.0 + w), "L = M + w S, R = M - w S (4 ulp)", worst, 4.0 * FLT_EPSILON * peak);
    if (WS[k] == 0.0f) ok(same(g_ol, g_or, n), "width 0 is mono, both legs the same bits", 0, 0);
  }
  clean();
}

/* ---- C, D: the comb and Haas ----------------------------------------------------------------- */
static void arm_pseudo(void) {
  static const float MS[4] = {0.5f, 7.3f, 12.0f, 29.7f};
  const uint32_t n = (uint32_t)(0.04f * g_sr);
  for (int k = 0; k < 4; k++) {
    const uint32_t d = (uint32_t)lround(MS[k] * (double)g_sr / 1000.0);
    g_arm = "C";
    memset(g_l, 0, n * sizeof(float));
    memset(g_r, 0, n * sizeof(float));
    g_l[0] = g_r[0] = 1.0f;
    setup(100.0f, 1, MS[k], 0, 120.0f);
    run(&g_w, g_l, g_r, g_ol, g_or, n);
    int at = g_ol[0] == 1.0f && g_or[0] == 1.0f && g_ol[d] == (float)GAIN && g_or[d] == (float)-GAIN;
    for (uint32_t i = 1; i < n; i++)
      if (i != d) at &= g_ol[i] == 0.0f && g_or[i] == 0.0f;
    ok(at, "a mono impulse: 1, then +g left and -g right at d = round(pseudoMs rate / 1000)", g_ol[d], GAIN);
    g_arm = "D";
    memset(g_l, 0, n * sizeof(float));
    memset(g_r, 0, n * sizeof(float));
    g_r[0] = 1.0f;
    setup(100.0f, 2, MS[k], 0, 120.0f);
    run(&g_w, g_l, g_r, g_ol, g_or, n);
    int h = g_or[d] == 1.0f;
    for (uint32_t i = 0; i < n; i++) {
      h &= g_ol[i] == 0.0f;
      if (i != d) h &= g_or[i] == 0.0f;
    }
    ok(h, "Haas: the right leg d samples late, the left silent", g_or[d], 1.0);
  }
  g_arm = "C";
  const uint32_t m = (uint32_t)(0.2f * g_sr);
  stimulus(g_l, g_r, m, 0x5151u, 1500u);
  setup(140.0f, 1, 9.0f, 0, 120.0f);
  run(&g_w, g_l, g_r, g_ol, g_or, m);
  double worst = 0.0, peak = 0.0;
  for (uint32_t i = 0; i < m; i++) {
    const double e = fabs(((double)g_ol[i] + g_or[i]) - ((double)g_l[i] + g_r[i]));
    if (!(e <= worst)) worst = e; /* a comparison, not fmax: BUILDING.md, GCC 12.2 on arm64 */
    peak = fmax(peak, fabs(g_ol[i]) + fabs(g_or[i]));
  }
  ok(worst <= 4.0 * FLT_EPSILON * peak, "the comb's legs sum to twice the mid (4 ulp)", worst, 4.0 * FLT_EPSILON * peak);
  clean();
}

/* ---- E: bass mono ---------------------------------------------------------------------------- */
static double dtft(const float *x, uint32_t n, double f) {
  const double complex z = cexp(-I * 2.0 * M_PI * f / g_sr);
  double complex h = 0.0, zn = 1.0;
  for (uint32_t i = 0; i < n; i++) h += x[i] * zn, zn *= z;
  return cabs(h);
}
static void arm_bass(void) {
  g_arm = "E";
  static const float HZ[3] = {40.0f, 120.0f, 300.0f};
  const uint32_t n = (uint32_t)g_sr;
  double worst = 0.0;
  for (int k = 0; k < 3; k++) {
    memset(g_l, 0, n * sizeof(float));
    memset(g_r, 0, n * sizeof(float));
    g_l[0] = 1.0f, g_r[0] = -1.0f; /* a side impulse */
    setup(100.0f, 0, 12.0f, 1, HZ[k]);
    run(&g_w, g_l, g_r, g_ol, g_or, n);
    int opp = 1;
    for (uint32_t i = 0; i < n; i++) opp &= g_or[i] == -g_ol[i];
    ok(opp, "the side comes out on the legs with opposite signs", opp, 1);
    const double kc = tan(M_PI * HZ[k] / g_sr);
    for (double f = 10.0; f < 0.45 * g_sr; f *= 1.25) {
      const double w = tan(M_PI * f / g_sr) / kc, want = pow(w, 4) / (1.0 + pow(w, 4));
      if (20.0 * log10(want) < -40.0) continue;
      const double e = fabs(20.0 * log10(dtft(g_ol, n, f) / want));
      worst = fmax(worst, e);
      ok(e < 1e-3, "the side is the LR4 high band, W^4/(1 + W^4) (1e-3 dB)", e, 1e-3);
    }
    g_l[0] = g_r[0] = 1.0f; /* a mono impulse */
    setup(100.0f, 0, 12.0f, 1, HZ[k]);
    run(&g_w, g_l, g_r, g_ol, g_or, n);
    int eq = 1; /* by value: the all-pass's tail may leave -0 on one leg and +0 on the other */
    for (uint32_t i = 0; i < n; i++) eq &= g_ol[i] == g_or[i];
    ok(eq, "the mid comes out on both legs, the same values", eq, 1);
    for (double f = 10.0; f < 0.45 * g_sr; f *= 1.6) {
      const double e = fabs(20.0 * log10(dtft(g_ol, n, f)));
      ok(e < 1e-4, "the mid passes the all-pass at unity (1e-4 dB)", e, 1e-4);
    }
  }
  printf("  width @ %.0f Hz: bass mono's side against the LR4 high band, worst %.2e dB\n", (double)g_sr, worst);
  clean();
}

/* ---- F: hostile words, readback, blocks ------------------------------------------------------ */
static void arm_hostile(void) {
  g_arm = "F";
  const uint32_t n = (uint32_t)(0.1f * g_sr);
  stimulus(g_l, g_r, n, 0x4040u, 800u);
  g_counting = 1;
  g_allocs = 0;
  setup(1e9f, 1, 1e9f, 1, 1e9f);
  const int top = g_w.wpct == TOP[0] && g_w.pms == TOP[2] && g_w.bhz == TOP[4];
  setup(-1e9f, 1, -1e9f, 1, -1e9f);
  const int foot = g_w.wpct == FOOT[0] && g_w.pms == FOOT[2] && g_w.bhz == FOOT[4];
  setup(NAN, 9, NAN, -4, NAN);
  const int dflt = g_w.wpct == DEFAULTS[0] && g_w.pmode == 0 && g_w.pms == DEFAULTS[2] && g_w.bmode == 0 && g_w.bhz == DEFAULTS[4];
  ok(top && foot && dflt, "words past, under and outside land on the declared top, foot and default", top + 2 * foot + 4 * dflt, 7);
  setup(160.0f, 1, 9.0f, 1, 150.0f);
  omx_width_instance_run(&g_w, g_l, g_r, g_ol, g_or, n);
  drain();
  setup(160.0f, 1, 9.0f, 1, 150.0f);
  uint32_t lcg = 7u;
  for (uint32_t o = 0; o < n;) {
    lcg = lcg * 1664525u + 1013904223u;
    uint32_t b = 1u + (lcg >> 8) % 4096u;
    if (b > n - o) b = n - o;
    omx_width_instance_run(&g_w, g_l + o, g_r + o, g_xl + o, g_xr + o, b);
    drain();
    o += b;
  }
  ok(same(g_ol, g_xl, n) && same(g_or, g_xr, n), "random blocks of 1 to 4096 frames are one block", 0, 0);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0.0);
  OmxWidthInstance bad;
  ok(omx_width_instance_init(&bad, 22050.0f) == 0, "a rate outside the oracle set is refused", 0, 0);
  ok(omx_width_instance_latency(&g_w) == 0u, "no latency", 0, 0);
  clean();
}

int main(void) {
  (void)sine;
  (void)rms;
  (void)expect_clean;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (unsigned ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_identity();
    arm_ms();
    arm_pseudo();
    arm_bass();
    arm_hostile();
  }
  return instance_oracle_end("width_instance");
}
