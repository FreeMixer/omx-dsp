// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The sub-octaver's instance oracle (omx_suboct_instance.h, the FX-17 design's oracle table) at each of
// the nine oracle rates (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  each range, a sine at its lowest, middle and highest tracked fundamental: the first octave's
//      lines are the divider's series times the sections' closed forms at the declared corners,
//      A_t 8/(3 pi) |LP24(f/2)| at f/2 and A_t 8/(5 pi) |LP24(3f/2)| at 3f/2, A_t the sine through
//      the tracking band (0.05 and 0.1 dB, a Hann-windowed DTFT over the last second), and the line at
//      f stays 60 dB under the sine;
//   B  the second octave, at the same nine fundamentals: its line at f/4 leads every other line and is
//      the second divider's line derived from the first octave's whole series (second_line), through
//      post2's low-pass, within 0.05 dB (the card's arm B);
//   C  110 Hz with uniform noise 20 dB under its peak: one toggle per input period over 1.8 s, and
//      trackedHz's period is the tone's within 2/44100 s and a sample (noise moves each toggle's sample,
//      never its count, L3); a dip 1.08 SUBOCT_HYSTERESIS of the peak deep arms the divider, one 0.8 of
//      it deep does not;
//   D  each toggle is the first sample at or after the tracking band's upward crossing, and agrees with
//      the coarsest rate's within one of its samples; the same second toggles as often at every rate;
//   E  560 Hz at 0 dBFS through the high range: no output step passes twice the input's largest (L4);
//   F  the level is bounded: over band-limited noise, RMS(y) <= (dry + oct1 + oct2)/100 RMS(x)
//      (+0.05 dB) at the corners of the three levels, each range;
//   G  a note decaying through trackGateDb (-40 dBFS, and a non-finite word read as the default): the
//      octaves fade out SUBOCT_GATE_RAMP_MS, within a sample, after the envelope computed here from the
//      declared times crosses it, the output is then the dry alone, and trackedHz reads 0;
//   H  oct1 = oct2 = 0 and dry = 100, or bypassed, is the input bit for bit; random blocks are one
//      block; identical legs stay identical; the latency is 0;
//   I  the declarations read back: the range table's corners, the envelope's times, the gate's ramp,
//      the arming level SUBOCT_HYSTERESIS e, the travels' ends and defaults; nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_suboct_instance.h>

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

/* The declarations, stated here so a moved one turns its arm red (suboct_instance-perturb.sh). */
static const double LO[3] = {35, 70, 140}, HI[3] = {140, 280, 560}, POST1[3] = {90, 180, 360}, POST2[3] = {45, 90, 180};
#define HYST 0.25
#define ENV_ATTACK_MS 1.0f
#define ENV_RELEASE_MS 100.0f
#define RAMP_MS 5.0
static const float DEFAULTS[5] = {0, 100, 0, 100, -50};

#define MAXN 800000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxSuboctInstance g_s;
static uint32_t g_toggles[OMX_FX_ORACLE_RATE_COUNT];
static double g_first[64]; /* the coarsest rate's first toggles after the settle, s */
static unsigned g_ri;

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static void setup(int range, float o1, float o2, float dry, float gate) {
  omx_suboct_instance_init(&g_s, g_sr);
  drain();
  omx_suboct_instance_resolve(&g_s, 0, range, o1, o2, dry, gate);
  drain();
}
static void run(OmxSuboctInstance *s, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) {
    omx_suboct_instance_run(s, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
    drain();
  }
}
static void tone(float *x, uint32_t n, double f, double a) {
  for (uint32_t i = 0; i < n; i++) x[i] = (float)(a * sin(2.0 * M_PI * f * i / g_sr));
}
/* The closed forms, from the declared corners alone and never from the instance's coefficients. The
 * EQ's high-pass is the cookbook's, its analog prototype through the bilinear transform prewarped at
 * the corner, H(e^jw) = H_a(jW), W = tan(pi f/rate) / tan(pi fc/rate); its low-pass is matched to
 * the analog prototype, H_a(j f/fc), whose magnitude it meets (its phase runs a fraction of a sample
 * behind, a delay that moves no line's magnitude). The tracking band is a 12 dB high-pass at LO/2 and
 * a 12 dB low-pass at HI, Q 1/sqrt 2; each octave's low-pass is 24 dB, two sections at the
 * fourth-order Butterworth Qs 1/(2 cos(pi/8)) and 1/(2 cos(3 pi/8)). */
