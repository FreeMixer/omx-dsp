// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The tone face's instance oracle (omx_tone_instance.h) at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  flat and open is the input, bit for bit: a span of 0 and a filter of 0 at any pivot and
//      resonance, a fresh instance, and a bypassed one at any setting;
//   B  the tilt is a SPAN (omx-dsp#188 ruling R3b): a = 10^(tiltDb/40), the measured gain is 1/a at
//      DC and a at Nyquist (1e-6), 0 dB at the pivot and the prototype's closed form at a quarter and
//      three times the pivot (1e-4 dB), 20 log10 a = tiltDb/2, the slope at the pivot
//      20 log10(2) (a^2 - 1)/(a^2 + 1), about 2 dB per octave at +6; and the face's tilt is ONE
//      section, omx_tilt run by hand, bit for bit;
//   C  the DJ filter: the corner is TONE_FILTER_HIGH_HZ (LOW/HIGH)^(-filter/100) below 0 and
//      TONE_FILTER_LOW_HZ (HIGH/LOW)^(filter/100) above it (1e-12); at the corner the measured
//      magnitude is the resonance, for a low-pass and a high-pass at four resonances wherever the
//      corner sits below 0.45 of the rate (1e-3 dB); the low-pass is unity at DC and the high-pass at
//      Nyquist (1e-4 dB);
//   D  the face's magnitude word, what a host draws, is the measured response of tilt and filter
//      together on a log grid (1e-3 dB above -60 dB);
//   E  hostile and out-of-travel words land on the declared travels' ends and defaults, the ledger
//      is clean, nothing allocates;
//   F  a non-finite pivot or resonance reads as its declared default, 1000 Hz and 0.71, and a
//      non-finite span or filter as flat and open;
//   G  one block or random blocks of 1 to 4096 frames are the same bits; identical legs stay identical;
//   H  a jump of the filter across 0 (low-pass to high-pass and back) starts the new side from silence:
//      after the jump the output is the new side run from silence on the same samples, bit for bit.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_tone_instance.h>

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

/* The declarations, stated here so a moved one turns its arm red (tone_instance-perturb.sh). */
#define LOW_HZ 20.0
#define HIGH_HZ 20000.0
#define PIVOT_DEFAULT 1000.0f
#define RES_DEFAULT 0.71f
#define TILT_MAX 6.0f
#define PIVOT_MIN 200.0f
#define PIVOT_MAX 5000.0f
#define RES_MIN 0.5f
#define RES_MAX 4.0f

#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];

static OmxToneInstance g_t;

static void setup(float tilt, float pivot, float filter, float res) {
  omx_tone_instance_init(&g_t, g_sr);
  omx_tone_instance_resolve(&g_t, 0, tilt, pivot, filter, res);
}

static void run(OmxToneInstance *t, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) omx_tone_instance_run(t, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
}

/* The face's impulse response on the left leg, `len` samples, into g_ol. */
static void impulse(uint32_t len) {
  memset(g_l, 0, len * sizeof(float));
  memset(g_r, 0, len * sizeof(float));
  g_l[0] = g_r[0] = 1.0f;
  run(&g_t, g_l, g_r, g_ol, g_or, len);
}

/* |DTFT| of g_ol[0..len) at f, by a rotating phasor in double. */
static double resp(uint32_t len, double f) {
  const double w = 2.0 * M_PI * f / g_sr, cw = cos(w), sw = sin(w);
  double re = 0.0, im = 0.0, pr = 1.0, pi = 0.0;
  for (uint32_t i = 0; i < len; i++) {
    re += g_ol[i] * pr;
    im -= g_ol[i] * pi;
    const double nr = pr * cw - pi * sw;
    pi = pr * sw + pi * cw;
    pr = nr;
    if ((i & 4095u) == 4095u) {
      const double k = 1.0 / sqrt(pr * pr + pi * pi);
      pr *= k, pi *= k;
    }
  }
  return sqrt(re * re + im * im);
}

static double db(double x) { return 20.0 * log10(x); }

