// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The spectral balancer (fx/omx_balancer_instance.h) at each of the nine oracle rates:
//   A  an amount of 0 and a bypass are the input, bit for bit; a rate outside the set is refused;
//   B  on four steady tones at the bands' centres, at known levels, each band's settled gain is the closed
//      form: D[k] from the tones through the cookbook bandpass, written out here, T[k] the mean with the
//      slope about the bands' middle, gain -amount (D - T), inside the range (0.05 dB); each tone leaves at
//      its level times the sections' product (0.1 dB);
//   C  a pink spectrum on a pink target moves under 0.65 dB per band, the top one 1.6; at a slope of 0 its bottom comes down;
//   D  the range stops a band exactly on it, the speed walks it speedDbPerS in a second (one tick);
//   E  under the gate the gains hold, and they hold until the detector has settled;
//   F  one run cut into blocks of 1, 64, 1024 is the same output, bit for bit;
//   G  hostile words land inside the travels, the ledger is clean, nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_balancer_instance.h>
#include <omxdsp/omx_magnitude.h>

#include "instance_oracle.h"
#include "instance_no_alloc.h"

#include <complex.h>

#define NB 4
static const double HZ[NB] = {125.0, 500.0, 2000.0, 8000.0};
#define Q 0.7
#define BAND_DB_OCT 3.0103
#define PINK 96
#define MAXN 2000000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxBalancerInstance g_b;

static void brun(const float *l, const float *r, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 2048u) {
    omx_balancer_instance_run(&g_b, l + o, r + o, ol + o, orr + o, n - o < 2048u ? n - o : 2048u);
    drain();
  }
}

/* the constant-0 dB-peak bandpass's power gain at f, the cookbook's transfer */
static double bp2(double fc, double f) {
  const double w0 = 2.0 * M_PI * fc / (double)g_sr, alpha = sin(w0) / (2.0 * Q), w = 2.0 * M_PI * f / (double)g_sr;
  const double complex z = cexp(I * w), zi = 1.0 / z;
  const double complex h = (alpha / (1.0 + alpha)) * (1.0 - zi * zi) /
                           (1.0 - (2.0 * cos(w0) / (1.0 + alpha)) * zi + ((1.0 - alpha) / (1.0 + alpha)) * zi * zi);
  return creal(h * conj(h));
}

/* the bands' wants from any set of tones: each band's power through its bandpass, against the target */
static void law_of(const double *f, const double *amp, int m, double amount, double slope, double range, double want[NB]) {
  double d[NB], mean = 0.0, mlog = 0.0;
  for (int k = 0; k < NB; k++) {
    double p = 0.0;
    for (int j = 0; j < m; j++) p += 0.5 * amp[j] * amp[j] * bp2(HZ[k], f[j]);
    d[k] = 10.0 * log10(p);
    mean += d[k] / NB;
    mlog += log2(HZ[k]) / NB;
  }
  for (int k = 0; k < NB; k++) {
    double g = -amount * (d[k] - (mean + (slope + BAND_DB_OCT) * (log2(HZ[k]) - mlog)));
    g = g > range ? range : g < -range ? -range : g;
    want[k] = g;
  }
}
static void law(const double amp[NB], double amount, double slope, double range, double want[NB]) {
  law_of(HZ, amp, NB, amount, slope, range, want);
}

static void tones(uint32_t n, const double amp[NB]) {
  for (uint32_t i = 0; i < n; i++) {
    double x = 0.0;
    for (int k = 0; k < NB; k++) x += amp[k] * sin(2.0 * M_PI * HZ[k] * (double)i / (double)g_sr);
    g_l[i] = g_r[i] = (float)x;
  }
}

static double goertzel(const float *x, uint32_t from, uint32_t n, double f) {
  const double complex st = cexp(-I * 2.0 * M_PI * f / (double)g_sr);
  double complex w = 1.0, a = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    /* a Hann window keeps the other tones out */
    const double h = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)n);
    a += h * (double)x[from + i] * w;
    w *= st;
  }
  return 4.0 * cabs(a) / (double)n;
}

static void arm_identity(void) {
  g_arm = "A";
  ok(omx_balancer_instance_init(&g_b, 11025.0f) == 0, "a rate outside the oracle set is refused", 1, 0);
  const uint32_t n = 4096u;
  stimulus(g_l, g_r, n, 0x1212u, 512u);
  omx_balancer_instance_init(&g_b, g_sr);
  brun(g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "an amount of 0 is the input", 1, 0);
  omx_balancer_instance_resolve(&g_b, 1, 100.0f, 12.0f, 6.0f, -3.0f);
  brun(g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "a bypass is the input", 1, 0);
  clean();
}

