// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the low-pass near Nyquist (FreeMixer/omx-dsp#105) ----------------------------------------
 *
 * omx_eq_design(OMX_EQ_LOWPASS, …) with matched poles lifted its passband as the corner neared
 * Nyquist: +0.54 dB at 0.8·Nyquist, +19 dB at the 0.999 hold. Every arm is closed form, read off
 * the coefficients with omx_biquad_magnitude_db_d(); nothing is rendered. Run at the nine RME
 * rates for the two Butterworth sections that have no peak of their own (Q = 1/√2, the 12 dB
 * slope; Q = 0.5412, the low-Q half of the 24 dB slope).
 *
 *   peak    no point of the response is above +0.1 dB, for every corner from 0.5·Nyquist to the
 *           0.999 hold and for corners past Nyquist;
 *   corner  the response at the corner is the prototype's |H| = Q (−3.01 dB for 1/√2), 0.01 dB;
 *   below   up to the hand-over start the section is the matched design, coefficient for
 *           coefficient (lpn_matched() is that design as it stood before #105);
 *   low     at or below a quarter of the corner, within LPN_LOW_TOL_DB of the matched design
 *           wherever the matched design itself met the peak bound;
 *   sweep   a corner swept from 0.55 to 0.95·Nyquist in 0.0004·Nyquist steps moves no grid frequency
 *           under the corner by more in one step than LPN_STEP_RATIO times the matched design's
 *           own worst step for the same corner step below the hand-over.
 *
 * And over the declared space (omx-dsp#116), at every corner of the declared EQ grid
 * (OMX_EQ_FREQ_RANGE_MIN·1.5^k up to OMX_EQ_FREQ_RANGE_MAX, and the maximum itself) above half
 * Nyquist and under the 0.999 hold, at every Q from OMX_EQ_Q_RANGE_MIN to OMX_EQ_Q_RANGE_MAX in
 * 1 % steps:
 *
 *   cookbook  the low-pass is no further from its analogue prototype than the RBJ cookbook
 *             section, 20 Hz to 20 kHz (Nyquist where lower) on a 5 Hz grid where the prototype is
 *             above −40 dB;
 *   null      the numerator has no zero on the unit circle short of Nyquist (a complex pair with
 *             b0 = b2 is one: what the clamped fit gave at Q 3, 19.7 kHz, 44.1 kHz).
 */

#define LPN_PEAK_DB 0.1
#define LPN_CORNER_TOL_DB 0.01
#define LPN_LOW_TOL_DB 0.05
#define LPN_STEP_RATIO 2.5
#define LPN_GRID 1200u
/* Where the hand-over is complete the low-pass IS the cookbook section in exact arithmetic (the
 * bilinear poles under a numerator fitted to DC, the corner and a Nyquist zero), reached by other
 * operations: measured 4.9e-13 dB apart at most. A tie within this is a tie. */
#define LPN_TIE_DB 1e-9

static const float LPN_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;
static const double LPN_Q[2] = {0.70710678118654752, 0.54119610014619698};

/* The matched low-pass as omx_eq_design ran it before #105, the reference the `below` and `low`
 * arms compare against. */
static void lpn_matched(double freq_hz, double q, double sr, double c[5]) {
  const double nyquist = sr * 0.5, hi = nyquist * 0.999;
  const double f0 = freq_hz < 1.0 ? 1.0 : (freq_hz > hi ? hi : freq_hz);
  const double qq = q <= 0.0 ? 1e-3 : q;
  const double w0 = (2.0 * M_PI * f0) / sr;
  double p[3];
  omx_eq_matched_pair(w0, 1.0 / (2.0 * qq), p);
  const double xn = nyquist / f0, un = 1.0 - xn * xn;
  const double tPi = 1.0 / (un * un + (xn / qq) * (xn / qq));
  const double dPi = 1.0 - p[0] + p[1], cw = cos(w0);
  const double dr = 1.0 + p[0] * cw + p[1] * cos(2.0 * w0);
  const double di = -(p[0] * sin(w0) + p[1] * sin(2.0 * w0));
  const double s0 = sin(w0 / 2.0) * sin(w0 / 2.0);
  const OmxMatchedNumerator n =
      omx_matched_fit_numerator(p[2] * p[2], tPi * dPi * dPi, qq * qq * (dr * dr + di * di), s0);
  c[0] = n.b0; c[1] = n.b1; c[2] = n.b2; c[3] = p[0]; c[4] = p[1];
}

/* The highest point of the response over a uniform grid from DC to Nyquist. */
static double lpn_peak_db(const double c[5], double sr) {
  double m = -1e9;
  for (uint32_t i = 1; i <= LPN_GRID; i++) {
    const double v = omx_biquad_magnitude_db_d(c, sr * 0.5 * i / (LPN_GRID + 1.0), sr, -300.0);
    if (v > m) m = v;
  }
  return m;
}

/* The corner grid: 0.5 to 0.95 of Nyquist in hundredths, then to the 0.999 hold in thousandths,
 * then two corners past Nyquist (the hold itself). */
static uint32_t lpn_corners(double r[128]) {
  uint32_t n = 0;
  for (int k = 50; k < 95; k++) r[n++] = k / 100.0;
  for (int k = 950; k <= 999; k += 2) r[n++] = k / 1000.0;
  r[n++] = 0.999;
  r[n++] = 1.2;
  r[n++] = 2.0;
  return n;
}

/* The RBJ cookbook low-pass, transcribed (the design the matched one replaced): the bar. */
static void lpn_cookbook(double f0, double q, double sr, double c[5]) {
  const double w0 = 2.0 * M_PI * f0 / sr, cw = cos(w0), al = sin(w0) / (2.0 * q), a0 = 1.0 + al;
  c[0] = (1.0 - cw) / 2.0 / a0; c[1] = (1.0 - cw) / a0; c[2] = (1.0 - cw) / 2.0 / a0;
  c[3] = -2.0 * cw / a0; c[4] = (1.0 - al) / a0;
}

/* The worst |section − prototype| in dB, 20 Hz to 20 kHz (Nyquist where lower) every 5 Hz, where
 * the prototype 1/(s² + s/Q + 1) is above −40 dB. Prototype transcribed, not the design's. */
static double lpn_worst_vs_prototype(const double c[5], double f0, double q, double sr) {
  const double top = sr * 0.5 < 20000.0 ? sr * 0.5 : 20000.0;
  double worst = 0.0;
  for (double f = 20.0; f <= top; f += 5.0) {
    const double x = f / f0, u = 1.0 - x * x;
    const double proto = -10.0 * log10(u * u + (x / q) * (x / q));
    if (proto < -40.0) continue;
    const double e = fabs(omx_biquad_magnitude_db_d(c, f, sr, -300.0) - proto);
    if (!(e <= worst)) worst = e;
  }
  return worst;
}

static void lpn_declared_space(void) {
  double corners[40];
  uint32_t nc = 0;
  for (double f = OMX_EQ_FREQ_RANGE_MIN; f <= OMX_EQ_FREQ_RANGE_MAX; f *= 1.5) corners[nc++] = f;
  corners[nc++] = OMX_EQ_FREQ_RANGE_MAX;
  for (uint32_t ri = 0; ri < OMX_RME_RATES_COUNT; ri++) {
    const double sr = LPN_RATES[ri], nyq = sr * 0.5;
    uint32_t points = 0, further = 0, nulls = 0;
    double worst_ratio = 0.0, at_f0 = 0.0, at_q = 0.0, excess = 0.0;
    for (uint32_t k = 0; k < nc; k++) {
      const double f0 = corners[k];
      if (f0 <= 0.5 * nyq || f0 >= 0.999 * nyq) continue;
      for (double q = OMX_EQ_Q_RANGE_MIN_DOUBLE; q <= OMX_EQ_Q_RANGE_MAX * 1.000001; q *= 1.01) {
        double c[5], b[5];
        omx_eq_design(OMX_EQ_LOWPASS, f0, q, 0.0, sr, c);
        lpn_cookbook(f0, q, sr, b);
        const double m = lpn_worst_vs_prototype(c, f0, q, sr), cb = lpn_worst_vs_prototype(b, f0, q, sr);
        points++;
        if (!(m <= cb + LPN_TIE_DB)) further++;
        if (m - cb > excess) excess = m - cb;
        if (m / cb > worst_ratio) { worst_ratio = m / cb; at_f0 = f0; at_q = q; }
        if (c[1] * c[1] < 4.0 * c[0] * c[2] && fabs(c[2] - c[0]) <= 1e-9 * fabs(c[0])) nulls++;
      }
    }
    if (points == 0u) continue;
    printf("  lowpass_nyquist declared space %6.0f Hz: %u points, worst against the cookbook %.4f (%.0f Hz, Q %.3f), most over it %.2e dB\n",
           sr, points, worst_ratio, at_f0, at_q, excess);
    ok(further == 0u, "no further from the prototype than the cookbook at any declared top corner and Q", (double)further, 0.0);
    ok(nulls == 0u, "no numerator zero on the unit circle short of Nyquist", (double)nulls, 0.0);
  }
}

static void arm_lowpass_nyquist(void) {
  g_arm = "lowpass_nyquist";
  double r[128];
  const uint32_t nr = lpn_corners(r);
  for (uint32_t ri = 0; ri < OMX_RME_RATES_COUNT; ri++) {
    const double sr = LPN_RATES[ri], nyq = sr * 0.5;
    for (uint32_t qi = 0; qi < 2; qi++) {
      const double q = LPN_Q[qi];
      double worst_peak = -1e9, worst_corner = 0.0, worst_low = 0.0;
      for (uint32_t k = 0; k < nr; k++) {
        double c[5], m[5];
        omx_eq_design(OMX_EQ_LOWPASS, r[k] * nyq, q, 0.0, sr, c);
        lpn_matched(r[k] * nyq, q, sr, m);
        const double pk = lpn_peak_db(c, sr);
        if (pk > worst_peak) worst_peak = pk;
        if (r[k] < 0.999) {
          const double d = fabs(omx_biquad_magnitude_db_d(c, r[k] * nyq, sr, -300.0) - 20.0 * log10(q));
          if (d > worst_corner) worst_corner = d;
        }
        if (lpn_peak_db(m, sr) <= LPN_PEAK_DB) {
          const double fc = (r[k] < 0.999 ? r[k] : 0.999) * nyq;
          for (uint32_t i = 1; i <= 200u; i++) {
            const double f = 0.25 * fc * i / 200.0;
            const double d = fabs(omx_biquad_magnitude_db_d(c, f, sr, -300.0) - omx_biquad_magnitude_db_d(m, f, sr, -300.0));
            if (d > worst_low) worst_low = d;
          }
        }
      }
      printf("  lowpass_nyquist %6.0f Hz Q %.4f: peak %+.4f dB, corner off %.5f dB, low band off %.4f dB\n",
             sr, q, worst_peak, worst_corner, worst_low);
      ok(worst_peak <= LPN_PEAK_DB, "no lift above +0.1 dB from 0.5·Nyquist to past Nyquist", worst_peak, LPN_PEAK_DB);
      ok(worst_corner <= LPN_CORNER_TOL_DB, "the corner reads the prototype's |H| = Q", worst_corner, LPN_CORNER_TOL_DB);
      ok(worst_low <= LPN_LOW_TOL_DB, "a quarter of the corner and below stays the matched design's", worst_low, LPN_LOW_TOL_DB);
      /* below: the matched design, coefficient for coefficient, up to the hand-over start. */
      double band[2];
      omx_eq_lowpass_handover(q, band);
      int same = 1;
      for (int k = 1; k <= 100; k++) {
        double c[5], m[5];
        omx_eq_design(OMX_EQ_LOWPASS, k / 100.0 * band[0] * nyq, q, 0.0, sr, c);
        lpn_matched(k / 100.0 * band[0] * nyq, q, sr, m);
        same &= memcmp(c, m, sizeof c) == 0;
      }
      ok(same, "below the hand-over the low-pass is the matched design, bit for bit", (double)same, 1.0);
      /* sweep: across the hand-over, no step at any grid frequency under the corner larger than
       * twice the matched design's own worst step for the same corner step below the hand-over. */
      double ref_step = 0.0, worst_step = 0.0;
      for (int pass = 0; pass < 2; pass++) {
        double prev[LPN_GRID + 1];
        const double from = pass == 0 ? 0.15 : 0.55;
        for (int k = 0; k <= 1000; k++) {
          const double rc = from + k * 0.0004;
          if (pass == 0 && rc > band[0]) break;
          double c[5];
          omx_eq_design(OMX_EQ_LOWPASS, rc * nyq, q, 0.0, sr, c);
          for (uint32_t i = 1; i <= LPN_GRID; i++) {
            const double f = nyq * i / (LPN_GRID + 1.0);
            const double v = omx_biquad_magnitude_db_d(c, f, sr, -300.0);
            double *w = pass == 0 ? &ref_step : &worst_step;
            if (k > 0 && f < rc * nyq && fabs(v - prev[i]) > *w) *w = fabs(v - prev[i]);
            prev[i] = v;
          }
        }
      }
      printf("  lowpass_nyquist %6.0f Hz Q %.4f: worst step across the hand-over %.4f dB, the matched design's own %.4f dB\n",
             sr, q, worst_step, ref_step);
      ok(worst_step <= LPN_STEP_RATIO * ref_step, "a corner swept across the hand-over moves the passband smoothly",
         worst_step, LPN_STEP_RATIO * ref_step);
    }
  }
  lpn_declared_space();
  expect_clean();
}