/* ---- A: flat and open ------------------------------------------------------------------------ */
static void arm_identity(void) {
  g_arm = "A";
  const uint32_t n = (uint32_t)(g_sr * 0.2f);
  stimulus(g_l, g_r, n, 0x5eedu, 3000u);
  static const float PIVOTS[] = {200.0f, 1000.0f, 5000.0f};
  static const float RES[] = {0.5f, 0.71f, 4.0f};
  int same = 1;
  for (int p = 0; p < 3; p++)
    for (int q = 0; q < 3; q++) {
      setup(0.0f, PIVOTS[p], 0.0f, RES[q]);
      run(&g_t, g_l, g_r, g_ol, g_or, n);
      same &= memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0;
    }
  ok(same, "a span of 0 and a filter of 0 are the input, bit for bit, at any pivot and resonance", same, 1);
  omx_tone_instance_init(&g_t, g_sr);
  run(&g_t, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0,
     "a fresh instance is the input, bit for bit", 0, 0);
  omx_tone_instance_init(&g_t, g_sr);
  omx_tone_instance_resolve(&g_t, 1, 6.0f, 300.0f, -70.0f, 3.0f);
  run(&g_t, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0,
     "a bypassed instance is the input, bit for bit", 0, 0);
  /* Back to rest after a tilt and a filter: the input again, the sections not run. */
  setup(-4.0f, 700.0f, 40.0f, 2.0f);
  run(&g_t, g_l, g_r, g_ol, g_or, n);
  omx_tone_instance_resolve(&g_t, 0, 0.0f, 700.0f, 0.0f, 2.0f);
  run(&g_t, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0,
     "back at rest the face is the input again, bit for bit", 0, 0);
  expect_clean();
}

/* ---- B: the tilt's span ---------------------------------------------------------------------- */
static void arm_tilt(void) {
  g_arm = "B";
  static const float SPANS[] = {-6.0f, -2.5f, 3.0f, 6.0f};
  static const float PIVOTS[] = {200.0f, 1000.0f, 5000.0f};
  const uint32_t len = (uint32_t)g_sr;
  double worst = 0.0;
  for (int si = 0; si < 4; si++)
    for (int pi = 0; pi < 3; pi++) {
      const double span = SPANS[si], piv = PIVOTS[pi];
      const double a = pow(10.0, span / 40.0);
      setup(SPANS[si], PIVOTS[pi], 0.0f, RES_DEFAULT);
      ok(fabs(g_t.tilt.a / a - 1.0) < 1e-12, "a = 10^(tiltDb/40)", g_t.tilt.a, a);
      ok(fabs(db(g_t.tilt.a) - 0.5 * span) < 1e-9, "the pair is +-tiltDb/2", db(g_t.tilt.a), 0.5 * span);
      impulse(len);
      const double dc = resp(len, 0.0), ny = resp(len, 0.5 * g_sr);
      ok(fabs(dc * a - 1.0) < 1e-6, "1/a at DC, -tiltDb/2", db(dc), -0.5 * span);
      ok(fabs(ny / a - 1.0) < 1e-6, "a at Nyquist, +tiltDb/2", db(ny), 0.5 * span);
      const double at = db(resp(len, piv));
      worst = fmax(worst, fabs(at));
      ok(fabs(at) < 1e-4, "0 dB at the pivot (1e-4 dB)", at, 0.0);
      const double fs[2] = {0.25 * piv, 3.0 * piv};
      for (int k = 0; k < 2; k++) {
        const double o = tan(M_PI * fs[k] / g_sr) / tan(M_PI * piv / g_sr);
        const double closed = sqrt((a * a * o * o + 1.0) / (o * o + a * a));
        const double got = resp(len, fs[k]);
        worst = fmax(worst, fabs(db(got / closed)));
        ok(fabs(db(got / closed)) < 1e-4, "the prototype's closed form at the prewarped frequency (1e-4 dB)", db(got), db(closed));
      }
      const double up = omx_tilt_magnitude(a, piv, piv * pow(2.0, 1.0 / 1200.0), 1e12);
      const double dn = omx_tilt_magnitude(a, piv, piv * pow(2.0, -1.0 / 1200.0), 1e12);
      const double slope = db(up / dn) * 600.0, want = 20.0 * log10(2.0) * (a * a - 1.0) / (a * a + 1.0);
      ok(fabs(slope - want) < 1e-4, "the slope at the pivot is 20 log10(2) (a^2 - 1)/(a^2 + 1) dB per octave", slope, want);
      if (span == 6.0) ok(fabs(slope - 2.0) < 0.01, "+6 dB is about 2 dB per octave at the pivot", slope, 2.0);
    }
  /* One section: the face's left leg is omx_tilt run by hand, bit for bit. */
  const uint32_t n = (uint32_t)(g_sr * 0.1f);
  stimulus(g_l, g_r, n, 0xb0b0u, 1500u);
  setup(4.5f, 2500.0f, 0.0f, RES_DEFAULT);
  run(&g_t, g_l, g_r, g_ol, g_or, n);
  struct omx_tilt t;
  omx_tilt_design(&t, pow(10.0, 4.5 / 40.0), 2500.0, g_sr);
  double s[4] = {0, 0, 0, 0};
  int same = 1;
  for (uint32_t i = 0; i < n; i++) same &= omx_tilt_tick(&t, g_l[i], s) == g_ol[i];
  ok(same, "the tilt is one section, omx_tilt by hand, bit for bit", same, 1);
  printf("  tone @ %.0f Hz: tilt, worst against the closed form %.2e dB\n", (double)g_sr, worst);
  expect_clean();
}