static void arm_settle(void) {
  g_arm = "B";
  static const double AMP[NB] = {0.2, 0.02, 0.1, 0.01};
  static const float AM[] = {100.0f, 50.0f};
  const uint32_t n = (uint32_t)(5.0f * g_sr), w = (uint32_t)(0.5f * g_sr);
  tones(n, AMP);
  for (int a = 0; a < 2; a++) {
    omx_balancer_instance_init(&g_b, g_sr);
    omx_balancer_instance_resolve(&g_b, 0, AM[a], 12.0f, 6.0f, -3.0f);
    brun(g_l, g_r, g_ol, g_or, n);
    double want[NB];
    law(AMP, (double)AM[a] * 0.01, -3.0, 12.0, want);
    for (int k = 0; k < NB; k++) {
      const double got = omx_balancer_instance_band_gain_db(&g_b, (uint32_t)k);
      ok(fabs(got - want[k]) < 0.05, "each band settles on the closed form (0.05 dB)", got, want[k]);
    }
    for (int j = 0; j < NB; j++) {
      double db = 0.0;
      for (int k = 0; k < NB; k++) db += omx_biquad_magnitude_db(g_b.eq[k], HZ[j], (double)g_sr, -200.0);
      const double in = goertzel(g_l, n - w, w, HZ[j]), out = goertzel(g_ol, n - w, w, HZ[j]);
      ok(fabs(20.0 * log10(out / in) - db) < 0.1, "each tone leaves at the sections' product (0.1 dB)", 20.0 * log10(out / in), db);
    }
  }
  clean();
}

static void arm_target(void) {
  g_arm = "C";
  /* a pink spectrum: tones spaced evenly in log frequency at one level, 30 Hz to 14 kHz, scattered phases */
  double f[PINK], amp[PINK], ph[PINK];
  uint32_t seed = 0x1357u;
  for (int k = 0; k < PINK; k++) {
    f[k] = 30.0 * pow(14000.0 / 30.0, (double)k / (PINK - 1));
    amp[k] = 0.02;
    seed = seed * 1664525u + 1013904223u;
    ph[k] = 2.0 * M_PI * (double)(seed >> 8) / 16777216.0;
  }
  const uint32_t n = (uint32_t)(5.0f * g_sr);
  for (uint32_t i = 0; i < n; i++) {
    double x = 0.0;
    for (int k = 0; k < PINK; k++) x += amp[k] * sin(2.0 * M_PI * f[k] * (double)i / (double)g_sr + ph[k]);
    g_l[i] = g_r[i] = (float)x;
  }
  omx_balancer_instance_init(&g_b, g_sr);
  omx_balancer_instance_resolve(&g_b, 0, 100.0f, 12.0f, 6.0f, -3.0f);
  brun(g_l, g_r, g_ol, g_or, n);
  double want[NB];
  law_of(f, amp, PINK, 1.0, -3.0, 12.0, want);
  for (int k = 0; k < NB; k++) {
    const double got = omx_balancer_instance_band_gain_db(&g_b, (uint32_t)k);
    ok(fabs(got - want[k]) < 0.25, "a pink spectrum sits on the closed form (0.25 dB)", got, want[k]);
    /* the comb stops at 14 kHz, under the top band's skirt: that band alone is lifted, 1.6 dB at most at 32 kHz */
    ok(fabs(got) < (k == NB - 1 ? 1.6 : 0.65), "and on a pink target hardly moves", got, 0.65);
  }
  /* the same signal at a slope of 0 is lifted at the top and pulled at the bottom */
  omx_balancer_instance_init(&g_b, g_sr);
  omx_balancer_instance_resolve(&g_b, 0, 100.0f, 12.0f, 6.0f, 0.0f);
  brun(g_l, g_r, g_ol, g_or, n);
  ok(omx_balancer_instance_band_gain_db(&g_b, 0u) < -2.0f && omx_balancer_instance_band_gain_db(&g_b, 3u) > 2.0f,
     "at a slope of 0 the bottom comes down and the top comes up", omx_balancer_instance_band_gain_db(&g_b, 3u), 2);
  clean();
}

