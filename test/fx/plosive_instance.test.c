// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The plosive tamer's instance oracle (omx_plosive_instance.h, the FX-13 design's oracle table) at each
// of the nine oracle rates (OMX_FX_ORACLE_RATES, 32 to 192 kHz), on a synthetic corpus whose expected
// values have closed forms: a vowel (140 Hz harmonics through two formants), t bursts (noise through a
// 2 to 8 kHz band, 1 ms rise, 15 ms decay, +30 dB over the vowel's band) and p pops (a 60 Hz
// half-cosine, 30 ms, +12 dB over the vowel's low band):
//   A  the pop split at a frozen gain g is |((1 + g) + (g - 1) A)/2|, A the first-order allpass at
//      popHz (0.005 dB over a log grid), never above 0 dB;
//   B  the consonant split at a frozen gain is |1 + (g - 1) B|, B the band's section (1e-3 dB), never
//      above 0 dB, and exactly g at the band's centre, where B is 1 (1e-3 dB);
//   C  each t burst drives the consonant cut to its full consonantDb (0.01 dB) while the pop cut stays
//      off, and over the samples where the cut is full the band's peak falls by at least a third of it:
//      noise across a two-octave band is cut by consonantDb at the centre only (B), so the fall is
//      printed, not pinned to consonantDb;
//   D  a vowel alone at eight levels from -60 to 0 dBFS is the input bit for bit after its first 50 ms
//      at a sensitivity above the contrast it raises (L2); at the declared 10 dB, which this buzzy vowel
//      exceeds, its steady cuts are printed (the design leaves that default open, to a recorded take);
//   E  each p pop drives the pop cut to its full popDb (0.01 dB), and above 2 kHz (a 24 dB per octave
//      high-pass, clear of the vowel's 700 Hz formant) the output is within 0.15 dB of the input;
//   F  the cuts do not depend on the level: past the onset the per-sample gains at -20 dB and at 0 dB
//      agree (1e-3 dB);
//   G  no gain is added: over the corpus each 10 ms window's RMS is at most the input's (+1e-4 dB);
//   H  bypassed, or both amounts 0, is the input bit for bit, subnormal words included; random blocks
//      are one block; identical legs stay identical; the latency is 0;
//   I  hostile words land on the travels' ends, non-finite ones on the declared defaults; nothing
//      allocates; a rate outside the oracle set is refused;
//   J  the band's top edge is held under PLOSIVE_BAND_EDGE_FRACTION of the rate: the section is the
//      bandpass designed from the held edges, written out here.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_plosive_instance.h>

#include "instance_oracle.h"

#include <complex.h>
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

/* The declarations, stated here so a moved one turns its arm red (plosive_instance-perturb.sh). */
static const float DEFAULTS[9] = {6, 10, 8, 4000, 2, 12, 10, 40, 150};
/* The corpus arms run both sensitivities at 14.5 dB, arm D at 16: the vowel's steady contrast reaches
 * 13.5 dB in the consonant band and 10.5 dB in the pop band (arm D prints it), above the declared 10, and
 * its onset more for a few ms past 50. */
static const float CORPUS[9] = {6, 14.5f, 8, 4000, 2, 12, 14.5f, 40, 150};
static const float VOWEL[9] = {6, 16, 8, 4000, 2, 12, 16, 40, 150};
#define EDGE 0.45

#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static float g_gc[MAXN], g_gp[MAXN];
static OmxPlosiveInstance g_p;

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static void setup_with(const float *c) {
  omx_plosive_instance_init(&g_p, g_sr);
  drain();
  omx_plosive_instance_resolve(&g_p, 0, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
  drain();
}
static void run(OmxPlosiveInstance *p, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) {
    omx_plosive_instance_run(p, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
    drain();
  }
}
/* One sample a block: each sample's cuts, linear, into g_gc and g_gp. */
static void run_gains(const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    omx_plosive_instance_run(&g_p, il + i, ir + i, ol + i, orr + i, 1u);
    g_gc[i] = g_p.cons_min;
    g_gp[i] = g_p.pop_min;
    if ((i & 31u) == 31u) drain();
  }
  drain();
}