/* ---- C: the DJ filter ------------------------------------------------------------------------ */
static void arm_filter(void) {
  g_arm = "C";
  static const float FILTERS[] = {-100.0f, -80.0f, -55.0f, -20.0f, -1.0f, 1.0f, 20.0f, 55.0f, 80.0f, 100.0f};
  static const float RES[] = {0.5f, 0.71f, 2.0f, 4.0f};
  const uint32_t len = 2u * (uint32_t)g_sr;
  ok(fabs(omx_tone_filter_hz(-100.0) / LOW_HZ - 1.0) < 1e-12 && fabs(omx_tone_filter_hz(100.0) / HIGH_HZ - 1.0) < 1e-12 &&
         omx_tone_filter_hz(0.0) == 0.0,
     "the ends: TONE_FILTER_LOW_HZ at -100, TONE_FILTER_HIGH_HZ at +100 (1e-12), open at 0", omx_tone_filter_hz(-100.0),
     LOW_HZ);
  double worst = 0.0;
  for (int fi = 0; fi < 10; fi++) {
    const double f = FILTERS[fi], u = fabs(f) / 100.0;
    const double want = f < 0.0 ? HIGH_HZ * pow(LOW_HZ / HIGH_HZ, u) : LOW_HZ * pow(HIGH_HZ / LOW_HZ, u);
    const double fc = omx_tone_filter_hz(f);
    ok(fabs(fc / want - 1.0) < 1e-12, "the corner on its log map", fc, want);
    if (!(fc < 0.45 * g_sr)) continue;
    for (int qi = 0; qi < 4; qi++) {
      setup(0.0f, PIVOT_DEFAULT, FILTERS[fi], RES[qi]);
      impulse(len);
      const double at = db(resp(len, fc)), q = db(RES[qi]);
      worst = fmax(worst, fabs(at - q));
      ok(fabs(at - q) < 1e-3, f < 0.0 ? "the low-pass's magnitude at its corner is the resonance (1e-3 dB)"
                                      : "the high-pass's magnitude at its corner is the resonance (1e-3 dB)",
         at, q);
      const double pass = f < 0.0 ? db(resp(len, 0.0)) : db(resp(len, 0.5 * g_sr));
      ok(fabs(pass) < 1e-4, f < 0.0 ? "the low-pass is unity at DC" : "the high-pass is unity at Nyquist", pass, 0.0);
    }
  }
  printf("  tone @ %.0f Hz: filter, worst corner against the resonance %.2e dB\n", (double)g_sr, worst);
  expect_clean();
}

/* ---- D: the magnitude word ------------------------------------------------------------------- */
static void arm_magnitude(void) {
  g_arm = "D";
  static const float SET[][4] = {{6.0f, 1000.0f, -30.0f, 0.71f}, {-6.0f, 300.0f, 35.0f, 2.0f}, {2.5f, 4000.0f, -70.0f, 4.0f},
                                 {-1.5f, 200.0f, 10.0f, 0.5f}};
  const uint32_t len = 2u * (uint32_t)g_sr;
  double worst = 0.0;
  for (int k = 0; k < 4; k++) {
    setup(SET[k][0], SET[k][1], SET[k][2], SET[k][3]);
    impulse(len);
    for (double f = 30.0; f < 0.45 * g_sr; f *= 1.37) {
      const double want = omx_tone_instance_magnitude(&g_t, f);
      if (db(want) < -60.0) continue;
      const double e = fabs(db(resp(len, f) / want));
      worst = fmax(worst, e);
      ok(e < 1e-3, "the magnitude word is the measured response (1e-3 dB)", e, 1e-3);
    }
  }
  printf("  tone @ %.0f Hz: magnitude word, worst %.2e dB\n", (double)g_sr, worst);
  expect_clean();
}