static double complex lp2(double f, double fc, double q) {
  const double w = f / fc;
  return 1.0 / (1.0 - w * w + I * w / q);
}
static double complex hp2(double f, double fc, double q) {
  const double w = tan(M_PI * f / g_sr) / tan(M_PI * fc / g_sr);
  return -w * w / (1.0 - w * w + I * w / q);
}
static double complex lp24(double f, double fc) {
  return lp2(f, fc, 0.5 / cos(M_PI / 8.0)) * lp2(f, fc, 0.5 / cos(3.0 * M_PI / 8.0));
}
static double complex band(int k, double f) { return hp2(f, 0.5 * LO[k], M_SQRT1_2) * lp2(f, HI[k], M_SQRT1_2); }
/* The divider's series on A sin: D = A 2|sin phi| cos phi, phi the half-rate phase from an upward
 * crossing, a cosine series over the odd m: 8/(3 pi) at m = 1 and -8/(pi (2j - 1)(2j + 3)) at m = 2j + 1. */
static double series(int m) {
  if (m == 1) return 8.0 / (3.0 * M_PI);
  const int j = (m - 1) / 2;
  return -8.0 / (M_PI * (2 * j - 1) * (2 * j + 3));
}
/* The first octave, s1(phi) = A_t sum_m series(m) |H1| cos(m phi + arg H1), H1 post1's low-pass at
 * m f/2: its lines below Nyquist into c[] and mm[], their count returned. */
#define SERIES_LINES 64
static int first_octave(int k, double f, double at, double complex c[SERIES_LINES], int mm[SERIES_LINES]) {
  int n = 0;
  for (int m = 1; n < SERIES_LINES && 0.5 * m * f < 0.5 * g_sr; m += 2, n++) {
    c[n] = at * series(m) * lp24(0.5 * m * f, POST1[k]);
    mm[n] = m;
  }
  return n;
}
static double s1_at(const double complex *c, const int *mm, int n, double phi) {
  double v = 0.0;
  for (int i = 0; i < n; i++) v += creal(c[i] * cexp(I * mm[i] * phi));
  return v;
}
/* The second octave's f/4 line, derived. The second divider toggles q2 at s1's upward crossing phi0
 * after s1 has dipped under -HYST of its peak; q2's period is 4 pi in phi. Over it the second half
 * repeats the first (s1 has period 2 pi, q2 and e^{-i phi/2} both change sign), so the line is
 *   |D2 at f/4| = (1/pi) |int_{phi0}^{phi0 + 2 pi} s1(phi) e^{-i phi/2} dphi|,
 * each cosine of s1 integrated in closed form. On a pure s1 = L cos phi this is 8 L/(3 pi), the
 * series applied to s1's f/2 line (the design card's arm B). s1's own 3f/2, 5f/2, ... lines, which
 * post1 passes, add to it: +0.551 dB at a range's middle fundamental (post1/f = 9/7, the same in
 * every range and at every rate), +1.137 dB at the low range's lowest (35 Hz under a 90 Hz post1),
 * -0.007 dB at its highest. The kernel's f/4 line carries that term; the series on the f/2 line alone
 * does not, which was the constant +0.551 dB the 1 dB bound stood over. */
static double second_line(int k, double f, double at) {
  double complex c[SERIES_LINES];
  int mm[SERIES_LINES];
  const int n = first_octave(k, f, at, c, mm);
  const int grid = 8192;
  double peak = 0.0;
  for (int i = 0; i < grid; i++) peak = fmax(peak, fabs(s1_at(c, mm, n, 2.0 * M_PI * i / grid)));
  double phi0 = -1.0, prev = s1_at(c, mm, n, 0.0);
  int armed = 0;
  for (int i = 1; i < 2 * grid && phi0 < 0.0; i++) {
    const double p = 2.0 * M_PI * i / grid, v = s1_at(c, mm, n, p);
    if (v < -HYST * peak) armed = 1;
    if (armed && prev < 0.0 && v >= 0.0) {
      double a = p - 2.0 * M_PI / grid, b = p;
      for (int it = 0; it < 60; it++) {
        const double mid = 0.5 * (a + b);
        if (s1_at(c, mm, n, mid) < 0.0) a = mid;
        else b = mid;
      }
      phi0 = b;
    }
    prev = v;
  }
  double complex integral = 0.0;
  for (int i = 0; i < n; i++) {
    const double kp = mm[i] - 0.5, km = -mm[i] - 0.5;
    integral += 0.5 * c[i] * (cexp(I * kp * (phi0 + 2.0 * M_PI)) - cexp(I * kp * phi0)) / (I * kp);
    integral += 0.5 * conj(c[i]) * (cexp(I * km * (phi0 + 2.0 * M_PI)) - cexp(I * km * phi0)) / (I * km);
  }
  return cabs(integral) / M_PI;
}
/* A line's amplitude at f over x[from, from + len), Hann-windowed. */
static double line(const float *x, uint32_t from, uint32_t len, double f) {
  double complex h = 0.0;
  double wsum = 0.0;
  for (uint32_t i = 0; i < len; i++) {
    const double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / len);
    h += w * x[from + i] * cexp(-I * 2.0 * M_PI * f * (from + i) / g_sr);
    wsum += w;
  }
  return 2.0 * cabs(h) / wsum;
}
static double db(double x) { return 20.0 * log10(x); }