/* ---- the corpus ------------------------------------------------------------------------------ */
/* A cookbook bandpass in double, for building the bursts and measuring bands. */
static void bp_design(double f, double q, double c[5]) { omx_eq_design(OMX_EQ_BANDPASS, f, q, 0.0, g_sr, c); }
static double bq(double x, const double c[5], double s[4]) {
  const double y = c[0] * x + c[1] * s[0] + c[2] * s[1] - c[3] * s[2] - c[4] * s[3];
  s[1] = s[0], s[0] = x, s[3] = s[2], s[2] = y;
  return y;
}
static void vowel(float *x, uint32_t n, double amp) {
  for (uint32_t i = 0; i < n; i++) {
    const double t = i / (double)g_sr, ramp = t < 0.02 ? t / 0.02 : 1.0;
    double v = 0.0;
    for (int k = 1; k <= 24; k++) {
      const double f = 140.0 * k;
      if (f > 0.45 * g_sr) break;
      const double w = (1.0 + 4.0 / (1.0 + pow((f - 700.0) / 150.0, 2)) + 2.4 / (1.0 + pow((f - 1200.0) / 200.0, 2))) / k;
      v += w * sin(2.0 * M_PI * f * t + 0.3 * k);
    }
    x[i] = (float)(amp * ramp * v / 3.0);
  }
}
/* `count` t bursts of band noise at `start` + k `gap` seconds, peak `amp`, into x (added). */
static void bursts(float *x, uint32_t n, double start, double gap, int count, double amp) {
  double c[5], s[4] = {0, 0, 0, 0}, s2[4] = {0, 0, 0, 0};
  bp_design(4000.0, omx_eq_octaves_q(2.0), c);
  uint32_t lcg = 0xb1b1u;
  for (uint32_t i = 0; i < n; i++) {
    lcg = lcg * 1664525u + 1013904223u;
    const double noise = bq(bq((double)(int32_t)(lcg >> 8) / 8388608.0 - 1.0, c, s), c, s2);
    const double t = i / (double)g_sr;
    double env = 0.0;
    for (int k = 0; k < count; k++) {
      const double u = t - (start + k * gap);
      if (u >= 0.0 && u < 0.001) env = u / 0.001;
      else if (u >= 0.001 && u < 0.08) env = exp(-(u - 0.001) / 0.015);
    }
    x[i] += (float)(amp * env * noise);
  }
}
/* `count` p pops (60 Hz half-cosine, 30 ms) at `start` + k `gap` seconds, peak `amp`, into x (added). */
static void pops(float *x, uint32_t n, double start, double gap, int count, double amp) {
  for (uint32_t i = 0; i < n; i++) {
    const double t = i / (double)g_sr;
    for (int k = 0; k < count; k++) {
      const double u = t - (start + k * gap);
      if (u >= 0.0 && u < 0.03) x[i] += (float)(amp * sin(M_PI * u / 0.03) * sin(2.0 * M_PI * 60.0 * u));
    }
  }
}
/* The energy of x[from, to) through four cookbook high-pass sections at `f`. */
static double high_energy(const float *x, uint32_t from, uint32_t to, double f) {
  double c[5], s[4][4];
  omx_eq_design(OMX_EQ_HIGHPASS, f, 0.7071, 0.0, g_sr, c);
  memset(s, 0, sizeof s);
  double e = 0.0;
  for (uint32_t i = 0; i < to; i++) {
    const double y = bq(bq(bq(bq(x[i], c, s[0]), c, s[1]), c, s[2]), c, s[3]);
    if (i >= from) e += y * y;
  }
  return e;
}
static double band_energy(const float *x, uint32_t from, uint32_t to, double f, double oct) {
  double c[5], s[4] = {0, 0, 0, 0}, e = 0.0;
  bp_design(f, omx_eq_octaves_q(oct), c);
  for (uint32_t i = 0; i < to; i++) {
    const double y = bq(x[i], c, s);
    if (i >= from) e += y * y;
  }
  return e;
}

