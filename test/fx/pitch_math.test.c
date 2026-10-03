// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The dual-tap pitch shifter (omx_pitch.h) against the closed forms of its own splice — the
 * engine's spec docs/design/specs/2026-09-22-native-pitch-shift.md §3, §7 and §11, at EVERY
 * declared rate (OMX_DECLARED_RATES, the generated header — never this file's own list). Every
 * number the stage declares is read from OMX_PITCH_*; the oracle's formulas are the spec's, in
 * double, and never call the kernel's own delay or weight words. Contracts are compiled in and
 * every arm drains the ledger empty.
 *
 *   A  — on the grid (f0·W_s/2 whole): the output frequency is r·f0, two meters (upward zero
 *        crossings with linear placement; the peak of a Hann-windowed DTFT, zoomed), each against
 *        the closed form, 0.5 cent. A2 — the same run's complex envelope is flat, 0.05 dB.
 *   B  — off the grid: each meter against §3a's splice law f_out, 0.5 cent, over whole ramp
 *        periods. B2 — the envelope's dip against 20·log10|cos(π·wrap)|, 0.1 dB, and a null
 *        below −60 dB at wrap = ½.
 *   C  — rate direction: the pre-filter's coefficients are the tier's low-pass at
 *        min(ceiling, sr/(2r)), and the product's attenuation (a filtered run over an
 *        identity-filter copy) equals that filter's closed form |H(e^{jω})| at the input tone,
 *        ±1 dB. The copy is the positive control: its product must be present.
 *   D  — the delay follows the ramp: impulse centroids and cluster sums at frozen phases, both
 *        directions, against d_min + W·u (or 1 − u) plus the filter's DC group delay, 0.001
 *        sample; the direction hand-over reflects the phase and moves no delay.
 *
 * These are the closed-form arms of the engine's mix_pitch.test.c, moved with the kernel; the
 * bypass, finite/flush and thread arms are test/fx/pitch.test.c.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

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

/* ---- the oracle's own closed forms (spec §1, §3), in double ------------------------------ */

/** The kernel's reach at order 3 (the fdelay spec): the shortest delay a tap reads. */
#define O_DMIN 1.0

static double o_window(double sr) { return (double)OMX_PITCH_WINDOW_MS * 1e-3 * sr; }
static double o_shape(double x) {
  x -= floor(x);
  const double t = 2.0 * x - 1.0;
  return 4.0 * t * (1.0 - fabs(t));
}
static double o_weight(double u, int tap) { return 0.5 * (1.0 + o_shape(u + (tap ? 0.75 : 0.25))); }
static double o_delay(double W, double u, int dir, int tap) {
  double v = u + (tap ? 0.5 : 0.0);
  v -= floor(v);
  return O_DMIN + W * (dir > 0 ? 1.0 - v : v);
}
static double o_wrap(double q) { return q - floor(q + 0.5); }
static double o_splice_law(double f0, double r, double Ws) {
  return r * f0 + (2.0 * (1.0 - r) / Ws) * o_wrap(f0 * Ws / 2.0);
}
static double cents(double a, double b) { return 1200.0 * log2(a / b); }

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

/* ---- the meters ---------------------------------------------------------------------------- */

static double zc_freq(const float *y, size_t n, double sr) {
  double first = -1.0, last = -1.0;
  long count = 0;
  for (size_t i = 0; i + 1 < n; i++) {
    if (y[i] < 0.0f && y[i + 1] >= 0.0f) {
      const double t = (double)i + (double)y[i] / ((double)y[i] - (double)y[i + 1]);
      if (first < 0.0) first = t;
      last = t;
      count++;
    }
  }
  return count < 2 ? 0.0 : (double)(count - 1) * sr / (last - first);
}

static double dtft_mag(const float *y, size_t n, double f, double sr) {
  const double w = 2.0 * M_PI * f / sr;
  double re = 0.0, im = 0.0, cr = 1.0, ci = 0.0;
  const double sr1 = cos(w), si1 = -sin(w);
  for (size_t k = 0; k < n; k++) {
    const double h = 0.5 - 0.5 * cos(2.0 * M_PI * (double)k / (double)(n - 1));
    re += h * (double)y[k] * cr;
    im += h * (double)y[k] * ci;
    const double nr = cr * sr1 - ci * si1;
    ci = cr * si1 + ci * sr1;
    cr = nr;
    if ((k & 4095u) == 4095u) {
      const double g = 1.0 / hypot(cr, ci);
      cr *= g;
      ci *= g;
    }
  }
  return hypot(re, im);
}