/* ---- A, B: the lines --------------------------------------------------------------------------- */
static void arm_lines(void) {
  const uint32_t n = (uint32_t)(2.0 * g_sr), from = (uint32_t)g_sr, len = (uint32_t)g_sr;
  double worst_a = 0.0, worst_b = 0.0;
  for (int k = 0; k < 3; k++) {
    const double F[3] = {LO[k], sqrt(LO[k] * HI[k]), HI[k]}; /* the lowest, middle and highest tracked */
    for (int p = 0; p < 3; p++) {
      g_arm = "A";
      const double f = F[p], a = 0.5;
      tone(g_l, n, f, a);
      memcpy(g_r, g_l, n * sizeof(float));
      setup(k, 100.0f, 0.0f, 0.0f, -80.0f);
      run(&g_s, g_l, g_r, g_ol, g_or, n);
      const double at = a * cabs(band(k, f));
      const double l1 = line(g_ol, from, len, 0.5 * f), w1 = at * series(1) * cabs(lp24(0.5 * f, POST1[k]));
      const double l3 = line(g_ol, from, len, 1.5 * f), w3 = at * -series(3) * cabs(lp24(1.5 * f, POST1[k]));
      const double lf = line(g_ol, from, len, f);
      ok(fabs(db(l1 / w1)) < 0.05, "the f/2 line is A_t 8/(3 pi) |LP24| (0.05 dB)", db(l1 / w1), 0.0);
      ok(fabs(db(l3 / w3)) < 0.1, "the 3f/2 line is A_t 8/(5 pi) |LP24| (0.1 dB)", db(l3 / w3), 0.0);
      ok(db(lf / a) < -60.0, "the line at f stays 60 dB under the sine", db(lf / a), -60.0);
      worst_a = fmax(worst_a, fabs(db(l1 / w1)));
      g_arm = "B";
      setup(k, 0.0f, 100.0f, 0.0f, -80.0f);
      run(&g_s, g_l, g_r, g_ol, g_or, n);
      const double l4 = line(g_ol, from, len, 0.25 * f), w4 = second_line(k, f, at) * cabs(lp24(0.25 * f, POST2[k]));
      int leads = 1;
      for (int j = 1; j < 8; j++) leads &= line(g_ol, from, len, (2 * j + 1) * 0.25 * f) < l4;
      ok(leads, "the second octave's f/4 line leads every other line", leads, 1);
      ok(fabs(db(l4 / w4)) < 0.05, "the f/4 line is the second divider's derived line through post2 (0.05 dB)", db(l4 / w4), 0.0);
      worst_b = fmax(worst_b, fabs(db(l4 / w4)));
    }
  }
  printf("  suboct @ %.0f Hz: f/2 worst %.4f dB, f/4 worst %.4f dB against the closed forms\n", (double)g_sr, worst_a, worst_b);
  clean();
}

/* ---- C, D: the toggles ------------------------------------------------------------------------- */
/* The arming level, seen from outside: y = sin t + c sin 2t (c > 1/2) has, each period, a deep dip
 * (minus its peak) and a shallow one whose depth over the peak grows with c. Driven through the
 * tracking band's inverse, the band's output is y: a shallow dip deeper than HYST of the peak arms the
 * divider a second time each period, one shallower does not. */
