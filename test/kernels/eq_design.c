// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- EQ design and the matched pair ---------------------------------------------------------- */

/* The steady-state magnitude of a float section at `hz`, measured by running a sine through it. */
static double section_mag_at(const float c[5], double hz, double sr) {
  float st[4] = {0};
  const double w = 2.0 * M_PI * hz / sr;
  const uint32_t settle = (uint32_t)(sr * 0.25), meas = (uint32_t)(sr * 0.25);
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < settle + meas; i++) {
    const float y = omx_biquad(sinf((float)(w * i)), c, st);
    if (i >= settle) { re += y * cos(w * i); im += y * sin(w * i); }
  }
  return 20.0 * log10(2.0 * sqrt(re * re + im * im) / meas);
}

static void arm_eq_design(void) {
  g_arm = "eq-design";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    float c[5];
    omx_eq_design_f(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, c);
    ok(fabs(section_mag_at(c, 1000.0, sr) - 6.0) < 0.05, "a +6 dB bell reads +6 dB at its centre", section_mag_at(c, 1000.0, sr), 6.0);
    ok(fabs(section_mag_at(c, 40.0, sr)) < 0.1, "the bell is unity far below its centre", section_mag_at(c, 40.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_HIGHPASS, 200.0, M_SQRT1_2, 0.0, sr, c);
    ok(fabs(section_mag_at(c, 200.0, sr) + 3.01) < 0.1, "the Butterworth high-pass sits at -3 dB on its corner", section_mag_at(c, 200.0, sr), -3.01);
    ok(section_mag_at(c, 20.0, sr) < -35.0, "the high-pass rejects two decades down", section_mag_at(c, 20.0, sr), -35.0);
    omx_eq_design_f(OMX_EQ_LOWPASS, 2000.0, M_SQRT1_2, 0.0, sr, c);
    ok(fabs(section_mag_at(c, 2000.0, sr) + 3.01) < 0.15, "the fitted low-pass sits at -3 dB on its corner", section_mag_at(c, 2000.0, sr), -3.01);
    ok(fabs(section_mag_at(c, 50.0, sr)) < 0.05, "the low-pass is unity at DC", section_mag_at(c, 50.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_NOTCH, 1000.0, 4.0, 0.0, sr, c);
    ok(section_mag_at(c, 1000.0, sr) < -40.0, "the notch nulls its centre", section_mag_at(c, 1000.0, sr), -40.0);
    ok(fabs(section_mag_at(c, 100.0, sr)) < 0.05, "the notch is unity a decade below", section_mag_at(c, 100.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_LOWSHELF, 300.0, M_SQRT1_2, -9.0, sr, c);
    ok(fabs(section_mag_at(c, 20.0, sr) + 9.0) < 0.1, "the low shelf carries its gain at DC", section_mag_at(c, 20.0, sr), -9.0);
    omx_eq_design_f(OMX_EQ_HIGHSHELF, 3000.0, M_SQRT1_2, 4.0, sr, c);
    ok(fabs(section_mag_at(c, 30.0, sr)) < 0.05, "the high shelf is unity at DC", section_mag_at(c, 30.0, sr), 0.0);
    ok(fabs(section_mag_at(c, sr * 0.45, sr) - 4.0) < 0.15, "the high shelf carries its gain near Nyquist", section_mag_at(c, sr * 0.45, sr), 4.0);
    /* the double design and its float narrowing agree; every design keeps its poles inside */
    double d[5];
    omx_eq_design(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, d);
    omx_eq_design_f(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, c);
    for (int i = 0; i < 5; i++) ok(c[i] == (float)d[i], "the float design is the narrowed double design", c[i], d[i]);
    for (int k = 0; k <= 5; k++) {
      omx_eq_design((enum omx_eq_kind)k, 15000.0, 8.0, 12.0, sr, d);
      ok(fabs(d[3]) < 1.0 + d[4] && d[4] < 1.0, "an extreme design stays inside the unit circle", d[4], 1.0);
    }
  }
  double qs[2];
  ok(omx_eq_butterworth_qs(0, qs) == 1u && fabs(qs[0] - M_SQRT1_2) < 1e-12, "12 dB/oct is one section at 1/sqrt2", qs[0], M_SQRT1_2);
  ok(omx_eq_butterworth_qs(1, qs) == 2u && fabs(qs[0] * qs[1] - M_SQRT1_2) < 1e-6, "24 dB/oct's two Qs multiply to 1/sqrt2", qs[0] * qs[1], M_SQRT1_2);
  expect_clean();
}