/** The Hann DTFT's peak inside [lo, hi], zoomed three times, then parabolic on the last grid. */
static double dtft_peak(const float *y, size_t n, double sr, double lo, double hi) {
  enum { G = 21 };
  double m[G];
  for (int pass = 0; pass < 4; pass++) {
    const double step = (hi - lo) / (G - 1);
    int best = 0;
    for (int g = 0; g < G; g++) {
      m[g] = dtft_mag(y, n, lo + step * g, sr);
      if (m[g] > m[best]) best = g;
    }
    if (pass == 3) {
      if (best == 0 || best == G - 1) return lo + step * best;
      const double a = m[best - 1], b = m[best], c = m[best + 1];
      const double d = 0.5 * (a - c) / (a - 2.0 * b + c);
      return lo + step * (best + d);
    }
    const double centre = lo + step * best;
    lo = centre - step;
    hi = centre + step;
  }
  return 0.0;
}

/** The demodulation length: `mlo..mhi` periods of `f`, the one whose 2ω image leaks least. */
static size_t demod_len(double f, double sr, int mlo, int mhi) {
  size_t best = 0;
  double leak = 1e9;
  for (int m = mlo; m <= mhi; m++) {
    const size_t K = (size_t)lround(m * sr / f);
    const double l = fabs(sin(M_PI * (double)K * 2.0 * f / sr)) / (double)K;
    if (l < leak) {
      leak = l;
      best = K;
    }
  }
  return best;
}

/**
 * The complex envelope at the local frequency `f`: a K-sample moving mean of y·e^{−jωk}. Returns
 * its max and its min, the min taken on the segment between consecutive means (the envelope is a
 * continuous-time fact; a sample grid must not hide a null between two samples).
 */
static void envelope(const float *y, size_t n, double f, double sr, size_t K, double *emax,
                     double *emin) {
  const double w = 2.0 * M_PI * f / sr;
  double *zr = malloc(n * sizeof(double)), *zi = malloc(n * sizeof(double));
  if (zr == NULL || zi == NULL) abort();
  for (size_t k = 0; k < n; k++) {
    zr[k] = (double)y[k] * cos(w * (double)k);
    zi[k] = -(double)y[k] * sin(w * (double)k);
  }
  double sr_ = 0.0, si_ = 0.0, pr = 0.0, pi_ = 0.0;
  *emax = 0.0;
  *emin = 1e30;
  for (size_t k = 0; k < n; k++) {
    sr_ += zr[k];
    si_ += zi[k];
    if (k >= K) {
      sr_ -= zr[k - K];
      si_ -= zi[k - K];
    }
    if (k + 1 < K) continue;
    const double ar = sr_ / (double)K, ai = si_ / (double)K;
    const double e = hypot(ar, ai);
    if (e > *emax) *emax = e;
    if (k + 1 > K) {
      const double dr = ar - pr, di = ai - pi_;
      const double dd = dr * dr + di * di;
      double t = dd > 0.0 ? -(pr * dr + pi_ * di) / dd : 0.0;
      t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
      const double m = hypot(pr + t * dr, pi_ + t * di);
      if (m < *emin) *emin = m;
    }
    pr = ar;
    pi_ = ai;
  }
  free(zr);
  free(zi);
}

/* ---- the arms ------------------------------------------------------------------------------ */

static const double GRID[6] = {0.25, 0.5, 0.66741992708501718 /* 2^(−7/12) */,
                               1.02930223664349207 /* 2^(+50/1200) */,
                               1.49830707687668152 /* 2^(+7/12) */, 2.0};

/** Run a tone through a wet-only stage; returns the output (left leg) past a warm-up of 2W. */
static float *wet_run(double sr, double r, double f0, size_t span, size_t *warm_out) {
  const size_t warm = (size_t)(2.0 * o_window(sr)) + 16u;
  const size_t n = warm + span;
  float *l = malloc(n * sizeof(float)), *rr = malloc(n * sizeof(float));
  if (l == NULL || rr == NULL) abort();
  tone(l, n, f0, sr, 0.5);
  memcpy(rr, l, n * sizeof(float));
  struct run R;
  setup(&R, sr, r, 1.0);
  process(&R, l, rr, n);
  ok(memcmp(l, rr, n * sizeof(float)) == 0, "both legs run one ramp phase", sr, r, 0);
  free(rr);
  *warm_out = warm;
  return l;
}

