/* ---- double-precision all-pass coefficient and second-order design (dsp-primitives §1 rows
 * 4a/5; the RULED first-order lattice itself is test/kernels/allpass1.c's oracle) -------------- */

/* Ten test frequencies, log-spaced from 20 Hz to 0.45·sr — shared with xover.c (included after,
 * sorted by filename). */
static void ten_freqs(double sr, double f[10]) {
  for (int k = 0; k < 10; k++) f[k] = 20.0 * pow(0.45 * sr / 20.0, k / 9.0);
}

/* The transfer function of a normalised `{b0, b1, b2, a1, a2}` section at `w` — shared with
 * xover.c. */
static double complex section_h(const double c[5], double w) {
  const double complex z1 = cexp(-I * w), z2 = z1 * z1;
  return (c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2);
}

static void arm_allpass(void) {
  g_arm = "allpass";
  enum { IMP = 1 << 17 };
  static float h[IMP];
  static const double fcs[5] = {50.0, 500.0, 1000.0, 5000.0, 15000.0};
  double worst_mag = 0.0, worst_phase = 0.0, worst_fc = 0.0, worst_energy = 0.0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    double f[10];
    ten_freqs(sr, f);
    for (int fi = 0; fi < 5; fi++) {
      const double fc = fcs[fi], K = tan(M_PI * fc / sr);
      const float a = (float)omx_allpass1_coef_d(fc, sr);
      ok(fabs(a) < 1.0f, "the coefficient is inside unity", a, 1.0);
      struct omx_allpass1 st = {0.0f};
      memset(h, 0, sizeof h);
      h[0] = 1.0f;
      omx_allpass1_block(&st, h, IMP, a);
      for (int k = 0; k <= 10; k++) {
        const double w = 2.0 * M_PI * (k < 10 ? f[k] : fc) / sr;
        double complex H = 0.0;
        for (uint32_t n = 0; n < IMP; n++) H += h[n] * cexp(-I * w * n);
        const double phase = carg(H), want = -2.0 * atan(tan(w / 2.0) / K);
        double dphi = fabs(remainder(phase - want, 2.0 * M_PI)) * 180.0 / M_PI;
        if (fabs(cabs(H) - 1.0) > worst_mag) worst_mag = fabs(cabs(H) - 1.0);
        if (k < 10 && dphi > worst_phase) worst_phase = dphi;
        if (k == 10) {
          dphi = fabs(remainder(phase + M_PI / 2.0, 2.0 * M_PI)) * 180.0 / M_PI;
          if (dphi > worst_fc) worst_fc = dphi;
        }
      }
      /* the lossless identity over a block that starts and ends with state in the section */
      float x[777], y[777];
      for (int i = 0; i < 777; i++) x[i] = y[i] = rnd(-0.8f, 0.8f);
      struct omx_allpass1 s0 = {rnd(-0.5f, 0.5f)}, s1 = s0;
      omx_allpass1_block(&s1, y, 777u, a);
      double ein = s0.s * (double)s0.s / K, eout = s1.s * (double)s1.s / K;
      for (int i = 0; i < 777; i++) { ein += (double)x[i] * x[i]; eout += (double)y[i] * y[i]; }
      if (fabs(eout - ein) / ein > worst_energy) worst_energy = fabs(eout - ein) / ein;
      /* the block word is the per-sample word, bit for bit */
      struct omx_allpass1 s2 = s0;
      float z[777];
      for (int i = 0; i < 777; i++) z[i] = omx_allpass1_tick(&s2, x[i], a);
      ok(memcmp(y, z, sizeof z) == 0 && s1.s == s2.s, "the block word is the per-sample word, bit for bit", 0.0, 0.0);
    }
    /* the second-order design: the reversed denominator, unity at ten frequencies, −180° at fc */
    static const double qs[3] = {0.5, OMX_XOVER_LR4_SECTION_Q, 2.0};
    for (int qi = 0; qi < 3; qi++) {
      double c[5];
      omx_allpass2_design(1000.0, qs[qi], sr, c);
      ok(c[0] == c[4] && c[1] == c[3] && c[2] == 1.0, "the numerator is the reversed denominator, bit for bit", c[0] - c[4], 0.0);
      for (int k = 0; k < 10; k++) {
        const double m = cabs(section_h(c, 2.0 * M_PI * f[k] / sr));
        if (fabs(m - 1.0) > worst_mag) worst_mag = fabs(m - 1.0);
      }
      const double at = carg(section_h(c, 2.0 * M_PI * 1000.0 / sr));
      ok(fabs(fabs(at) - M_PI) < 1e-9, "the second-order all-pass is −180° at its corner", at, M_PI);
    }
  }
  ok(worst_mag < OMX_ALLPASS_UNITY_TOLERANCE, "|H| = 1 at ten frequencies, every rate", worst_mag, OMX_ALLPASS_UNITY_TOLERANCE);
  ok(worst_phase < 0.1, "the phase is -2·atan(tan(w/2)/tan(π·fc/sr)) at ten frequencies to 0.1°", worst_phase, 0.1);
  ok(worst_fc < 0.1, "the section is −90° at fc to 0.1°", worst_fc, 0.1);
  ok(worst_energy < OMX_ALLPASS_UNITY_TOLERANCE, "a block conserves Σx² + P·s² with P = 1/tan(π·fc/sr)", worst_energy, OMX_ALLPASS_UNITY_TOLERANCE);
  printf("allpass: |H|-1 worst %.3g, phase worst %.3g deg, at fc %.3g deg, block energy worst %.3g\n", worst_mag,
         worst_phase, worst_fc, worst_energy);
  expect_clean();
}