static double shallow_ratio(double c) {
  double top = 0.0, small = 0.0;
  for (int i = 0; i < 20000; i++) {
    const double t = 2.0 * M_PI * i / 20000.0, y = sin(t) + c * sin(2.0 * t);
    if (t < M_PI) top = fmax(top, y);
    else small = fmax(small, y);
  }
  return small / top;
}
static uint32_t dip_toggles(double ratio, double f, uint32_t *want_periods) {
  double lo = 0.5, hi = 5.0;
  for (int it = 0; it < 50; it++) {
    const double mid = 0.5 * (lo + hi);
    if (shallow_ratio(mid) < ratio) lo = mid;
    else hi = mid;
  }
  const double c = 0.5 * (lo + hi);
  const double complex h1 = band(1, f), h2 = band(1, 2.0 * f);
  const uint32_t n = (uint32_t)(1.2 * g_sr), from = (uint32_t)(0.2 * g_sr);
  for (uint32_t i = 0; i < n; i++) {
    const double t = 2.0 * M_PI * f * i / g_sr;
    g_l[i] = g_r[i] = (float)(0.3 * (sin(t - carg(h1)) / cabs(h1) + c * sin(2.0 * t - carg(h2)) / cabs(h2)));
  }
  setup(1, 100.0f, 0.0f, 0.0f, -80.0f);
  run(&g_s, g_l, g_r, g_ol, g_or, from);
  const uint32_t t0 = g_s.d1.toggles;
  run(&g_s, g_l + from, g_r + from, g_ol + from, g_or + from, n - from);
  *want_periods = (uint32_t)lround(f * (n - from) / g_sr);
  return g_s.d1.toggles - t0;
}
static void arm_toggles(void) {
  g_arm = "C";
  const double f = 110.0;
  const uint32_t n = (uint32_t)(2.0 * g_sr), from = (uint32_t)(0.2 * g_sr);
  tone(g_l, n, f, 0.5);
  uint32_t lcg = 0x5eedu;
  for (uint32_t i = 0; i < n; i++) {
    lcg = lcg * 1664525u + 1013904223u;
    g_l[i] += (float)(0.05 * ((double)(lcg >> 8) / 8388608.0 - 1.0)); /* uniform, 20 dB under the peak */
  }
  memcpy(g_r, g_l, n * sizeof(float));
  setup(1, 100.0f, 0.0f, 0.0f, -80.0f);
  run(&g_s, g_l, g_r, g_ol, g_or, from);
  const uint32_t t0 = g_s.d1.toggles;
  run(&g_s, g_l + from, g_r + from, g_ol + from, g_or + from, n - from);
  const uint32_t count = g_s.d1.toggles - t0, want = (uint32_t)lround(f * (n - from) / g_sr);
  ok(count + 1u >= want && count <= want + 1u, "one toggle per input period under noise 20 dB down", count, want);
  const double th = omx_suboct_instance_tracked_hz(&g_s);
  ok(fabs(1.0 / th - 1.0 / f) <= 2.0 / 44100.0 + 1.0 / g_sr, "trackedHz's period is the tone's within 2/44100 s and a sample",
     th, f);
  uint32_t periods = 0u;
  const uint32_t deep = dip_toggles(1.08 * HYST, f, &periods);
  ok(deep + 1u >= 2u * periods && deep <= 2u * periods + 1u, "a dip 1.08 HYST of the peak deep arms: two toggles a period", deep,
     2.0 * periods);
  const uint32_t shallow = dip_toggles(0.8 * HYST, f, &periods);
  ok(shallow + 1u >= periods && shallow <= periods + 1u, "a dip 0.8 HYST of the peak deep does not arm: one toggle a period",
     shallow, periods);
  clean();

  /* D: each toggle lands on the first sample at or after the tracking band's upward crossing, the
   * band designed here at the declared corners and its crossing interpolated between the samples
   * around it: within one sample of this rate. Each toggle's time agrees with the coarsest rate's
   * within one sample of it (the card's arm D), and the same second toggles as often at every rate. */
  g_arm = "D";
  const uint32_t m = (uint32_t)g_sr, settle = (uint32_t)(0.2 * g_sr);
  tone(g_l, m, f, 0.5);
  memcpy(g_r, g_l, m * sizeof(float));
  setup(1, 100.0f, 0.0f, 0.0f, -80.0f);
  double hp[5], lp[5], hs[4] = {0, 0, 0, 0}, ls[4] = {0, 0, 0, 0};
  omx_eq_design(OMX_EQ_HIGHPASS, 0.5 * LO[1], M_SQRT1_2, 0.0, g_sr, hp);
  omx_eq_design(OMX_EQ_LOWPASS, HI[1], M_SQRT1_2, 0.0, g_sr, lp);
  double early = 1.0, late = -1.0, tc = -1.0, drift = 0.0, prev = 0.0;
  uint32_t seen = 0u, crossings = 0u, toggled = 0u;
  for (uint32_t i = 0; i < m; i++) {
    const double y = (float)omx_biquad_d(omx_biquad_d(g_l[i], hp, hs), lp, ls);
    if (prev < 0.0 && y >= 0.0) {
      tc = (i - 1.0 + prev / (prev - y)) / g_sr;
      if (i >= settle) crossings++;
    }
    prev = y;
    const uint32_t t1 = g_s.d1.toggles;
    omx_suboct_instance_run(&g_s, g_l + i, g_r + i, g_ol + i, g_or + i, 1u);
    if ((i & 255u) == 255u) drain();
    if (g_s.d1.toggles == t1 || i < settle) continue;
    const double t = (double)i / g_sr;
    toggled++;
    early = fmin(early, t - tc);
    late = fmax(late, t - tc);
    if (seen < 64u) {
      if (g_ri == 0u) g_first[seen] = t;
      drift = fmax(drift, fabs(t - g_first[seen]));
      seen++;
    }
  }
  ok(toggled == crossings && early >= 0.0 && late < 1.0 / g_sr,
     "each toggle is the first sample at or after the band's crossing (one sample)", late * g_sr, 1.0);
  ok(seen == 64u && drift < 1.0 / OMX_FX_ORACLE_RATES[0], "each toggle's time is the coarsest rate's within one of its samples",
     drift * OMX_FX_ORACLE_RATES[0], 1.0);
  printf("  suboct @ %.0f Hz: toggles %.4f to %.4f samples after the band's crossing, %.4f coarse samples from the coarsest rate\n",
         (double)g_sr, early * g_sr, late * g_sr, drift * OMX_FX_ORACLE_RATES[0]);
  g_toggles[g_ri] = g_s.d1.toggles;
  ok(g_s.d1.toggles == g_toggles[0], "the same second of tone toggles as often at every rate", g_s.d1.toggles, g_toggles[0]);
  clean();
}

