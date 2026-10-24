// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The gate's key listen and key tilt (omx_gate.h, omx_gate_instance.h) at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz). Off and flat are the gate that shipped: its golden digests
// (gate_instance, gate_knee, gate_keyfilter) do not move. Here:
//   A  the tilt's magnitude, heard through the listen, is the first-order pair's closed form
//      sqrt((a²Ω² + 1)/(Ω² + a²)) about KEY_TILT_PIVOT_HZ at four frequencies and four slopes (0.01 dB);
//   B  the listen is the key after the filter and the tilt, bit for bit against a reference built
//      here, for an external key and for the self key (l + r) / 2;
//   C  the tilt reaches the detector: an 8 kHz key 5 dB under the threshold keeps the gate on its floor
//      when flat and opens it at +6 dB per octave; the listen does not reach the detector (the gate's
//      state with the listen on is the state with it off, bit for bit);
//   D  hostile words land inside the declared travel and the listen's ids, the ledger is clean, and
//      nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_gate_instance.h>

#include "instance_oracle.h"

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

/** The key tilt's pivot, KEY_TILT_PIVOT_HZ as declared, stated here so a moved one turns arm A red. */
#define PIVOT_HZ 1000.0

#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_k[MAXN], g_ol[MAXN], g_or[MAXN];

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static OmxGateInstance g_g;

static void setup(OmxGateInstance *g, int src, float thr, float hp, float lp, int listen, float tilt) {
  omx_gate_instance_init(g, g_sr);
  omx_gate_instance_resolve(g, 0, src, thr, -60.0f, thr, thr, 0.5f, 0.0f, 50.0f, 0.0f, 16.0f, hp, lp, listen, tilt);
  drain();
}

static void run(OmxGateInstance *g, const float *key, const float *il, const float *ir, float *ol, float *orr,
                uint32_t n) {
  for (uint32_t o = 0; o < n; o += 300u) {
    omx_gate_instance_run(g, key ? key + o : NULL, il + o, ir + o, ol + o, orr + o, n - o < 300u ? n - o : 300u);
    drain();
  }
}

static double mag_db(const float *x, uint32_t n, double f) {
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    re += x[i] * cos(2.0 * M_PI * f * i / g_sr);
    im -= x[i] * sin(2.0 * M_PI * f * i / g_sr);
  }
  return 20.0 * log10(2.0 * sqrt(re * re + im * im) / n);
}

/* ---- A: the tilt's closed form ------------------------------------------------------------ */
static void arm_tilt(void) {
  g_arm = "A";
  static const double SLOPE[4] = {-6.0, -3.0, 3.0, 6.0}, F[4] = {80.0, 400.0, 3000.0, 9000.0};
  const uint32_t n = (uint32_t)(g_sr * 0.5f);
  double worst = 0.0;
  for (int s = 0; s < 4; s++)
    for (int q = 0; q < 4; q++) {
      for (uint32_t i = 0; i < n; i++) {
        g_k[i] = (float)(0.25 * sin(2.0 * M_PI * F[q] * i / g_sr));
        g_l[i] = g_r[i] = 0.0f;
      }
      setup(&g_g, 1, -40.0f, 0.0f, 0.0f, (int)OMX_GATE_KEY_LISTENS_ON, (float)SLOPE[s]);
      run(&g_g, g_k, g_l, g_r, g_ol, g_or, n);
      const double sg = SLOPE[s] / (20.0 * log10(2.0)), a2 = (1.0 + sg) / (1.0 - sg);
      const double o = tan(M_PI * F[q] / g_sr) / tan(M_PI * PIVOT_HZ / g_sr);
      const double want = 10.0 * log10((a2 * o * o + 1.0) / (o * o + a2));
      const double got = mag_db(g_ol + n / 2u, n / 2u, F[q]) - 20.0 * log10(0.25);
      worst = fmax(worst, fabs(got - want));
      ok(fabs(got - want) <= 0.01, "the key's tilt is the first-order pair's closed form (0.01 dB)", got, want);
    }
  printf("  %6.0f Hz: A worst tilt gap %.5f dB\n", (double)g_sr, worst);
  clean();
}