/* ---- A, B: the frozen splits ----------------------------------------------------------------- */
static double complex at_w(double f) { return cexp(-I * 2.0 * M_PI * f / g_sr); }
static void arm_splits(void) {
  g_arm = "A";
  setup_with(DEFAULTS);
  static const float GS[] = {1.0f, 0.5f, 0.25f, 0.063f};
  const uint32_t len = (uint32_t)g_sr;
  double worst = 0.0;
  int under = 1;
  for (int k = 0; k < 4; k++) {
    struct omx_allpass1 ap = {0.0f};
    for (uint32_t i = 0; i < len; i++) {
      const float x = i == 0 ? 1.0f : 0.0f;
      g_ol[i] = omx_plosive_pop_apply(x, omx_plosive_pop_band(&ap, g_p.a, x), GS[k]);
    }
    for (double f = 30.0; f < 0.45 * g_sr; f *= 1.21) {
      double complex h = 0.0;
      const double complex z = at_w(f);
      double complex zn = 1.0;
      for (uint32_t i = 0; i < len; i++) h += g_ol[i] * zn, zn *= z;
      const double complex A = (g_p.a + z) / (1.0 + g_p.a * z);
      const double want = cabs(((1.0 + GS[k]) + (GS[k] - 1.0) * A) / 2.0);
      const double e = fabs(20.0 * log10(cabs(h) / want));
      worst = fmax(worst, e);
      ok(e < 0.005, "the pop split is |((1 + g) + (g - 1) A)/2| (0.005 dB)", 20.0 * log10(cabs(h)), 20.0 * log10(want));
      under &= 20.0 * log10(cabs(h)) <= 1e-4;
    }
  }
  ok(under, "the pop split never passes 0 dB", under, 1);
  printf("  plosive @ %.0f Hz: pop split, worst %.2e dB\n", (double)g_sr, worst);
  clean();

  g_arm = "B";
  worst = 0.0;
  under = 1;
  for (int k = 0; k < 4; k++) {
    float s[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < len; i++) {
      const float x = i == 0 ? 1.0f : 0.0f;
      g_ol[i] = omx_plosive_cons_apply(x, omx_biquad(x, g_p.bp, s), GS[k]);
    }
    for (double f = 300.0; f < 0.45 * g_sr; f *= 1.21) {
      double complex h = 0.0;
      const double complex z = at_w(f);
      double complex zn = 1.0;
      for (uint32_t i = 0; i < len; i++) h += g_ol[i] * zn, zn *= z;
      const double *c = (const double[]){g_p.bp[0], g_p.bp[1], g_p.bp[2], g_p.bp[3], g_p.bp[4]};
      const double complex B = (c[0] + c[1] * z + c[2] * z * z) / (1.0 + c[3] * z + c[4] * z * z);
      const double want = cabs(1.0 + (GS[k] - 1.0) * B);
      const double e = fabs(20.0 * log10(cabs(h) / want));
      worst = fmax(worst, e);
      ok(e < 1e-3, "the consonant split is |1 + (g - 1) B| (1e-3 dB)", 20.0 * log10(cabs(h)), 20.0 * log10(want));
      under &= 20.0 * log10(cabs(h)) <= 1e-4;
    }
    /* the band's centre: the bandpass is 1 there, the cut is g */
    {
      const double f0 = sqrt(DEFAULTS[3] / 2.0 * DEFAULTS[3] * 2.0);
      double complex h = 0.0;
      const double complex z = at_w(f0);
      double complex zn = 1.0;
      for (uint32_t i = 0; i < len; i++) h += g_ol[i] * zn, zn *= z;
      ok(fabs(20.0 * log10(cabs(h)) - 20.0 * log10(GS[k])) < 1e-3, "at the band's centre the cut is g exactly (1e-3 dB)",
         20.0 * log10(cabs(h)), 20.0 * log10(GS[k]));
    }
  }
  ok(under, "the consonant split never passes 0 dB", under, 1);
  printf("  plosive @ %.0f Hz: consonant split, worst %.2e dB\n", (double)g_sr, worst);
  clean();
}

