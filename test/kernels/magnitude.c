// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the magnitude evaluator (FreeMixer/omx-dsp#69) ------------------------------------------ */

static const float MAGNITUDE_RME_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;

/* A log grid of `n` points from 10 Hz to just under Nyquist. */
static void magnitude_grid(double sr, double *f, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) f[i] = 10.0 * pow(sr * 0.4995 / 10.0, (double)i / (n - 1));
}

/* The ledger after one rate's checks, emptied: at a rate outside the declaration a design's
 * `rate-is-declared` precondition is the contract saying so (the all-pass designs carry one), and
 * is explained; every other record fails the arm. magnitude_ts.c, after this file by name, reads it
 * too. */
static void magnitude_drain(double sr) {
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

static void arm_magnitude(void) {
  g_arm = "magnitude";
  enum { N = 64 };
  double f[N];
  for (uint32_t ri = 0; ri < OMX_RME_RATES_COUNT; ri++) {
    const double sr = MAGNITUDE_RME_RATES[ri];
    magnitude_grid(sr, f, N);
    /* A pure gain and a pure delay: 20·log10(g), and 0 dB. */
    const double gain[5] = {0.5, 0.0, 0.0, 0.0, 0.0}, delay[5] = {0.0, 0.0, 1.0, 0.0, 0.0};
    for (uint32_t i = 0; i < N; i++) {
      ok(fabs(omx_biquad_magnitude_db_d(gain, f[i], sr, -120.0) - 20.0 * log10(0.5)) < 1e-12,
         "a gain of one half reads -6.02 dB", omx_biquad_magnitude_db_d(gain, f[i], sr, -120.0), 20.0 * log10(0.5));
      ok(fabs(omx_biquad_magnitude_db_d(delay, f[i], sr, -120.0)) < 1e-12, "a delay reads 0 dB",
         omx_biquad_magnitude_db_d(delay, f[i], sr, -120.0), 0.0);
    }
    /* Two zeros on the unit circle at theta: |N| = 2·|cos w − cos theta|. Two poles at r·e^±jtheta:
     * 1/|H| is the product of the distances from e^jw to the poles. */
    const double th = 2.0 * M_PI * 1000.0 / sr, r = 0.95;
    const double zeros[5] = {1.0, -2.0 * cos(th), 1.0, 0.0, 0.0};
    const double poles[5] = {1.0, 0.0, 0.0, -2.0 * r * cos(th), r * r};
    for (uint32_t i = 0; i < N; i++) {
      const double w = 2.0 * M_PI * f[i] / sr;
      const double zc = 20.0 * log10(2.0 * fabs(cos(w) - cos(th)));
      const double pc = -10.0 * log10((1.0 - 2.0 * r * cos(w - th) + r * r) * (1.0 - 2.0 * r * cos(w + th) + r * r));
      const double zm = omx_biquad_magnitude_db_d(zeros, f[i], sr, -300.0);
      const double pm = omx_biquad_magnitude_db_d(poles, f[i], sr, -300.0);
      ok(fabs(zm - zc) < 1e-9 * (1.0 + fabs(zc)), "a zero pair meets 2|cos w - cos theta|", zm, zc);
      ok(fabs(pm - pc) < 1e-9, "a pole pair meets the product of its distances", pm, pc);
    }
    /* Designed sections with a closed form: the cookbook high-pass reads Q at its corner, the
     * cookbook bandpass 0 dB at its centre and -3.01 dB at its exact bilinear edges, both
     * all-passes 0 dB everywhere, the notch the floor on its centre. */
    double c[5];
    const double db3 = 20.0 * log10(M_SQRT1_2);
    omx_eq_design(OMX_EQ_HIGHPASS, 1000.0, M_SQRT1_2, 0.0, sr, c);
    ok(fabs(omx_biquad_magnitude_db_d(c, 1000.0, sr, -120.0) - db3) < 1e-9, "the Butterworth high-pass reads -3.01 dB at its corner",
       omx_biquad_magnitude_db_d(c, 1000.0, sr, -120.0), db3);
    const double q = omx_eq_bandwidth_q(2000.0, 1.0, sr);
    omx_eq_design(OMX_EQ_BANDPASS, 2000.0, q, 0.0, sr, c);
    ok(fabs(omx_biquad_magnitude_db_d(c, 2000.0, sr, -120.0)) < 1e-9, "the bandpass reads 0 dB at its centre",
       omx_biquad_magnitude_db_d(c, 2000.0, sr, -120.0), 0.0);
    /* The exact -3 dB edges of the bilinear bandpass: Ω2 − Ω1 = 1/Q, Ω1·Ω2 = 1 in the prototype,
     * Ω = tan(w/2)/tan(w0/2). */
    const double t0 = tan(M_PI * 2000.0 / sr), root = sqrt(1.0 / (q * q) + 4.0);
    const double e1 = sr / M_PI * atan(t0 * (root - 1.0 / q) / 2.0), e2 = sr / M_PI * atan(t0 * (root + 1.0 / q) / 2.0);
    const double lo = omx_biquad_magnitude_db_d(c, e1, sr, -120.0), hi = omx_biquad_magnitude_db_d(c, e2, sr, -120.0);
    ok(fabs(lo - db3) < 1e-9, "the bandpass reads -3.01 dB at its lower edge", lo, db3);
    ok(fabs(hi - db3) < 1e-9, "the bandpass reads -3.01 dB at its upper edge", hi, db3);
    for (int k = OMX_EQ_ALLPASS1; k <= OMX_EQ_ALLPASS2; k++) {
      omx_eq_design((enum omx_eq_kind)k, 700.0, 0.8, 0.0, sr, c);
      for (uint32_t i = 0; i < N; i++)
        ok(fabs(omx_biquad_magnitude_db_d(c, f[i], sr, -120.0)) < 1e-9, "an all-pass reads 0 dB",
           omx_biquad_magnitude_db_d(c, f[i], sr, -120.0), 0.0);
    }
    omx_eq_design(OMX_EQ_NOTCH, 130.0, 116.0, 0.0, sr, c);
    ok(omx_biquad_magnitude_db_d(c, 130.0, sr, -120.0) == -120.0, "a Q-116 notch reads the floor on its centre",
       omx_biquad_magnitude_db_d(c, 130.0, sr, -120.0), -120.0);
    ok(omx_biquad_magnitude_db_d(c, 130.0, sr, -150.0) == -150.0, "the floor is the caller's",
       omx_biquad_magnitude_db_d(c, 130.0, sr, -150.0), -150.0);
    ok(omx_biquad_magnitude_db_d(c, sr, sr, -120.0) == omx_biquad_magnitude_db_d(c, sr * 0.5 * 0.999, sr, -120.0),
       "a frequency past Nyquist reads as 0.999 Nyquist", sr, sr * 0.5 * 0.999);
    ok(omx_biquad_magnitude_db_d(c, 0.0, sr, -120.0) == 0.0 && omx_biquad_magnitude_db_d(c, -5.0, sr, -120.0) == 0.0,
       "a non-positive frequency reads 0 dB", 0.0, 0.0);
    /* A cascade is the sum of its live sections; a parked one adds nothing; the float bank reads
     * as its exact widening. */
    double bank[3][5], one[N], two[N], sum[N];
    float fbank[3][5];
    omx_eq_design(OMX_EQ_PEAKING, 300.0, 1.5, 7.0, sr, bank[0]);
    omx_eq_design(OMX_EQ_HIGHSHELF, 6000.0, 0.7, -4.0, sr, bank[1]);
    omx_eq_design(OMX_EQ_NOTCH, 900.0, 30.0, 0.0, sr, bank[2]);
    const uint8_t parked[3] = {1, 1, 0};
    omx_biquad_cascade_magnitude_db_d((const double(*)[5])bank, NULL, 1u, f, N, sr, -120.0, one);
    omx_biquad_cascade_magnitude_db_d((const double(*)[5])&bank[1], NULL, 1u, f, N, sr, -120.0, two);
    omx_biquad_cascade_magnitude_db_d((const double(*)[5])bank, parked, 3u, f, N, sr, -120.0, sum);
    for (uint32_t i = 0; i < N; i++)
      ok(sum[i] == one[i] + two[i], "a cascade is the sum of its live sections", sum[i], one[i] + two[i]);
    omx_biquad_cascade_magnitude_db_d((const double(*)[5])bank, NULL, 0u, f, N, sr, -120.0, sum);
    for (uint32_t i = 0; i < N; i++) ok(sum[i] == 0.0, "an empty cascade is flat", sum[i], 0.0);
    for (int b = 0; b < 3; b++) omx_eq_design_f((enum omx_eq_kind)(b == 0 ? OMX_EQ_PEAKING : b == 1 ? OMX_EQ_HIGHSHELF : OMX_EQ_NOTCH),
                                                b == 0 ? 300.0 : b == 1 ? 6000.0 : 900.0, b == 0 ? 1.5 : b == 1 ? 0.7 : 30.0,
                                                b == 0 ? 7.0 : b == 1 ? -4.0 : 0.0, sr, fbank[b]);
    omx_biquad_cascade_magnitude_db((const float(*)[5])fbank, NULL, 3u, f, N, sr, -120.0, sum);
    for (uint32_t i = 0; i < N; i++) {
      double want = 0.0;
      for (int b = 0; b < 3; b++) {
        const double w[5] = {fbank[b][0], fbank[b][1], fbank[b][2], fbank[b][3], fbank[b][4]};
        want += omx_biquad_magnitude_db_d(w, f[i], sr, -120.0);
      }
      ok(sum[i] == want, "the float bank reads as its exact widening", sum[i], want);
    }
    magnitude_drain(sr);
  }
  expect_clean();
}

