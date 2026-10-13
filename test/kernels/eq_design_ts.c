// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the EQ design and curve against today's TypeScript (openmixer #1601) ------------------ */

#include "fixtures/eq_design_ts.h"

/* The bounds of openmixer's dsp-results-from-c spec §4.3: a coefficient or a Q within
 * EQ_TS_TOL_ULP ulps of the TypeScript's, a magnitude within EQ_TS_TOL_DB dB. Both sides run one
 * formula in double; what differs is the libm: V8's exp, cos, sin and pow are fdlibm's and not
 * always correctly rounded, the C library's are. A frequency of the drawn grid is
 * f_min·pow(ratio, i/(n−1)) on both sides, so it may differ by EQ_TS_TOL_ULP too; the anchors are
 * the same arithmetic and match bit for bit. Measured: 1 ulp for the width law and 2 ulp for a
 * grid frequency, 8.6e-13 dB for a magnitude, and every cookbook coefficient within 4 ulp. */
#define EQ_TS_TOL_ULP 4
#define EQ_TS_TOL_DB 1e-9
/* The gap the spec asks to be named: two families of coefficient pass 4 ulp, both from a
 * cancellation that turns one libm ulp into many.
 *  - The matched bells and shelves at a low corner or a wide Q (20 Hz or 1 kHz at Q 0.3): the
 *    gain normalisation divides two pair values `dc = (1 − e·cos θ)² + (e·sin θ)²` that are each
 *    the small difference of numbers near 1, so an ulp of exp or cos there is ~200 ulp of b0, b1
 *    or b2 (measured 180 ulp, 2e-14 absolute). Taking the TypeScript's Math.pow for the amplitude
 *    instead of exp leaves the gap as it is: it is the pair, not the gain law.
 *  - The fitted low-pass's b2, (b0 + b2) less b0, small where the corner is far below Nyquist
 *    (measured 960 ulp of a b2 of 0.003, 4e-16 absolute).
 * Such a coefficient is held to EQ_TS_TOL_ABS absolute instead: 2.5 times the largest gap, ten
 * million times under the float the engine narrows each coefficient to, and 8.6e-13 dB at most in
 * the magnitude (the curve checks below). */
#define EQ_TS_TOL_ABS 1e-13

/* The distance between two doubles in ulps (their places on the ordered line of doubles). */
static double eq_ts_ulps(double a, double b) {
  if (a == b) return 0.0;
  int64_t ia, ib;
  memcpy(&ia, &a, sizeof ia);
  memcpy(&ib, &b, sizeof ib);
  if ((ia < 0) != (ib < 0)) return INFINITY;
  return ia > ib ? (double)(ia - ib) : (double)(ib - ia);
}

/* A coefficient against the TypeScript's: within EQ_TS_TOL_ULP ulps, or EQ_TS_TOL_ABS absolute. */
static int eq_ts_coeff_ok(double c, double ts, double *worst_ulp, double *worst_abs) {
  const double u = eq_ts_ulps(c, ts), d = fabs(c - ts);
  if (u <= EQ_TS_TOL_ULP) {
    if (u > *worst_ulp) *worst_ulp = u;
    return 1;
  }
  if (d > *worst_abs) *worst_abs = d;
  return d <= EQ_TS_TOL_ABS;
}

/* The ledger after one omx_eq_curve_db() call. At a declared rate it must be empty. At an
 * undeclared rate one call designs each all-pass band at every point, and its expected
 * `rate-is-declared` records outrun the ledger, so the call's records are dropped and each point is
 * evaluated again on its own, with the ledger drained after it: every record is then read, and the
 * point must equal the call's. */
static void eq_ts_curve_drain(const struct omx_eq_band *bands, uint32_t nbands, const double *f, const double *db,
                              uint32_t n, double sr) {
  if (omx_rate_is_declared((float)sr)) {
    eq_curve_drain(sr);
    return;
  }
  omx_contract_reset();
  const double hi = sr / 2.0 * 0.999;
  for (uint32_t i = 0; i < n; i++) {
    const double m = omx_eq_magnitude_sum_db(bands, nbands, f[i] < hi ? f[i] : hi, sr, -120.0);
    ok(m == db[i], "a curve point is its bands' sum, evaluated alone", m, db[i]);
    eq_curve_drain(sr);
  }
}