/* ---- C, D, E, F, G: the corpus --------------------------------------------------------------- */
static void arm_corpus(void) {
  const uint32_t n = (uint32_t)(1.6 * g_sr);
  /* D: vowels untouched */
  g_arm = "D";
  int same = 1;
  for (int k = 0; k < 8; k++) {
    const double amp = pow(10.0, (-60.0 + 60.0 * k / 7.0) / 20.0);
    vowel(g_l, n, amp);
    memcpy(g_r, g_l, n * sizeof(float));
    setup_with(VOWEL);
    run(&g_p, g_l, g_r, g_ol, g_or, n);
    const uint32_t from = (uint32_t)(0.05 * g_sr);
    same &= memcmp(g_ol + from, g_l + from, (n - from) * sizeof(float)) == 0;
  }
  ok(same, "a vowel alone is the input bit for bit after its first 50 ms, -60 to 0 dBFS", same, 1);
  vowel(g_l, n, 0.1);
  memcpy(g_r, g_l, n * sizeof(float));
  setup_with(DEFAULTS);
  run_gains(g_l, g_r, g_ol, g_or, n);
  const float cd = 20.0f * log10f(g_gc[n - 1u]), pd = 20.0f * log10f(g_gp[n - 1u]);
  printf("  plosive @ %.0f Hz: the vowel at the declared 10 dB: steady cuts %.2f dB (contrast %.1f dB) and %.2f dB (%.1f dB)\n",
         (double)g_sr, cd, 10.0 - 6.0 * cd / DEFAULTS[0], pd, 10.0 - 6.0 * pd / DEFAULTS[5]);
  clean();

  /* C: t bursts over the vowel */
  g_arm = "C";
  vowel(g_l, n, 0.1);
  const double vb = sqrt(band_energy(g_l, (uint32_t)(0.1 * g_sr), n, 4000.0, 2.0) / (n - 0.1 * g_sr));
  bursts(g_l, n, 0.3, 0.4, 3, 31.6 * 3.0 * vb);
  memcpy(g_r, g_l, n * sizeof(float));
  setup_with(CORPUS);
  run_gains(g_l, g_r, g_ol, g_or, n);
  for (int k = 0; k < 3; k++) {
    const uint32_t a = (uint32_t)((0.3 + 0.4 * k) * g_sr), b = a + (uint32_t)(0.06 * g_sr);
    float gmin = 1.0f, pmin = 1.0f;
    for (uint32_t i = a; i < b; i++) gmin = fminf(gmin, g_gc[i]), pmin = fminf(pmin, g_gp[i]);
    ok(fabs(20.0 * log10(gmin) + DEFAULTS[0]) < 0.01, "each t burst drives the consonant cut to its full consonantDb",
       20.0 * log10(gmin), -DEFAULTS[0]);
    ok(pmin > omx_db_to_lin(-0.5f), "a t burst leaves the pop cut off", 20.0 * log10(pmin), -0.5);
    /* the band's peak, in and out, over the samples whose cut is full */
    double c[5], si[4] = {0, 0, 0, 0}, so[4] = {0, 0, 0, 0}, pin = 0.0, pout = 0.0;
    for (int j = 0; j < 5; j++) c[j] = g_p.bp[j];
    const float full = omx_db_to_lin_poly(-CORPUS[0]);
    for (uint32_t i = 0; i < b; i++) {
      const double yi = bq(g_l[i], c, si), yo = bq(g_ol[i], c, so);
      if (i >= a && g_gc[i] == full) pin = fmax(pin, fabs(yi)), pout = fmax(pout, fabs(yo));
    }
    ok(pin > 0.0, "the cut is full on some samples of the burst", pin, 0.0);
    const double red = 20.0 * log10(pin / pout);
    ok(red >= CORPUS[0] / 3.0, "over the full cut the band's peak falls by at least a third of consonantDb", red, CORPUS[0] / 3.0);
    printf("  plosive @ %.0f Hz: t burst %d: band peak over the full cut down %.2f dB\n", (double)g_sr, k, red);
  }
  /* G: no gain added, the bursts and the pops */
  g_arm = "G";
  int under = 1;
  double worst = -1e9;
  const uint32_t w = (uint32_t)(0.01 * g_sr);
  for (uint32_t o = 0; o + w <= n; o += w) {
    const double ri = rms(g_l, o, o + w), ro = rms(g_ol, o, o + w);
    worst = fmax(worst, 20.0 * log10(ro / ri));
    under &= ro <= ri * pow(10.0, 1e-4 / 20.0);
  }
  ok(under, "no gain is added: each 10 ms window's RMS is at most the input's", worst, 1e-4);

  /* F: level independence */
  g_arm = "F";
  memcpy(g_xl, g_gc, n * sizeof(float));
  for (uint32_t i = 0; i < n; i++) g_l[i] *= 0.1f;
  memcpy(g_r, g_l, n * sizeof(float));
  setup_with(CORPUS);
  run_gains(g_l, g_r, g_ol, g_or, n);
  double fw = 0.0;
  for (uint32_t i = (uint32_t)(0.05 * g_sr); i < n; i++) fw = fmax(fw, fabs(20.0 * log10(g_gc[i] / g_xl[i])));
  ok(fw < 1e-3, "the consonant cut does not depend on the level: -20 dB against 0 dB (1e-3 dB)", fw, 1e-3);
  clean();

  /* E: p pops over the vowel */
  g_arm = "E";
  vowel(g_l, n, 0.1);
  pops(g_l, n, 0.3, 0.5, 2, 4.0 * 0.1);
  memcpy(g_r, g_l, n * sizeof(float));
  setup_with(CORPUS);
  run_gains(g_l, g_r, g_ol, g_or, n);
  for (int k = 0; k < 2; k++) {
    const uint32_t a = (uint32_t)((0.3 + 0.5 * k) * g_sr), b = a + (uint32_t)(0.04 * g_sr);
    float pmin = 1.0f;
    for (uint32_t i = a; i < b; i++) pmin = fminf(pmin, g_gp[i]);
    ok(fabs(20.0 * log10(pmin) + DEFAULTS[5]) < 0.01, "each p pop drives the pop cut to its full popDb", 20.0 * log10(pmin), -DEFAULTS[5]);
    const double ein = high_energy(g_l, a, b, 2000.0), eout = high_energy(g_ol, a, b, 2000.0);
    ok(fabs(10.0 * log10(eout / ein)) < 0.15, "above 2 kHz the pop leaves the voice within 0.15 dB", 10.0 * log10(eout / ein), 0.15);
  }
  g_arm = "G";
  under = 1;
  for (uint32_t o = 0; o + w <= n; o += w) under &= rms(g_ol, o, o + w) <= rms(g_l, o, o + w) * pow(10.0, 1e-4 / 20.0);
  ok(under, "no gain is added over the pops", under, 1);
  g_arm = "F";
  memcpy(g_xl, g_gp, n * sizeof(float));
  for (uint32_t i = 0; i < n; i++) g_l[i] *= 0.1f;
  memcpy(g_r, g_l, n * sizeof(float));
  setup_with(CORPUS);
  run_gains(g_l, g_r, g_ol, g_or, n);
  fw = 0.0;
  for (uint32_t i = (uint32_t)(0.05 * g_sr); i < n; i++) fw = fmax(fw, fabs(20.0 * log10(g_gp[i] / g_xl[i])));
  ok(fw < 1e-3, "the pop cut does not depend on the level: -20 dB against 0 dB (1e-3 dB)", fw, 1e-3);
  clean();
}