/* ---- B: the listen, bit for bit ----------------------------------------------------------- */
static void arm_listen(void) {
  g_arm = "B";
  const uint32_t n = 12000u;
  stimulus(g_l, g_r, n, 0x1357u, 900u);
  stimulus(g_k, g_or, n, 0x2468u, 700u);
  for (int self = 0; self < 2; self++) {
    setup(&g_g, self ? 0 : 1, -40.0f, 150.0f, 7000.0f, (int)OMX_GATE_KEY_LISTENS_ON, -2.5f);
    run(&g_g, g_k, g_l, g_r, g_ol, g_or, n);
    double hc[5], lc[5], tc[5] = {0}, s1[4] = {0}, s2[4] = {0}, s3[4] = {0};
    omx_eq_design(OMX_EQ_HIGHPASS, 150.0, M_SQRT1_2, 0.0, g_sr, hc);
    omx_eq_design(OMX_EQ_LOWPASS, 7000.0, M_SQRT1_2, 0.0, g_sr, lc);
    const double sg = -2.5 / OMX_TILT_OCTAVE_DB, a = sqrt((1.0 + sg) / (1.0 - sg)), w = tan(M_PI * PIVOT_HZ / g_sr);
    tc[0] = (a + w) / (1.0 + a * w);
    tc[1] = (w - a) / (1.0 + a * w);
    tc[3] = (a * w - 1.0) / (1.0 + a * w);
    int same = 1;
    for (uint32_t i = 0; i < n; i++) {
      const float k = self ? 0.5f * (g_l[i] + g_r[i]) : g_k[i];
      const float y = omx_biquad_d(omx_biquad_d(omx_biquad_d(k, hc, s1), lc, s2), tc, s3);
      same &= g_ol[i] == y && g_or[i] == y;
    }
    ok(same, self ? "listen, self key: (l + r) / 2 filtered and tilted on both outputs, bit for bit"
                  : "listen, external key: the key filtered and tilted on both outputs, bit for bit",
       self, 0);
  }
  clean();
}

/* ---- C: the tilt reaches the detector, the listen does not --------------------------------- */
static void arm_detector(void) {
  g_arm = "C";
  const uint32_t n = (uint32_t)(g_sr * 0.5f);
  const double ka = pow(10.0, -45.0 / 20.0);
  for (uint32_t i = 0; i < n; i++) {
    g_k[i] = (float)(ka * sin(2.0 * M_PI * 8000.0 * i / g_sr));
    g_l[i] = g_r[i] = 0.1f;
  }
  for (int t = 0; t < 2; t++) {
    setup(&g_g, 1, -40.0f, 0.0f, 0.0f, 0, t ? 6.0f : 0.0f);
    run(&g_g, g_k, g_l, g_r, g_ol, g_or, n);
    const double gain = 20.0 * log10(g_ol[n - 1u] / 0.1);
    if (t) ok(gain > -0.5, "+6 dB per octave lifts an 8 kHz key over the threshold: the gate opens", gain, 0.0);
    else ok(gain < -50.0, "flat, the 8 kHz key 5 dB under the threshold keeps the gate on its floor", gain, -60.0);
  }
  static OmxGateInstance a, b;
  stimulus(g_l, g_r, n, 0x99u, 2000u);
  /* no edge and flat, so the gate detects on its own legs and the listen is all that differs */
  setup(&a, 0, -30.0f, 0.0f, 0.0f, 0, 0.0f);
  setup(&b, 0, -30.0f, 0.0f, 0.0f, (int)OMX_GATE_KEY_LISTENS_ON, 0.0f);
  run(&a, NULL, g_l, g_r, g_ol, g_or, n);
  run(&b, NULL, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(&a.gate.st, &b.gate.st, sizeof a.gate.st) == 0, "the listen does not reach the detector: the state is the same",
     0, 0);
  clean();
}

/* ---- D: hostile words, no allocation ------------------------------------------------------- */
static void arm_hostile(void) {
  g_arm = "D";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  for (unsigned h = 0; h < sizeof W / sizeof W[0]; h++) {
    omx_gate_instance_init(&g_g, g_sr);
    omx_gate_instance_resolve(&g_g, 0, 1, -40.0f, -60.0f, -40.0f, -40.0f, 0.5f, 0.0f, 50.0f, 0.0f, 16.0f, 0.0f, 0.0f, 7,
                              W[h]);
    drain();
    const double amax = omx_tilt_a_of_slope(OMX_GATE_KEY_TILT_DB_RANGE_MAX), amin = omx_tilt_a_of_slope(OMX_GATE_KEY_TILT_DB_RANGE_MIN);
    ok(g_g.gate.listen == 0 && g_g.gate.tilt.a >= amin && g_g.gate.tilt.a <= amax,
       "hostile words: the listen at its default, the tilt inside its travel", h, 0);
    stimulus(g_l, g_r, 4096u, 0x5u, 300u);
    run(&g_g, g_l, g_l, g_r, g_ol, g_or, 4096u);
  }
  g_allocs = 0;
  g_counting = 1;
  setup(&g_g, 1, -40.0f, 100.0f, 9000.0f, 1, 4.0f);
  run(&g_g, g_l, g_l, g_r, g_ol, g_or, 4096u);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0);
  clean();
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  (void)sine;
  (void)rms;
  (void)expect_clean;
  for (unsigned r = 0; r < OMX_FX_ORACLE_RATE_COUNT; r++) {
    g_sr = OMX_FX_ORACLE_RATES[r];
    arm_tilt();
    arm_listen();
    arm_detector();
    arm_hostile();
  }
  return instance_oracle_end("gate_key");
}
