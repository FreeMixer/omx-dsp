// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the double-precision section and cascade (spec §1 row 2a) ------------------------------- */

/* The steady-state magnitude of a DOUBLE section at `hz`, by a sine through omx_biquad_d(). */
static double section_d_mag_at(const double c[5], double hz, double sr, double secs) {
  double st[4] = {0};
  const double w = 2.0 * M_PI * hz / sr;
  const uint32_t settle = (uint32_t)(sr * secs), meas = (uint32_t)lround(floor(secs * hz) * sr / hz);
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < settle + meas; i++) {
    const float y = omx_biquad_d((float)sin(w * i), c, st);
    if (i >= settle) { re += y * cos(w * i); im += y * sin(w * i); }
  }
  return 20.0 * log10(2.0 * sqrt(re * re + im * im) / meas);
}

static void arm_biquad_d(void) {
  g_arm = "biquad-double";
  const double unity[5] = {1.0, 0.0, 0.0, 0.0, 0.0};
  double s[4] = {0};
  float in[64];
  int same = 1;
  for (int i = 0; i < 64; i++) { in[i] = rnd(-1.0f, 1.0f); same &= omx_biquad_d(in[i], unity, s) == in[i]; }
  ok(same, "{1,0,0,0,0} in double is the identity bit for bit", same, 1.0);
  /* The pole pair nearest the unit circle the console runs: an ISO 20 Hz band, Q 4.3, +15 dB. Its
   * float32 twin reads 12.59 dB at 192 kHz; the double section meets the closed form. */
  const double fc = 1000.0 * pow(2.0, -17.0 / 3.0), q = pow(2.0, 1.0 / 6.0) / (pow(2.0, 1.0 / 3.0) - 1.0);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    double c[5];
    omx_eq_design(OMX_EQ_PEAKING, fc, q, 15.0, sr, c);
    const double m = section_d_mag_at(c, fc, sr, 2.0);
    ok(fabs(m - 15.0) < 0.005, "the double 20 Hz Q 4.3 +15 dB bell reads +15 dB at its centre", m, 15.0);
  }
  /* The cascade is the band-outer omx_biquad_d loop, bit for bit, output and state, both legs. */
  double cf[5][5], sl0[5][4] = {{0}}, sr0[5][4] = {{0}}, sl1[5][4] = {{0}}, sr1[5][4] = {{0}};
  const uint8_t en[5] = {1, 0, 1, 1, 0};
  for (int b = 0; b < 5; b++) omx_eq_design(OMX_EQ_PEAKING, 100.0 * (b + 1), 2.0, rnd(-12.0f, 12.0f), 48000.0, cf[b]);
  float l0[256], r0[256], l1[256], r1[256];
  for (int i = 0; i < 256; i++) { l0[i] = l1[i] = rnd(-1.0f, 1.0f); r0[i] = r1[i] = rnd(-1.0f, 1.0f); }
  for (int b = 0; b < 5; b++) {
    if (!en[b]) continue;
    for (int i = 0; i < 256; i++) { l0[i] = omx_biquad_d(l0[i], cf[b], sl0[b]); r0[i] = omx_biquad_d(r0[i], cf[b], sr0[b]); }
  }
  omx_biquad_cascade_d_stereo(l1, r1, 256u, 5u, (const double(*)[5])cf, en, sl1, sr1);
  const int eq = !memcmp(l0, l1, sizeof l0) && !memcmp(r0, r1, sizeof r0) && !memcmp(sl0, sl1, sizeof sl0) && !memcmp(sr0, sr1, sizeof sr0);
  ok(eq, "the double cascade is the band-outer omx_biquad_d loop, bit for bit (output and state)", eq, 1.0);
  ok(omx_flush_d(1e-21) == 0.0 && omx_flush_d(-1e-300) == 0.0 && omx_flush_d(1e-19) == 1e-19,
     "omx_flush_d zeroes below 1e-20 and keeps the rest", omx_flush_d(1e-19), 1e-19);
  expect_clean();
}