/* ---- H: identities --------------------------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "H";
  const uint32_t n = (uint32_t)(0.5 * g_sr);
  vowel(g_l, n, 0.3);
  bursts(g_l, n, 0.1, 0.2, 2, 0.8);
  for (uint32_t i = 0; i < n; i += 97u) g_l[i] = 1e-40f; /* subnormal words */
  memcpy(g_r, g_l, n * sizeof(float));
  float zero[9];
  memcpy(zero, DEFAULTS, sizeof zero);
  zero[0] = zero[5] = 0.0f;
  setup_with(zero);
  run(&g_p, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "both amounts 0 are the input bit for bit, the detectors running", 0, 0);
  omx_plosive_instance_init(&g_p, g_sr);
  omx_plosive_instance_resolve(&g_p, 1, 24, 0, 2, 4000, 2, 24, 0, 10, 150);
  drain();
  run(&g_p, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "bypassed is the input bit for bit", 0, 0);
  setup_with(DEFAULTS);
  omx_plosive_instance_run(&g_p, g_l, g_r, g_ol, g_or, n);
  drain();
  setup_with(DEFAULTS);
  uint32_t lcg = 31u;
  for (uint32_t o = 0; o < n;) {
    lcg = lcg * 1664525u + 1013904223u;
    uint32_t b = 1u + (lcg >> 8) % 4096u;
    if (b > n - o) b = n - o;
    omx_plosive_instance_run(&g_p, g_l + o, g_r + o, g_xl + o, g_xr + o, b);
    drain();
    o += b;
  }
  ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0, "random blocks of 1 to 4096 frames are one block", 0, 0);
  ok(memcmp(g_ol, g_or, n * sizeof(float)) == 0, "identical legs stay identical", 0, 0);
  ok(omx_plosive_instance_latency(&g_p) == 0u, "no latency", 0, 0);
  clean();
}