static void arm_range_speed(void) {
  g_arm = "D";
  static const double AMP[NB] = {0.3, 0.003, 0.003, 0.003};
  const uint32_t n = (uint32_t)(5.0f * g_sr);
  tones(n, AMP);
  omx_balancer_instance_init(&g_b, g_sr);
  omx_balancer_instance_resolve(&g_b, 0, 100.0f, 2.0f, 6.0f, -3.0f);
  brun(g_l, g_r, g_ol, g_or, n);
  ok(omx_balancer_instance_band_gain_db(&g_b, 0u) == -2.0f, "the range stops a band exactly on it", omx_balancer_instance_band_gain_db(&g_b, 0u), -2);
  /* the walk: after the 2 s the detector holds, 1 s at 1 dB/s */
  const uint32_t t0 = (uint32_t)(2.5f * g_sr), t1 = (uint32_t)(1.0f * g_sr);
  omx_balancer_instance_init(&g_b, g_sr);
  omx_balancer_instance_resolve(&g_b, 0, 100.0f, 12.0f, 1.0f, -3.0f);
  brun(g_l, g_r, g_ol, g_or, t0);
  const double g0 = omx_balancer_instance_band_gain_db(&g_b, 0u);
  brun(g_l + t0, g_r + t0, g_ol + t0, g_or + t0, t1);
  const double g1 = omx_balancer_instance_band_gain_db(&g_b, 0u);
  ok(fabs((g0 - g1) - 1.0) < 0.03, "the walk is its speed: 1 dB in a second", g0 - g1, 1.0);
  clean();
}

static void arm_gate(void) {
  g_arm = "E";
  static const double AMP[NB] = {0.2, 0.02, 0.1, 0.01};
  const uint32_t early = (uint32_t)(1.9f * g_sr), n = (uint32_t)(4.0f * g_sr);
  tones(n, AMP);
  omx_balancer_instance_init(&g_b, g_sr);
  omx_balancer_instance_resolve(&g_b, 0, 100.0f, 12.0f, 6.0f, -3.0f);
  brun(g_l, g_r, g_ol, g_or, early);
  ok(omx_balancer_instance_band_gain_db(&g_b, 0u) == 0.0f, "until the detector has settled nothing moves", omx_balancer_instance_band_gain_db(&g_b, 0u), 0);
  brun(g_l + early, g_r + early, g_ol + early, g_or + early, n - early);
  memset(g_xl, 0, n * sizeof(float));
  brun(g_xl, g_xl, g_ol, g_or, n);
  const float held = omx_balancer_instance_band_gain_db(&g_b, 0u);
  brun(g_xl, g_xl, g_ol, g_or, n / 2u);
  ok(omx_balancer_instance_band_gain_db(&g_b, 0u) == held, "under the gate the gains hold", omx_balancer_instance_band_gain_db(&g_b, 0u), held);
  ok(held != 0.0f, "and the hold is of a moved gain", held, 0);
  clean();
}

static void arm_cut(void) {
  g_arm = "F";
  static const uint32_t cuts[] = {1u, 64u, 1024u};
  const uint32_t n = (uint32_t)(2.5f * g_sr);
  stimulus(g_l, g_r, n, 0x5151u, 4096u);
  omx_balancer_instance_init(&g_b, g_sr);
  omx_balancer_instance_resolve(&g_b, 0, 100.0f, 6.0f, 6.0f, -3.0f);
  brun(g_l, g_r, g_xl, g_xr, n);
  for (int c = 0; c < 3; c++) {
    omx_balancer_instance_init(&g_b, g_sr);
    omx_balancer_instance_resolve(&g_b, 0, 100.0f, 6.0f, 6.0f, -3.0f);
    for (uint32_t o = 0; o < n; o += cuts[c]) {
      omx_balancer_instance_run(&g_b, g_l + o, g_r + o, g_ol + o, g_or + o, n - o < cuts[c] ? n - o : cuts[c]);
      if ((o & 2047u) == 0u) drain();
    }
    ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0, "a block cut changes no sample", cuts[c], 0);
  }
  clean();
}

static void arm_hostile(void) {
  g_arm = "G";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  stimulus(g_l, g_r, 512u, 0x77u, 128u);
  g_counting = 1;
  for (int a = 0; a < 6; a++)
    for (int b = 0; b < 6; b++) {
      omx_balancer_instance_init(&g_b, g_sr);
      omx_balancer_instance_resolve(&g_b, 0, W[a], W[b], W[a], W[b]);
      omx_balancer_instance_run(&g_b, g_l, g_r, g_ol, g_or, 512u);
      drain();
      ok(g_b.amount <= 1.0f && g_b.amount >= 0.0f && g_b.range <= 12.0f && g_b.step_db > 0.0f, "hostile words land inside the travels", g_b.amount, 1);
    }
  g_counting = 0;
  ok(g_allocs == 0, "nothing allocates", (double)g_allocs, 0);
  clean();
}

OMX_INSTANCE_ORACLE_MAIN("balancer_instance", arm_identity(); arm_settle(); arm_target(); arm_range_speed(); arm_gate(); arm_cut(); arm_hostile();)