/* ---- E, F: hostile words and the defaults ---------------------------------------------------- */
static int same_as(float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3, uint32_t n) {
  setup(a0, a1, a2, a3);
  run(&g_t, g_l, g_r, g_ol, g_or, n);
  setup(b0, b1, b2, b3);
  run(&g_t, g_l, g_r, g_xl, g_xr, n);
  return memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0;
}

static void arm_hostile(void) {
  g_arm = "E";
  const uint32_t n = (uint32_t)(g_sr * 0.05f);
  stimulus(g_l, g_r, n, 0xfeedu, 1000u);
  g_counting = 1;
  g_allocs = 0;
  ok(same_as(1e9f, 1000.0f, -30.0f, 1.0f, TILT_MAX, 1000.0f, -30.0f, 1.0f, n), "a span past the travel is its top, +6 dB", 0, 0);
  ok(same_as(-1e9f, 1000.0f, -30.0f, 1.0f, -TILT_MAX, 1000.0f, -30.0f, 1.0f, n), "a span under the travel is its foot, -6 dB", 0, 0);
  ok(same_as(3.0f, 1e9f, 0.0f, 1.0f, 3.0f, PIVOT_MAX, 0.0f, 1.0f, n), "a pivot past the travel is 5000 Hz", 0, 0);
  ok(same_as(3.0f, -1.0f, 0.0f, 1.0f, 3.0f, PIVOT_MIN, 0.0f, 1.0f, n), "a pivot under the travel is 200 Hz", 0, 0);
  ok(same_as(0.0f, 1000.0f, 1e9f, 1.0f, 0.0f, 1000.0f, 100.0f, 1.0f, n), "a filter past the travel is +100", 0, 0);
  ok(same_as(0.0f, 1000.0f, -1e9f, 1.0f, 0.0f, 1000.0f, -100.0f, 1.0f, n), "a filter under the travel is -100", 0, 0);
  ok(same_as(0.0f, 1000.0f, 50.0f, 1e9f, 0.0f, 1000.0f, 50.0f, RES_MAX, n), "a resonance past the travel is 4", 0, 0);
  ok(same_as(0.0f, 1000.0f, 50.0f, -1.0f, 0.0f, 1000.0f, 50.0f, RES_MIN, n), "a resonance under the travel is 0.5", 0, 0);
  ok(same_as(INFINITY, -INFINITY, NAN, INFINITY, 0.0f, PIVOT_DEFAULT, 0.0f, RES_DEFAULT, n), "non-finite words are the defaults", 0, 0);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0.0);
  ok(omx_tone_instance_latency(&g_t) == 0u, "no latency", omx_tone_instance_latency(&g_t), 0.0);
  OmxToneInstance bad;
  ok(omx_tone_instance_init(&bad, 22050.0f) == 0 && omx_tone_instance_init(NULL, 48000.0f) == 0, "a rate outside the oracle set is refused", 0, 0);
  run(&bad, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "a refused instance is the input", 0, 0);
  expect_clean();
}

static void arm_defaults(void) {
  g_arm = "F";
  const uint32_t n = (uint32_t)(g_sr * 0.05f);
  stimulus(g_l, g_r, n, 0xdefu, 1000u);
  ok(same_as(3.0f, NAN, 0.0f, 1.0f, 3.0f, PIVOT_DEFAULT, 0.0f, 1.0f, n), "a non-finite pivot is 1000 Hz", 0, 0);
  ok(same_as(0.0f, 1000.0f, -40.0f, NAN, 0.0f, 1000.0f, -40.0f, RES_DEFAULT, n), "a non-finite resonance is 0.71", 0, 0);
  setup(NAN, 1000.0f, NAN, 1.0f);
  run(&g_t, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "a non-finite span and filter are flat and open", 0, 0);
  /* The come-up: a fresh instance taking a span and a filter keeps the default pivot and resonance. */
  omx_tone_instance_init(&g_t, g_sr);
  ok(g_t.pivot_hz == PIVOT_DEFAULT && g_t.q == RES_DEFAULT && g_t.span_db == 0.0f && g_t.filter == 0.0f,
     "the instance comes up at 0 dB, 1000 Hz, open, 0.71", g_t.pivot_hz, PIVOT_DEFAULT);
  expect_clean();
}

