// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the EQ bank, the head filters and the drawn curve (openmixer #1601) ------------------- */

static const float EQ_CURVE_RME_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;

/* The ledger after one rate's checks, emptied: at a rate outside the declaration the all-pass
 * designs' `rate-is-declared` precondition is the contract saying so, and is explained; every other
 * record fails the arm. eq_design_ts.c, after this file by name, reads it too. */
static void eq_curve_drain(double sr) {
  const int declared = omx_rate_is_declared((float)sr);
  const uint32_t seen = omx_contract_log.count;
  const uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  uint32_t unexplained = seen - kept;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (omx_contract_record_ready(r) && !declared && strcmp(r->kind, "pre") == 0 &&
        strcmp(r->token, "rate-is-declared") == 0)
      continue;
    unexplained++;
    printf("VIOLATION @ %.0f Hz [%s] %s %s\n", sr, r->stage, r->kind, r->token);
  }
  ok(unexplained == 0u, "no unexplained contract violation at this rate", (double)unexplained, 0.0);
  omx_contract_reset();
}

/* |H(e^jw)|², linear, of a double section. */
static double eq_curve_power(const double c[5], double w) {
  const double nr = c[0] + c[1] * cos(w) + c[2] * cos(2.0 * w), ni = -(c[1] * sin(w) + c[2] * sin(2.0 * w));
  const double dr = 1.0 + c[3] * cos(w) + c[4] * cos(2.0 * w), di = -(c[3] * sin(w) + c[4] * sin(2.0 * w));
  return (nr * nr + ni * ni) / (dr * dr + di * di);
}