static void arm_eq_design_ts(void) {
  g_arm = "eq-design-ts";
  double w_design = 0.0, w_abs = 0.0, w_q = 0.0, w_bank = 0.0, w_f = 0.0, w_db = 0.0;
  /* rbjCoeffs */
  for (uint32_t i = 0; i < sizeof TS_DESIGN / sizeof TS_DESIGN[0]; i++) {
    const struct ts_design *t = &TS_DESIGN[i];
    double c[5];
    omx_eq_design(t->kind, t->freq, t->q, t->gain_db, t->sr, c);
    for (int j = 0; j < 5; j++)
      ok(eq_ts_coeff_ok(c[j], t->c[j], &w_design, &w_abs), "a coefficient agrees with rbjCoeffs", c[j], t->c[j]);
    eq_curve_drain(t->sr); /* per row: the ledger holds OMX_CONTRACT_MAX records, a rate's rows more */
  }
  /* complementSection, bit for bit */
  for (uint32_t i = 0; i < sizeof TS_COMPLEMENT / sizeof TS_COMPLEMENT[0]; i++) {
    double k[5];
    omx_eq_complement(TS_COMPLEMENT[i].c, k);
    for (int j = 0; j < 5; j++)
      ok(k[j] == TS_COMPLEMENT[i].out[j], "the complement is complementSection's, bit for bit", k[j], TS_COMPLEMENT[i].out[j]);
  }
  /* eqBandIsIdentity */
  for (uint32_t i = 0; i < sizeof TS_IDENTITY / sizeof TS_IDENTITY[0]; i++)
    ok(omx_eq_band_is_identity(TS_IDENTITY[i].kind, TS_IDENTITY[i].gain_db) == TS_IDENTITY[i].identity,
       "identity is eqBandIsIdentity's", TS_IDENTITY[i].gain_db, TS_IDENTITY[i].identity);
  /* octavesToQ */
  for (uint32_t i = 0; i < sizeof TS_OCTAVES_Q / sizeof TS_OCTAVES_Q[0]; i++) {
    const double q = omx_eq_octaves_q(TS_OCTAVES_Q[i].bw_oct), u = eq_ts_ulps(q, TS_OCTAVES_Q[i].q);
    if (u > w_q) w_q = u;
    ok(u <= EQ_TS_TOL_ULP, "the width's Q agrees with octavesToQ", q, TS_OCTAVES_Q[i].q);
  }
  /* eqCoeffs, passFilterCoeffs, eqStateCurveDb and magnitudeSumDb per state and rate */
  for (uint32_t r = 0; r < sizeof TS_RUNS / sizeof TS_RUNS[0]; r++) {
    const struct ts_run *t = &TS_RUNS[r];
    const struct ts_state *s = &TS_STATES[t->state];
    double bank[6][5], pass[OMX_EQ_PASS_MAX_SECTIONS][5];
    uint8_t on[6];
    const uint32_t nb = omx_eq_bank_design(s->on, s->b, s->nb, t->sr, bank, on);
    ok(nb == t->bank_n, "the bank has eqCoeffs' sections", nb, t->bank_n);
    for (uint32_t i = 0; i < nb && i < t->bank_n; i++) {
      ok(on[i] == TS_BANK_ON[t->bank_off + i], "a section runs or parks as eqCoeffs says", on[i], TS_BANK_ON[t->bank_off + i]);
      for (int j = 0; j < 5; j++)
        ok(eq_ts_coeff_ok(bank[i][j], TS_BANK[t->bank_off + i][j], &w_bank, &w_abs), "a bank coefficient agrees with eqCoeffs",
           bank[i][j], TS_BANK[t->bank_off + i][j]);
    }
    const uint32_t np = omx_eq_pass_design(&s->hpf, &s->lpf, t->sr, pass);
    ok(np == t->pass_n, "the head filters have passFilterCoeffs' sections", np, t->pass_n);
    for (uint32_t i = 0; i < np && i < t->pass_n; i++)
      for (int j = 0; j < 5; j++)
        ok(eq_ts_coeff_ok(pass[i][j], TS_BANK[t->pass_off + i][j], &w_bank, &w_abs), "a head coefficient agrees with passFilterCoeffs",
           pass[i][j], TS_BANK[t->pass_off + i][j]);
    struct omx_eq_band drawn[6 + OMX_EQ_PASS_MAX_SECTIONS];
    const uint32_t nd = omx_eq_curve_bands(s->on, s->b, s->nb, &s->hpf, &s->lpf, drawn);
    double f[OMX_EQ_CURVE_CAP(160u, 6u + OMX_EQ_PASS_MAX_SECTIONS)], db[OMX_EQ_CURVE_CAP(160u, 6u + OMX_EQ_PASS_MAX_SECTIONS)];
    ok(TS_WINDOW_POINTS == 160u, "the fixture's window is the 160 points this arm sizes for", TS_WINDOW_POINTS, 160.0);
    const uint32_t n = omx_eq_curve_db(drawn, nd, TS_WINDOW_F_MIN, TS_WINDOW_F_MAX, TS_WINDOW_POINTS, t->sr, -120.0, f, db);
    ok(n == t->curve_n, "the curve has eqStateCurveDb's points", n, t->curve_n);
    for (uint32_t i = 0; i < n && i < t->curve_n; i++) {
      const double tf = TS_STATE_F[TS_STATE_F_OFF[t->state] + i], td = TS_STATE_DB[t->curve_off + i];
      const double u = eq_ts_ulps(f[i], tf), d = fabs(db[i] - td);
      if (u > w_f) w_f = u;
      if (d > w_db) w_db = d;
      ok(u <= EQ_TS_TOL_ULP, "a curve frequency agrees with eqStateCurveDb's", f[i], tf);
      ok(d <= EQ_TS_TOL_DB, "a curve point agrees with eqStateCurveDb", db[i], td);
    }
    eq_ts_curve_drain(drawn, nd, f, db, n, t->sr);
    for (uint32_t i = 0; i < t->seat_n; i++) {
      const double sf = TS_SEAT_F[t->seat_off + i], m = omx_eq_magnitude_sum_db(drawn, nd, sf, t->sr, -120.0);
      const double d = fabs(m - TS_SEAT_DB[t->seat_off + i]);
      if (d > w_db) w_db = d;
      ok(d <= EQ_TS_TOL_DB, "a seat agrees with magnitudeSumDb", m, TS_SEAT_DB[t->seat_off + i]);
    }
    eq_curve_drain(t->sr);
  }
  /* biquadMagnitudeDb, at, around and past Nyquist */
  for (uint32_t i = 0; i < sizeof TS_BANDMAG / sizeof TS_BANDMAG[0]; i++) {
    const struct ts_bandmag *t = &TS_BANDMAG[i];
    const double m = omx_eq_band_magnitude_db(&t->b, t->f, t->sr, -120.0), d = fabs(m - t->db);
    if (d > w_db) w_db = d;
    ok(d <= EQ_TS_TOL_DB, "a band's magnitude agrees with biquadMagnitudeDb", m, t->db);
    if (!(t->f > 0.0) || t->f >= t->sr / 2.0)
      ok(m == 0.0 && t->db == 0.0, "past Nyquist or at a non-positive frequency both read 0 dB exactly", m, t->db);
    if (i + 1 == sizeof TS_BANDMAG / sizeof TS_BANDMAG[0] || TS_BANDMAG[i + 1].sr != t->sr) eq_curve_drain(t->sr);
  }
  /* compositeCurveDb */
  const uint32_t ncomp = (uint32_t)(sizeof TS_COMPOSITE_F / sizeof TS_COMPOSITE_F[0]);
  for (uint32_t r = 0; r < sizeof TS_COMPOSITE / sizeof TS_COMPOSITE[0]; r++) {
    struct omx_eq_band drawn[3 + OMX_EQ_PASS_MAX_SECTIONS], three[3];
    for (int b = 0; b < 3; b++) three[b] = TS_BANDMAG[(uint32_t)b * 9u].b;
    const uint32_t nd = omx_eq_curve_bands(1, three, 3u, &TS_COMPOSITE_HPF, &TS_COMPOSITE_LPF, drawn);
    double f[OMX_EQ_CURVE_CAP(160u, 3u + OMX_EQ_PASS_MAX_SECTIONS)], db[OMX_EQ_CURVE_CAP(160u, 3u + OMX_EQ_PASS_MAX_SECTIONS)];
    const uint32_t n = omx_eq_curve_db(drawn, nd, TS_AXIS_F_MIN, TS_AXIS_F_MAX, 160u, TS_COMPOSITE[r].sr, -120.0, f, db);
    ok(n == ncomp, "the composite has compositeCurveDb's points", n, ncomp);
    for (uint32_t i = 0; i < n && i < ncomp; i++) {
      const double u = eq_ts_ulps(f[i], TS_COMPOSITE_F[i]), d = fabs(db[i] - TS_COMPOSITE_DB[TS_COMPOSITE[r].off + i]);
      if (u > w_f) w_f = u;
      if (d > w_db) w_db = d;
      ok(u <= EQ_TS_TOL_ULP, "a composite frequency agrees with compositeCurveDb's", f[i], TS_COMPOSITE_F[i]);
      ok(d <= EQ_TS_TOL_DB, "a composite point agrees with compositeCurveDb", db[i], TS_COMPOSITE_DB[TS_COMPOSITE[r].off + i]);
    }
    eq_ts_curve_drain(drawn, nd, f, db, n, TS_COMPOSITE[r].sr);
  }
  printf("eq-design-ts: largest gaps from the TypeScript: design %.0f ulp, bank %.0f ulp, past the ulp bound %.3g absolute, "
         "octaves-Q %.0f ulp, curve frequency %.0f ulp, magnitude %.3g dB (bounds %d ulp, %.0e, %.0e dB)\n",
         w_design, w_bank, w_abs, w_q, w_f, w_db, EQ_TS_TOL_ULP, EQ_TS_TOL_ABS, EQ_TS_TOL_DB);
  expect_clean();
}