static void arm_a_on_the_grid(void) {
  const double f0 = 1000.0; /* f0·W_s/2 = 20 at 40 ms: on the grid */
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    for (int g = 0; g < 6; g++) {
      const double r = GRID[g], want = r * f0;
      size_t warm = 0;
      const size_t span = (size_t)(4.0 * sr);
      float *y = wet_run(sr, r, f0, span, &warm);
      const double zc = zc_freq(y + warm, span, sr);
      ok(fabs(cents(zc, want)) < 0.5, "A zero-crossing meter reads r·f0 (cents)", sr,
         cents(zc, want), 0.5);
      const double pk = dtft_peak(y + warm, (size_t)sr, sr, want - 4.0, want + 4.0);
      ok(fabs(cents(pk, want)) < 0.5, "A DTFT peak reads r·f0 (cents)", sr, cents(pk, want), 0.5);
      double emax = 0.0, emin = 0.0;
      envelope(y + warm, span, want, sr, demod_len(want, sr, 8, 64), &emax, &emin);
      const double flat = 20.0 * log10(emax / emin);
      ok(flat < 0.05, "A2 the envelope is flat on the grid (dB)", sr, flat, 0.05);
      free(y);
    }
  }
  drain_violations("A contracts");
}

static void arm_b_off_the_grid(void) {
  static const double FR[5] = {0.25, 0.5, 0.66741992708501718, 1.49830707687668152, 2.0};
  static const double F0[2] = {1010.0, 1012.5};
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri], W = o_window(sr), Ws = W / sr;
    for (int g = 0; g < 5; g++) {
      const double r = FR[g];
      const double T = W / fabs(1.0 - r); /* one ramp period, samples: two hand-overs */
      const size_t span = (size_t)lround(ceil(4.0 * sr / T) * T);
      for (int k = 0; k < 2; k++) {
        const double want = o_splice_law(F0[k], r, Ws);
        size_t warm = 0;
        float *y = wet_run(sr, r, F0[k], span, &warm);
        const double zc = zc_freq(y + warm, span, sr);
        ok(fabs(cents(zc, want)) < 0.5, "B zero-crossing meter reads the splice law (cents)", sr,
           cents(zc, want), 0.5);
        const double half = fabs(1.0 - r) / Ws; /* half the hand-over rate */
        const double pk = dtft_peak(y + warm, (size_t)(2.0 * sr), sr, want - half, want + half);
        ok(fabs(cents(pk, want)) < 0.5, "B DTFT peak reads the splice law (cents)", sr,
           cents(pk, want), 0.5);
        free(y);
      }
    }
    /* B2: the dip, on ratios whose ramp is slow against the demodulation length. */
    static const double RR[3] = {0.66741992708501718, 1.02930223664349207, 1.49830707687668152};
    static const double FD[3] = {1010.0, 1012.5, 1025.0};
    for (int g = 0; g < 3; g++) {
      for (int k = 0; k < 3; k++) {
        const double r = RR[g], wrap = o_wrap(FD[k] * Ws / 2.0);
        size_t warm = 0;
        const size_t span = (size_t)(4.0 * sr);
        float *y = wet_run(sr, r, FD[k], span, &warm);
        double emax = 0.0, emin = 0.0;
        envelope(y + warm, span, r * FD[k], sr, demod_len(r * FD[k], sr, 4, 8), &emax, &emin);
        const double dip = 20.0 * log10(emin / emax);
        if (fabs(fabs(wrap) - 0.5) < 1e-12) {
          ok(dip < -60.0, "B2 a half-integer f0·W_s/2 nulls the envelope (dB)", sr, dip, -60.0);
        } else {
          const double law = 20.0 * log10(fabs(cos(M_PI * wrap)));
          ok(fabs(dip - law) < 0.1, "B2 the dip is 20·log10|cos(π·wrap)| (dB error)", sr,
             dip - law, 0.1);
        }
        free(y);
      }
    }
  }
  drain_violations("B contracts");
}