/* ---- E: no step; F: the level -------------------------------------------------------------- */
static void arm_bounds(void) {
  g_arm = "E";
  const uint32_t n = (uint32_t)(0.5 * g_sr);
  tone(g_l, n, 560.0, 1.0);
  memcpy(g_r, g_l, n * sizeof(float));
  setup(2, 100.0f, 0.0f, 0.0f, -80.0f);
  run(&g_s, g_l, g_r, g_ol, g_or, n);
  double din = 0.0, dout = 0.0;
  for (uint32_t i = 1; i < n; i++) din = fmax(din, fabs(g_l[i] - g_l[i - 1])), dout = fmax(dout, fabs(g_ol[i] - g_ol[i - 1]));
  ok(dout <= 2.0 * din, "no output step passes twice the input's largest", dout, 2.0 * din);
  clean();

  g_arm = "F";
  const uint32_t m = (uint32_t)(1.0 * g_sr);
  for (int k = 0; k < 3; k++) {
    double c[5], st[4] = {0, 0, 0, 0};
    omx_eq_design(OMX_EQ_BANDPASS, sqrt(LO[k] * HI[k]), 0.7, 0.0, g_sr, c);
    uint32_t lcg = 77u + (uint32_t)k;
    for (uint32_t i = 0; i < m; i++) {
      lcg = lcg * 1664525u + 1013904223u;
      const double v = (double)(lcg >> 8) / 8388608.0 - 1.0;
      const double y = c[0] * v + c[1] * st[0] + c[2] * st[1] - c[3] * st[2] - c[4] * st[3];
      st[1] = st[0], st[0] = v, st[3] = st[2], st[2] = y;
      g_l[i] = g_r[i] = (float)(0.5 * y);
    }
    const double rin = rms(g_l, 0, m);
    int under = 1;
    double worst = -1e9;
    for (int corner = 1; corner < 8; corner++) {
      const float o1 = corner & 1 ? 100.0f : 0.0f, o2 = corner & 2 ? 100.0f : 0.0f, dr = corner & 4 ? 100.0f : 0.0f;
      setup(k, o1, o2, dr, -80.0f);
      run(&g_s, g_l, g_r, g_ol, g_or, m);
      const double bound = (o1 + o2 + dr) / 100.0 * rin, e = db(rms(g_ol, 0, m) / bound);
      worst = fmax(worst, e);
      under &= e <= 0.05;
    }
    ok(under, "RMS(y) <= (dry + oct1 + oct2)/100 RMS(x) (+0.05 dB)", worst, 0.05);
  }
  clean();
}