/* ---- G: blocks and legs ---------------------------------------------------------------------- */
static void arm_blocks(void) {
  g_arm = "G";
  const uint32_t n = (uint32_t)(g_sr * 0.25f);
  stimulus(g_l, g_r, n, 0xb10cu, 2500u);
  setup(-3.5f, 800.0f, -45.0f, 3.0f);
  omx_tone_instance_run(&g_t, g_l, g_r, g_ol, g_or, n);
  setup(-3.5f, 800.0f, -45.0f, 3.0f);
  uint32_t lcg = 77u;
  for (uint32_t o = 0; o < n;) {
    lcg = lcg * 1664525u + 1013904223u;
    uint32_t b = 1u + (lcg >> 8) % 4096u;
    if (b > n - o) b = n - o;
    omx_tone_instance_run(&g_t, g_l + o, g_r + o, g_xl + o, g_xr + o, b);
    o += b;
  }
  ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0,
     "random blocks of 1 to 4096 frames are one block, bit for bit", 0, 0);
  setup(5.0f, 1500.0f, 30.0f, 1.2f);
  run(&g_t, g_l, g_l, g_ol, g_or, n);
  ok(memcmp(g_ol, g_or, n * sizeof(float)) == 0, "identical legs stay identical", 0, 0);
  expect_clean();
}

/* ---- H: a change of side starts the filter from silence -------------------------------------- */
/* Noise through one side for 0.25 s, then the word jumps to the other side. The step at the jump is
 * the largest distance, over the 0.1 s after it, from the new side run from silence on the same
 * samples, against that run's own peak: 0 when the history is cleared. The low-pass's history read by
 * a 21 Hz high-pass at Q 4 rings at 21 Hz for tens of milliseconds. */
static OmxToneInstance g_ref;
static void jump(float from, float to, float res) {
  const uint32_t pre = (uint32_t)(g_sr * 0.25f), post = (uint32_t)(g_sr * 0.1f);
  stimulus(g_l, g_r, pre + post, 0x5e1du, 0u);
  setup(0.0f, 1000.0f, from, res);
  run(&g_t, g_l, g_r, g_ol, g_or, pre);
  omx_tone_instance_resolve(&g_t, 0, 0.0f, 1000.0f, to, res);
  run(&g_t, g_l + pre, g_r + pre, g_ol, g_or, post);
  omx_tone_instance_init(&g_ref, g_sr);
  omx_tone_instance_resolve(&g_ref, 0, 0.0f, 1000.0f, to, res);
  run(&g_ref, g_l + pre, g_r + pre, g_xl, g_xr, post);
  double step = 0.0, peak = 0.0;
  for (uint32_t i = 0; i < post; i++) { /* comparisons, not fmax: BUILDING.md, GCC 12.2 on arm64 */
    const double dl = fabs((double)g_ol[i] - g_xl[i]), dr = fabs((double)g_or[i] - g_xr[i]);
    const double pl = fabs((double)g_xl[i]), pr = fabs((double)g_xr[i]);
    if (!(dl <= step)) step = dl;
    if (!(dr <= step)) step = dr;
    if (!(pl <= peak)) peak = pl;
    if (!(pr <= peak)) peak = pr;
  }
  if (step > 0.0) printf("  tone @ %.0f Hz: filter %+.0f to %+.0f at Q %.2f, a step %.1f dB against the peak\n", (double)g_sr,
                         (double)from, (double)to, (double)res, db(step / peak));
  ok(step == 0.0, "a jump across 0 is the new side from silence: no step at the jump", step / peak, 0.0);
}
static void arm_side(void) {
  g_arm = "H";
  jump(-1.0f, 1.0f, RES_MAX);
  jump(-1.0f, 1.0f, RES_DEFAULT);
  jump(1.0f, -1.0f, RES_MAX);
  jump(-60.0f, 40.0f, 2.0f);
  expect_clean();
}

int main(void) {
  (void)sine;
  (void)rms;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (unsigned ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_identity();
    arm_tilt();
    arm_filter();
    arm_magnitude();
    arm_hostile();
    arm_defaults();
    arm_blocks();
    arm_side();
  }
  return instance_oracle_end("tone_instance");
}