/** |H(e^{jω})| of a biquad {b0, b1, b2, a1, a2}. */
static double biquad_mag(const float c[5], double f, double sr) {
  const double w = 2.0 * M_PI * f / sr;
  const double nr = c[0] + c[1] * cos(w) + c[2] * cos(2 * w), ni = -c[1] * sin(w) - c[2] * sin(2 * w);
  const double dr = 1.0 + c[3] * cos(w) + c[4] * cos(2 * w), di = -c[3] * sin(w) - c[4] * sin(2 * w);
  return hypot(nr, ni) / hypot(dr, di);
}

static double fold(double f, double sr) {
  f = fmod(f, sr);
  return f > sr / 2.0 ? sr - f : f;
}

static void arm_c_rate_direction(void) {
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    const double down = fmin(60000.0, floor(0.45 * sr / 50.0) * 50.0);
    const double RS[2] = {2.0, 0.25}, FS[2] = {15000.0, down};
    for (int d = 0; d < 2; d++) {
      const double r = RS[d], f_in = FS[d], f_p = fold(r * f_in, sr);
      const size_t span = (size_t)(sr / 2.0), warm = (size_t)(2.0 * o_window(sr)) + 16u,
                   n = warm + span;
      float *x = malloc(n * sizeof(float)), *a = malloc(n * sizeof(float)),
            *b = malloc(n * sizeof(float)), *junk = malloc(n * sizeof(float));
      if (x == NULL || a == NULL || b == NULL || junk == NULL) abort();
      tone(x, n, f_in, sr, 0.5);
      /* The corner law, designed by the oracle from the declaration, bit for bit. */
      struct run R;
      setup(&R, sr, r, 1.0);
      float want[5];
      omx_eq_design_f(OMX_EQ_LOWPASS, fmin((double)OMX_PITCH_PREFILTER_CEILING_HZ, sr / (2.0 * r)),
                      (double)OMX_PITCH_PREFILTER_Q, 0.0, sr, want);
      ok(memcmp(want, R.p.lp, sizeof want) == 0, "C the pre-filter is the low-pass at min(ceiling, sr/(2r))",
         sr, r, 0);
      memcpy(a, x, n * sizeof(float));
      memcpy(junk, x, n * sizeof(float));
      process(&R, a, junk, n);
      /* The positive control: the same stage with its filter forced to identity (order 0). */
      struct run O;
      setup(&O, sr, r, 1.0);
      static const float IDENT[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      memcpy(O.p.lp, IDENT, sizeof IDENT);
      memcpy(b, x, n * sizeof(float));
      memcpy(junk, x, n * sizeof(float));
      process(&O, b, junk, n);
      /* Each level at its own peak: the product sits within the ramp's 0.40 cent of f_p. */
      const double pa = dtft_peak(a + warm, span, sr, f_p - 20.0, f_p + 20.0);
      const double pb = dtft_peak(b + warm, span, sr, f_p - 20.0, f_p + 20.0);
      const double la = dtft_mag(a + warm, span, pa, sr), lb = dtft_mag(b + warm, span, pb, sr);
      const double ref = dtft_mag(x + warm, span, f_in, sr);
      const double open = 20.0 * log10(lb / ref);
      ok(open > -20.0, d == 0 ? "C positive control (up): the unfiltered product is present (dB re input)" : "C positive control (down): the unfiltered product is present (dB re input)", sr, open,
         -20.0);
      const double meas = 20.0 * log10(la / lb), law = 20.0 * log10(biquad_mag(R.p.lp, f_in, sr));
      ok(fabs(meas - law) < 1.0, "C the product's attenuation is the filter's |H| at the tone (dB error)",
         sr, meas - law, 1.0);
      free(x);
      free(a);
      free(b);
      free(junk);
    }
  }
  drain_violations("C contracts");
}

/** The biquad's DC group delay, samples: Σk·b_k/Σb_k − Σk·a_k/Σa_k (a0 = 1). */
static double biquad_dc_delay(const float c[5]) {
  const double sb = c[0] + c[1] + c[2], sa = 1.0 + c[3] + c[4];
  return (c[1] + 2.0 * c[2]) / sb - (c[3] + 2.0 * c[4]) / sa;
}

