// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- the tilt (omx_tilt.h): a first-order shelf pair about one pivot ---------------------- */

/* The tilt's measured magnitude at `f`: the DFT of its impulse response, run long enough to decay. */
static double tilt_measured(const struct omx_tilt *t, double f, double sr, uint32_t len) {
  double s[4] = {0.0, 0.0, 0.0, 0.0}, re = 0.0, im = 0.0;
  const double w = 2.0 * M_PI * f / sr;
  for (uint32_t n = 0; n < len; n++) {
    const double h = omx_tilt_tick(t, n == 0 ? 1.0f : 0.0f, s);
    re += h * cos(w * n);
    im -= h * sin(w * n);
  }
  return sqrt(re * re + im * im);
}

static void arm_tilt(void) {
  g_arm = "tilt";
  static const float RATES[] = OMX_ORACLE_FULL_RATES_INIT;
  static const double SLOPES[] = {-6.0, -3.0, -1.5, 1.0, 4.5, 6.0};
  static const double PIVOTS[] = {200.0, 1000.0, 5000.0};
  double worst = 0.0;
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const double sr = RATES[ri];
    for (uint32_t pi = 0; pi < 3u; pi++)
      for (uint32_t si = 0; si < 6u; si++) {
        const double a = omx_tilt_a_of_slope(SLOPES[si]);
        struct omx_tilt t;
        omx_tilt_design(&t, a, PIVOTS[pi], sr);
        /* The closed form at four frequencies, the pivot, DC and Nyquist. */
        const double fs[4] = {0.25 * PIVOTS[pi], 0.5 * PIVOTS[pi], 2.0 * PIVOTS[pi], 0.3 * sr};
        for (int k = 0; k < 4; k++) {
          const double got = tilt_measured(&t, fs[k], sr, (uint32_t)sr);
          const double want = omx_tilt_magnitude(a, PIVOTS[pi], fs[k], sr);
          const double o = tan(M_PI * fs[k] / sr) / tan(M_PI * PIVOTS[pi] / sr);
          const double closed = sqrt((a * a * o * o + 1.0) / (o * o + a * a));
          worst = fmax(worst, fabs(20.0 * log10(got / closed)));
          ok(fabs(20.0 * log10(got / closed)) < 1e-4, "|H| is the prototype's at the prewarped frequency (1e-4 dB)", got, closed);
          ok(want == closed, "omx_tilt_magnitude is the closed form", want, closed);
        }
        const double piv = tilt_measured(&t, PIVOTS[pi], sr, (uint32_t)sr);
        ok(fabs(20.0 * log10(piv)) < 1e-4, "unity at the pivot (1e-4 dB)", 20.0 * log10(piv), 0.0);
        const double dc = (t.c[0] + t.c[1]) / (1.0 + t.c[3]), ny = (t.c[0] - t.c[1]) / (1.0 - t.c[3]);
        ok(fabs(dc * a - 1.0) < 1e-12 && fabs(ny / a - 1.0) < 1e-9, "1/a at DC and a at Nyquist", dc * a, ny / a);
        /* The slope at the pivot, from the closed form a semitone either side: the knob's dB per octave. */
        const double up = omx_tilt_magnitude(a, PIVOTS[pi], PIVOTS[pi] * pow(2.0, 1.0 / 1200.0), 1e12);
        const double dn = omx_tilt_magnitude(a, PIVOTS[pi], PIVOTS[pi] * pow(2.0, -1.0 / 1200.0), 1e12);
        const double slope = 20.0 * log10(up / dn) * 600.0;
        ok(fabs(slope - SLOPES[si]) < 1e-3, "the prototype's slope at the pivot is the knob's, dB per octave", slope, SLOPES[si]);
      }
    /* 0 dB per octave is the wire, bit for bit. */
    struct omx_tilt flat;
    omx_tilt_design(&flat, omx_tilt_a_of_slope(0.0), 1000.0, sr);
    double s[4] = {0.0, 0.0, 0.0, 0.0};
    int same = 1;
    for (int i = 0; i < 4096; i++) {
      const float x = rnd(-1.0f, 1.0f);
      same &= omx_tilt_tick(&flat, x, s) == x;
    }
    ok(same, "a flat tilt is the identity, bit for bit", same, 1.0);
  }
  ok(omx_tilt_a_of_slope(NAN) == 1.0 && omx_tilt_a_of_slope(1e9) == omx_tilt_a_of_slope(0.9999 * OMX_TILT_OCTAVE_DB),
     "a NaN slope is flat, a slope past the asymptote is held inside it", omx_tilt_a_of_slope(1e9), 0.0);
  printf("  tilt: worst |H| against the closed form %.2e dB over nine rates\n", worst);
  expect_clean();
}