static void arm_eq_curve(void) {
  g_arm = "eq-curve";
  const double db3 = 20.0 * log10(M_SQRT1_2);
  /* The analogue width law: 1/Q = 2^(N/2) − 2^(−N/2), and the 0.05-octave floor. */
  for (double n = 0.05; n <= 4.0; n += 0.25) {
    const double q = omx_eq_octaves_q(n), want = 1.0 / (pow(2.0, n / 2.0) - pow(2.0, -n / 2.0));
    ok(fabs(q - want) <= 1e-12 * want, "octaves-to-Q meets 1/(2^(N/2) - 2^(-N/2))", q, want);
  }
  ok(fabs(omx_eq_octaves_q(1.0) - M_SQRT2) <= 4e-16 * M_SQRT2, "one octave is Q sqrt2", omx_eq_octaves_q(1.0), M_SQRT2);
  ok(omx_eq_octaves_q(0.0) == omx_eq_octaves_q(0.05) && omx_eq_octaves_q(-1.0) == omx_eq_octaves_q(0.05),
     "a width under 0.05 octave reads as 0.05", omx_eq_octaves_q(0.0), omx_eq_octaves_q(0.05));
  /* Identity: a gain kind at exactly 0 dB, no other. */
  for (int k = OMX_EQ_PEAKING; k <= OMX_EQ_ALLPASS2; k++) {
    const int gain_kind = k == OMX_EQ_PEAKING || k == OMX_EQ_LOWSHELF || k == OMX_EQ_HIGHSHELF;
    ok(omx_eq_band_is_identity((enum omx_eq_kind)k, 0.0) == gain_kind, "only a gain kind at 0 dB is identity", k, gain_kind);
    ok(!omx_eq_band_is_identity((enum omx_eq_kind)k, 0.1) && !omx_eq_band_is_identity((enum omx_eq_kind)k, -1e-9),
       "a band off 0 dB is not identity", k, 0.0);
  }
  for (uint32_t ri = 0; ri < OMX_RME_RATES_COUNT; ri++) {
    const double sr = EQ_CURVE_RME_RATES[ri];
    double c[5], k[5], kk[5];
    /* The identity a parked band rests on: numerator and denominator equal, term for term. */
    for (int kind = OMX_EQ_PEAKING; kind <= OMX_EQ_HIGHSHELF; kind++) {
      omx_eq_design((enum omx_eq_kind)kind, 1234.0, 2.7, 0.0, sr, c);
      ok(c[0] == 1.0 && c[1] == c[3] && c[2] == c[4], "a 0 dB gain kind designs H(z) = 1 exactly", c[1], c[3]);
    }
    /* The complement: the bandpass and its complement are power complementary, |B|^2 + |1-B|^2 = 1;
     * the complement of the complement is the section; the denominator is kept. */
    omx_eq_design(OMX_EQ_BANDPASS, 3000.0, omx_eq_bandwidth_q(3000.0, 1.0, sr), 0.0, sr, c);
    omx_eq_complement(c, k);
    omx_eq_complement(k, kk);
    ok(k[3] == c[3] && k[4] == c[4], "the complement keeps the denominator", k[3], c[3]);
    for (int i = 0; i < 5; i++)
      ok(fabs(kk[i] - c[i]) <= 1e-15, "the complement of the complement is the section, to rounding", kk[i], c[i]);
    for (int i = 1; i < 64; i++) {
      const double w = M_PI * i / 64.0, p = eq_curve_power(c, w) + eq_curve_power(k, w);
      ok(fabs(p - 1.0) < 1e-12, "a bandpass and its complement are power complementary", p, 1.0);
    }
    /* The bank: off bands leave, identity bands park in place, the rest run, in order. */
    const struct omx_eq_band bands[5] = {
        {OMX_EQ_LOWSHELF, 90.0, 3.0, 0.7, 1}, {OMX_EQ_PEAKING, 400.0, 0.0, 1.0, 1}, {OMX_EQ_PEAKING, 900.0, 5.0, 2.0, 0},
        {OMX_EQ_NOTCH, 130.0, 0.0, 116.0, 1}, {OMX_EQ_HIGHSHELF, 9000.0, 0.0, 0.7, 1}};
    double bank[5][5];
    uint8_t on[5];
    const uint32_t nb = omx_eq_bank_design(1, bands, 5u, sr, bank, on);
    ok(nb == 4u, "the bank holds the bands that are on", nb, 4.0);
    const uint32_t want_idx[4] = {0, 1, 3, 4};
    const uint8_t want_on[4] = {1, 0, 1, 0};
    for (uint32_t i = 0; i < nb && i < 4u; i++) {
      const struct omx_eq_band *b = &bands[want_idx[i]];
      omx_eq_design(b->kind, b->freq_hz, b->q, b->gain_db, sr, c);
      for (int j = 0; j < 5; j++) ok(bank[i][j] == c[j], "a bank section is its band's design, in order", bank[i][j], c[j]);
      ok(on[i] == want_on[i], "an identity band is parked, every other runs", on[i], want_on[i]);
    }
    ok(omx_eq_bank_design(0, bands, 5u, sr, bank, on) == 0u, "an EQ that is off is an empty bank", 0.0, 0.0);
    /* The head filters: one section at 12 dB/oct, two at 24; the corner reads -3.01 dB at both
     * slopes, the high-pass by the cookbook's |H(w0)| = Q, the fitted low-pass by its fit at f0. */
    double pass[OMX_EQ_PASS_MAX_SECTIONS][5];
    for (int slope = 0; slope <= 1; slope++) {
      const struct omx_eq_pass hp = {1, 1000.0, slope}, lp = {1, 4000.0, slope}, off = {0, 1000.0, 1};
      const uint32_t want_n = slope ? 2u : 1u;
      ok(omx_eq_pass_design(&hp, &off, sr, pass) == want_n, "the high-pass alone is its sections", want_n, want_n);
      double db = 0.0;
      for (uint32_t s = 0; s < want_n; s++) db += omx_biquad_magnitude_db_d(pass[s], 1000.0, sr, -120.0);
      ok(fabs(db - db3) < 1e-6, "the Butterworth high-pass reads -3.01 dB at its corner", db, db3);
      ok(omx_eq_pass_design(&off, &lp, sr, pass) == want_n, "the low-pass alone is its sections", want_n, want_n);
      db = 0.0;
      for (uint32_t s = 0; s < want_n; s++) db += omx_biquad_magnitude_db_d(pass[s], 4000.0, sr, -120.0);
      ok(fabs(db - db3) < 1e-6, "the Butterworth low-pass reads -3.01 dB at its corner", db, db3);
      ok(omx_eq_pass_design(&hp, &lp, sr, pass) == 2u * want_n, "the high-pass then the low-pass", 2.0 * want_n, 2.0 * want_n);
      omx_eq_design(OMX_EQ_HIGHPASS, 1000.0, slope ? 0.5411961 : M_SQRT1_2, 0.0, sr, c);
      for (int j = 0; j < 5; j++) ok(pass[0][j] == c[j], "the high-pass comes first", pass[0][j], c[j]);
      ok(omx_eq_pass_design(&off, &off, sr, pass) == 0u, "no filter on, no section", 0.0, 0.0);
    }
    /* A band's magnitude: the evaluator over its design below 0.999 Nyquist, unclamped up to
     * Nyquist, 0 dB at or past it and at a non-positive frequency. */
    const struct omx_eq_band bell = {OMX_EQ_PEAKING, 1000.0, 6.0, 2.0, 1};
    omx_eq_design(bell.kind, bell.freq_hz, bell.q, bell.gain_db, sr, c);
    for (int i = 1; i < 64; i++) {
      const double f = sr * 0.4995 * i / 64.0;
      ok(omx_eq_band_magnitude_db(&bell, f, sr, -120.0) == omx_biquad_magnitude_db_d(c, f, sr, -120.0),
         "a band reads its design's magnitude", omx_eq_band_magnitude_db(&bell, f, sr, -120.0), omx_biquad_magnitude_db_d(c, f, sr, -120.0));
    }
    const double sliver = sr * 0.49975;
    ok(omx_eq_band_magnitude_db(&bell, sliver, sr, -120.0) == omx_biquad_magnitude_db_w(c, 2.0 * M_PI * sliver / sr, -120.0),
       "a band reads at its own frequency up to Nyquist", omx_eq_band_magnitude_db(&bell, sliver, sr, -120.0), 0.0);
    ok(omx_eq_band_magnitude_db(&bell, sr * 0.5, sr, -120.0) == 0.0 && omx_eq_band_magnitude_db(&bell, sr, sr, -120.0) == 0.0 &&
           omx_eq_band_magnitude_db(&bell, 0.0, sr, -120.0) == 0.0 && omx_eq_band_magnitude_db(&bell, -3.0, sr, -120.0) == 0.0,
       "a band reads 0 dB at or past Nyquist and at a non-positive frequency", 0.0, 0.0);
    /* The curve's frequencies: a log grid from edge to edge, the anchors inside, ascending. */
    enum { G = 40 };
    double f[OMX_EQ_CURVE_CAP(G, 4u)], db[OMX_EQ_CURVE_CAP(G, 4u)];
    uint32_t n = omx_eq_curve_freqs(NULL, 0u, 20.0, 20000.0, G, f);
    ok(n == G && f[0] == 20.0 && fabs(f[G - 1] - 20000.0) <= 1e-12 * 20000.0, "the grid runs edge to edge", f[G - 1], 20000.0);
    const double step = pow(1000.0, 1.0 / (G - 1));
    for (uint32_t i = 1; i < n; i++) ok(fabs(f[i] / f[i - 1] - step) < 1e-12, "the grid is log-spaced", f[i] / f[i - 1], step);
    ok(omx_eq_curve_freqs(NULL, 0u, 20.0, 20000.0, 0u, f) == 2u && f[0] == 20.0, "a grid has two points at least", 2.0, 2.0);
    const struct omx_eq_band drawn[4] = {
        {OMX_EQ_NOTCH, 130.0, 0.0, 116.0, 1}, {OMX_EQ_HIGHPASS, 1000.0, 0.0, M_SQRT1_2, 1},
        {OMX_EQ_PEAKING, 20.0, 9.0, 1.0, 1}, {OMX_EQ_LOWSHELF, 20000.0, -6.0, 2.0, 1}};
    n = omx_eq_curve_freqs(drawn, 4u, 20.0, 20000.0, G, f);
    ok(n == G + 3u + 3u + 1u + 1u, "each anchor strictly inside the axis is added", n, G + 8.0);
    for (uint32_t i = 1; i < n; i++) ok(f[i - 1] <= f[i], "the frequencies ascend", f[i - 1], f[i]);
    /* The curve: at every point the sum of the bands' magnitudes, with the point held under
     * 0.999 Nyquist; a Q-116 notch reads the floor on its centre, a Butterworth corner -3.01 dB. */
    n = omx_eq_curve_db(drawn, 2u, 20.0, 20000.0, G, sr, -120.0, f, db);
    const double hi = sr * 0.5 * 0.999;
    int saw_null = 0, saw_corner = 0;
    for (uint32_t i = 0; i < n; i++) {
      const double fe = f[i] < hi ? f[i] : hi;
      const double want = omx_eq_band_magnitude_db(&drawn[0], fe, sr, -120.0) + omx_eq_band_magnitude_db(&drawn[1], fe, sr, -120.0);
      ok(db[i] == want, "a curve point is the sum of its bands", db[i], want);
      if (f[i] == 130.0) { saw_null = 1; ok(db[i] < -120.0 + 1.0, "the notch reaches the floor on its centre", db[i], -120.0); }
    }
    n = omx_eq_curve_db(&drawn[1], 1u, 20.0, 20000.0, G, sr, -120.0, f, db);
    for (uint32_t i = 0; i < n; i++)
      if (f[i] == 1000.0) { saw_corner = 1; ok(fabs(db[i] - db3) < 1e-9, "the high-pass reads -3.01 dB at its anchor", db[i], db3); }
    ok(saw_null && saw_corner, "the curve is sampled at the notch and the corner", saw_null, saw_corner);
    n = omx_eq_curve_db(NULL, 0u, 20.0, 20000.0, G, sr, -120.0, f, db);
    for (uint32_t i = 0; i < n; i++) ok(db[i] == 0.0, "no band is a flat line", db[i], 0.0);
    eq_curve_drain(sr);
  }
  /* The bands drawn: the EQ's own while it is on, then each armed filter with a positive corner,
   * one band per Butterworth section. */
  const struct omx_eq_band eqb[2] = {{OMX_EQ_PEAKING, 500.0, 3.0, 1.0, 1}, {OMX_EQ_NOTCH, 60.0, 0.0, 30.0, 0}};
  const struct omx_eq_pass hp24 = {1, 80.0, 1}, lp12 = {1, 15000.0, 0}, lp0 = {1, 0.0, 0};
  struct omx_eq_band out[2 + OMX_EQ_PASS_MAX_SECTIONS];
  uint32_t n = omx_eq_curve_bands(1, eqb, 2u, &hp24, &lp12, out);
  ok(n == 4u && out[0].kind == OMX_EQ_PEAKING && out[1].kind == OMX_EQ_HIGHPASS && out[1].q == 0.5411961 &&
         out[2].q == 1.3065630 && out[3].kind == OMX_EQ_LOWPASS && out[3].q == M_SQRT1_2 && out[3].gain_db == 0.0,
     "the EQ's live band, the 24 dB high-pass's two sections, the 12 dB low-pass", n, 4.0);
  ok(omx_eq_curve_bands(0, eqb, 2u, &hp24, &lp0, out) == 2u && out[0].kind == OMX_EQ_HIGHPASS,
     "an EQ that is off draws its armed filters alone; a filter at 0 Hz draws nothing", n, 2.0);
  ok(omx_eq_curve_bands(1, eqb, 2u, NULL, NULL, out) == 1u, "no filter given, none drawn", 1.0, 1.0);
  expect_clean();
}