/* ---- G: the gate ------------------------------------------------------------------------------- */
/* The envelope the gate reads, computed here from the declared times: the low range's tracking band
 * designed at the declared corners, then the peak cascade at ENV_ATTACK_MS and ENV_RELEASE_MS. The
 * crossing is where it falls under the gate; the instance's own envelope is never read. */
static void gate_once(float gate_word, float gate_db) {
  const uint32_t n = (uint32_t)(2.0 * g_sr);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(0.03 * exp(-(double)i / (0.25 * g_sr)) * sin(2.0 * M_PI * 70.0 * i / g_sr));
  setup(0, 100.0f, 50.0f, 60.0f, gate_word);
  double hp[5], lp[5], hs[4] = {0, 0, 0, 0}, ls[4] = {0, 0, 0, 0};
  omx_eq_design(OMX_EQ_HIGHPASS, 0.5 * LO[0], M_SQRT1_2, 0.0, g_sr, hp);
  omx_eq_design(OMX_EQ_LOWPASS, HI[0], M_SQRT1_2, 0.0, g_sr, lp);
  const struct omx_env_params ep = {omx_pole_from_time_ms(ENV_ATTACK_MS, g_sr), omx_pole_from_time_ms(ENV_RELEASE_MS, g_sr), OMX_DETECT_PEAK};
  float ac, rc;
  omx_env_stage_poles(&ep, 1u, &ac, &rc);
  struct omx_env env;
  memset(&env, 0, sizeof env);
  const float gl = omx_db_to_lin(gate_db);
  const double ramp = RAMP_MS * 0.001 * g_sr;
  int64_t cross = -1, gone = -1;
  int dry = 1;
  for (uint32_t i = 0; i < n; i++) {
    omx_suboct_instance_run(&g_s, g_l + i, g_r + i, g_ol + i, g_or + i, 1u);
    if ((i & 31u) == 31u) drain();
    const float xt = (float)omx_biquad_d(omx_biquad_d(g_l[i], hp, hs), lp, ls);
    const float e = omx_env_step(&env, &ep, fabsf(xt), ac, rc);
    for (int k = 0; k < OMX_DYN_ENV_STAGES; k++) env.stage[k] = omx_flush(env.stage[k]); /* as each block's end */
    if (cross < 0 && i > g_sr * 0.1 && e < gl) cross = i;
    if (cross >= 0 && gone < 0 && g_s.g == 0.0f) gone = i;
    if (gone >= 0) dry &= g_ol[i] == g_s.dr * g_l[i] && omx_suboct_instance_tracked_hz(&g_s) == 0.0f;
  }
  ok(cross > 0 && fabs((double)(gone - cross) - ramp) <= 1.0,
     "the octaves fade out SUBOCT_GATE_RAMP_MS after the declared envelope crosses the gate (a sample)", (double)(gone - cross), ramp);
  ok(dry, "once gated the output is the dry alone and trackedHz reads 0", dry, 1);
}
static void arm_gate(void) {
  g_arm = "G";
  gate_once(-40.0f, -40.0f);
  gate_once(NAN, DEFAULTS[4]); /* a non-finite gate reads as the declared default */
  clean();
}