static void arm_d_delay_follows(void) {
  static const double U[6] = {0.1, 0.3, 0.45, 0.6, 0.8, 0.95};
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri], W = o_window(sr);
    const size_t n0 = 64u, n = n0 + (size_t)(W + 64.0) + 256u;
    float *l = calloc(n, sizeof(float)), *rr = calloc(n, sizeof(float));
    if (l == NULL || rr == NULL) abort();
    for (int dir = -1; dir <= 1; dir += 2) {
      for (int k = 0; k < 6; k++) {
        struct run R;
        setup(&R, sr, 1.0, 1.0); /* zero shift: the ramp is frozen where it stands (R10) */
        ok(R.p.step == 0u && R.p.dir == 0, "D zero shift freezes the ramp", sr, R.p.step, 0);
        R.s.ramp = (uint32_t)llround(ldexp(U[k], 32));
        R.s.dir = dir;
        memset(l, 0, n * sizeof(float));
        l[n0] = 1.0f;
        memcpy(rr, l, n * sizeof(float));
        process(&R, l, rr, n);
        const double H0 = (R.p.lp[0] + R.p.lp[1] + R.p.lp[2]) / (1.0 + R.p.lp[3] + R.p.lp[4]);
        const double tau = biquad_dc_delay(R.p.lp);
        for (int tap = 0; tap < 2; tap++) {
          const double u = ldexp((double)R.s.ramp, -32);
          const double dw = o_delay(W, u, dir, tap), ww = o_weight(u, tap);
          const double centre = (double)n0 + dw + tau;
          const size_t lo = (size_t)fmax(0.0, centre - W / 4.0), hi = (size_t)(centre + W / 4.0);
          double m0 = 0.0, m1 = 0.0;
          for (size_t i = lo; i <= hi && i < n; i++) {
            m0 += l[i];
            m1 += (double)i * l[i];
          }
          ok(fabs(m0 - ww * H0) < 1e-5, "D a tap's cluster carries its window's weight", sr, m0 - ww * H0,
             1e-5);
          if (ww * H0 > 1e-3) {
            const double c = m1 / m0 - (double)n0 - tau;
            ok(fabs(c - dw) < 1e-3, "D a tap's centroid is d_min + W·u (samples)", sr, c - dw, 1e-3);
          }
        }
      }
    }
    /* The hand-over: going down at phase u, a shift upward reflects the phase and keeps both
     * taps' delays and weights. */
    struct run R;
    setup(&R, sr, 0.5, 1.0);
    R.s.ramp = (uint32_t)llround(ldexp(0.3, 32));
    R.s.dir = -1;
    const double u1 = ldexp((double)R.s.ramp, -32);
    const double dA = o_delay(W, u1, -1, 0), dB = o_delay(W, u1, -1, 1);
    omx_pitch_resolve_ratio(&R.p, 1, 1.49830707687668152f, 1.0f, (float)sr);
    float one_l = 0.0f, one_r = 0.0f;
    omx_pitch_process(&one_l, &one_r, 1u, &R.p, &R.s);
    const double u2 = ldexp((double)(uint32_t)(R.s.ramp - R.p.step), -32); /* the phase it read at */
    ok(R.s.dir == 1, "D the hand-over takes the new direction", sr, R.s.dir, 1);
    ok(fabs(o_delay(W, u2, 1, 0) - dA) < 1e-3 && fabs(o_delay(W, u2, 1, 1) - dB) < 1e-3,
       "D the hand-over moves no delay (samples)", sr, fabs(o_delay(W, u2, 1, 0) - dA), 1e-3);
    free(l);
    free(rr);
  }
  drain_violations("D contracts");
}

int main(void) {
  float top = 0.0f;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) top = fmaxf(top, OMX_DECLARED_RATES[ri]);
  g_ring_cap = omx_pitch_cap_for(top);
  g_ring = calloc(2u * g_ring_cap, sizeof(float));
  if (g_ring == NULL) return 2;
  omx_fx_require_rate_floor();
  arm_a_on_the_grid();
  arm_b_off_the_grid();
  arm_c_rate_direction();
  arm_d_delay_follows();
  free(g_ring);
  printf("fx/pitch_math: %d checks, %d failed (%d rates)\n", g_checks, g_failed, (int)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