/* ---- I: hostile words; J: the band edge ------------------------------------------------------ */
static int same_as(const float *a, const float *b, uint32_t n) {
  setup_with(a);
  run(&g_p, g_l, g_r, g_ol, g_or, n);
  setup_with(b);
  run(&g_p, g_l, g_r, g_xl, g_xr, n);
  return memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0;
}
static void arm_hostile(void) {
  g_arm = "I";
  const uint32_t n = (uint32_t)(0.4 * g_sr);
  vowel(g_l, n, 0.2);
  bursts(g_l, n, 0.1, 0.15, 2, 0.6);
  pops(g_l, n, 0.2, 0.1, 1, 2.0);
  memcpy(g_r, g_l, n * sizeof(float));
  g_counting = 1;
  g_allocs = 0;
  const float nan9[9] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
  ok(same_as(nan9, DEFAULTS, n), "non-finite words are the declared defaults", 0, 0);
  static const float big9[9] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
  static const float small9[9] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
  /* One word at a time past its travel, the others at the corpus setting, so each end is heard. */
  static const float TOP[9] = {24, 30, 50, 12000, 4, 24, 30, 200, 300};
  static const float FOOT[9] = {0, 0, 2, 1000, 0.5f, 0, 0, 10, 60};
  int tops = 1, feet = 1;
  for (int k = 0; k < 9; k++) {
    float w[9], v[9];
    memcpy(w, CORPUS, sizeof w);
    memcpy(v, CORPUS, sizeof v);
    w[k] = 1e9f, v[k] = TOP[k];
    tops &= same_as(w, v, n);
    w[k] = -1e9f, v[k] = FOOT[k];
    feet &= same_as(w, v, n);
  }
  ok(tops, "a word past its travel is its top", tops, 1);
  ok(feet, "a word under its travel is its foot", feet, 1);
  /* and what the face resolved, read back: an end moved in the declaration moves both sides above */
  const float *got[9] = {&g_p.cons_db, &g_p.cons_s, &g_p.cons_rec, &g_p.band_hz, &g_p.band_oct,
                         &g_p.pop_db,  &g_p.pop_s,  &g_p.pop_rec,  &g_p.pop_hz};
  int ends = 1;
  setup_with(big9);
  for (int k = 0; k < 9; k++) ends &= *got[k] == TOP[k];
  setup_with(small9);
  for (int k = 0; k < 9; k++) ends &= *got[k] == FOOT[k];
  setup_with(nan9);
  for (int k = 0; k < 9; k++) ends &= *got[k] == DEFAULTS[k];
  ok(ends, "the face resolves past, under and non-finite words to the declared top, foot and default", ends, 1);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0.0);
  OmxPlosiveInstance bad;
  ok(omx_plosive_instance_init(&bad, 22050.0f) == 0, "a rate outside the oracle set is refused", 0, 0);
  clean();

  g_arm = "J";
  const float wide[9] = {6, 10, 8, 12000, 4, 12, 10, 40, 150};
  setup_with(wide);
  const double lo = 12000.0 / 4.0, hi = fmin(12000.0 * 4.0, EDGE * g_sr);
  float want[5];
  omx_eq_design_f(OMX_EQ_BANDPASS, sqrt(lo * hi), omx_eq_octaves_q(log2(hi / lo)), 0.0, g_sr, want);
  ok(memcmp(want, g_p.bp, sizeof want) == 0, "the band is designed from its edges, the top held under 0.45 of the rate", g_p.bp[0],
     want[0]);
  clean();
}

int main(void) {
  (void)stimulus;
  (void)sine;
  (void)expect_clean;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (unsigned ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_splits();
    arm_corpus();
    arm_identity();
    arm_hostile();
  }
  return instance_oracle_end("plosive_instance");
}