/* ---- H: identities; I: readback ------------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "H";
  const uint32_t n = (uint32_t)(0.3 * g_sr);
  stimulus(g_l, g_r, n, 0x3c3cu, 1200u);
  setup(1, 0.0f, 0.0f, 100.0f, -80.0f);
  run(&g_s, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0,
     "both octaves 0 and dry 100 are the input bit for bit", 0, 0);
  ok(g_s.d1.toggles > 0u, "with the divider still tracking", g_s.d1.toggles, 1);
  omx_suboct_instance_init(&g_s, g_sr);
  omx_suboct_instance_resolve(&g_s, 1, 0, 100.0f, 100.0f, 50.0f, -80.0f);
  drain();
  run(&g_s, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "bypassed is the input bit for bit", 0, 0);
  setup(1, 80.0f, 40.0f, 70.0f, -80.0f);
  omx_suboct_instance_run(&g_s, g_l, g_l, g_ol, g_or, n);
  drain();
  ok(memcmp(g_ol, g_or, n * sizeof(float)) == 0, "identical legs stay identical", 0, 0);
  setup(1, 80.0f, 40.0f, 70.0f, -80.0f);
  uint32_t lcg = 9u;
  for (uint32_t o = 0; o < n;) {
    lcg = lcg * 1664525u + 1013904223u;
    uint32_t b = 1u + (lcg >> 8) % 4096u;
    if (b > n - o) b = n - o;
    omx_suboct_instance_run(&g_s, g_l + o, g_l + o, g_xl + o, g_xr + o, b);
    drain();
    o += b;
  }
  ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0, "random blocks of 1 to 4096 frames are one block", 0, 0);
  ok(omx_suboct_instance_latency(&g_s) == 0u, "no latency", 0, 0);
  clean();

  g_arm = "I";
  g_counting = 1;
  g_allocs = 0;
  int table = 1;
  for (int k = 0; k < 3; k++) {
    setup(k, 100.0f, 0.0f, 100.0f, -50.0f);
    double hp[5], lp[5], p1[2][5], p2[2][5];
    omx_eq_design(OMX_EQ_HIGHPASS, 0.5 * LO[k], 0.70710678118654752, 0.0, g_sr, hp);
    omx_eq_design(OMX_EQ_LOWPASS, HI[k], 0.70710678118654752, 0.0, g_sr, lp);
    omx_suboct_lp24(p1, POST1[k], g_sr);
    omx_suboct_lp24(p2, POST2[k], g_sr);
    table &= memcmp(hp, g_s.hp, sizeof hp) == 0 && memcmp(lp, g_s.lp, sizeof lp) == 0 && memcmp(p1, g_s.p1, sizeof p1) == 0 &&
             memcmp(p2, g_s.p2, sizeof p2) == 0;
  }
  ok(table, "each range's corners are the declared table's", table, 1);
  ok(g_s.ep.attack_pole == omx_pole_from_time_ms(ENV_ATTACK_MS, g_sr) && g_s.ep.release_pole == omx_pole_from_time_ms(ENV_RELEASE_MS, g_sr),
     "the envelope's times are 1 and 100 ms", g_s.ep.attack_pole, 0.0);
  ok(fabs(g_s.ramp_step * RAMP_MS * 0.001 * g_sr - 1.0) < 1e-6, "the gate's ramp is 5 ms", g_s.ramp_step, 0.0);
  ok(omx_suboct_arm_level(1.0f) == (float)HYST, "the arming level is SUBOCT_HYSTERESIS of the peak", omx_suboct_arm_level(1.0f), HYST);
  setup(9, NAN, NAN, NAN, NAN);
  int ends = g_s.rng == (int)DEFAULTS[0] && g_s.o1 == DEFAULTS[1] * 0.01f && g_s.o2 == DEFAULTS[2] * 0.01f &&
             g_s.dr == DEFAULTS[3] * 0.01f && g_s.gate_db == DEFAULTS[4];
  setup(0, 1e9f, 1e9f, 1e9f, 1e9f);
  ends &= g_s.o1 == 1.0f && g_s.o2 == 1.0f && g_s.dr == 1.0f && g_s.gate_db == -20.0f;
  setup(0, -1e9f, -1e9f, -1e9f, -1e9f);
  ends &= g_s.o1 == 0.0f && g_s.o2 == 0.0f && g_s.dr == 0.0f && g_s.gate_db == -80.0f;
  ok(ends, "words outside land on the declared ends and defaults", ends, 1);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0.0);
  OmxSuboctInstance bad;
  ok(omx_suboct_instance_init(&bad, 22050.0f) == 0, "a rate outside the oracle set is refused", 0, 0);
  clean();
}

int main(void) {
  (void)sine;
  (void)expect_clean;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (g_ri = 0; g_ri < OMX_FX_ORACLE_RATE_COUNT; g_ri++) {
    g_sr = OMX_FX_ORACLE_RATES[g_ri];
    arm_lines();
    arm_toggles();
    arm_bounds();
    arm_gate();
    arm_identity();
  }
  return instance_oracle_end("suboct_instance");
}
