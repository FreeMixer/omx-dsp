/* ---- first-order all-pass (dsp-primitives §1 row 4; phaser spec §7 arm A) ------------------- */

/* The impulse response of one section at one coefficient, until its (−a)ⁿ tail is below 1e-13. */
static uint32_t allpass1_impulse(float a, double *h, uint32_t cap) {
  struct omx_allpass1 st = {0.0f};
  uint32_t n = 0;
  for (; n < cap; n++) {
    h[n] = omx_allpass1_tick(&st, n == 0 ? 1.0f : 0.0f, a);
    if (n > 16 && fabs(h[n]) < 1e-13 && fabs(h[n - 1]) < 1e-13) return n + 1;
  }
  return n;
}

static void arm_allpass1(void) {
  g_arm = "allpass1";
  enum { CAP = 1 << 18 };
  static double h[CAP];
  static const float FCS[] = {20.0f, 200.0f, 3200.0f, 16000.0f};
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (uint32_t fi = 0; fi < 4u; fi++) {
      const float fc = FCS[fi];
      const float a = omx_allpass1_coef(fc, sr);
      ok(a > -1.0f && a < 1.0f, "the coefficient is inside (-1, 1)", a, 1.0);
      const uint32_t len = allpass1_impulse(a, h, CAP);
      ok(len < CAP, "the impulse response decays below 1e-13 inside the buffer", len, CAP);
      const double t = tan(M_PI * (double)fc / (double)sr);
      double worst_mag = 0.0, worst_ph = 0.0;
      for (int k = 0; k <= 10; k++) {
        const double f = k == 10 ? (double)fc : (0.02 + 0.045 * k) * (double)sr;
        const double w = 2.0 * M_PI * f / (double)sr;
        double re = 0.0, im = 0.0;
        for (uint32_t n = 0; n < len; n++) { re += h[n] * cos(w * n); im -= h[n] * sin(w * n); }
        const double mag = sqrt(re * re + im * im);
        const double closed = -2.0 * atan(tan(0.5 * w) / t);
        double d = atan2(im, re) - closed;
        while (d > M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        if (fabs(mag - 1.0) > worst_mag) worst_mag = fabs(mag - 1.0);
        if (fabs(d) > worst_ph) worst_ph = fabs(d);
        if (k == 10) ok(fabs(atan2(im, re) + 0.5 * M_PI) < 5e-5, "the phase at fc is -90 degrees", atan2(im, re), -0.5 * M_PI);
      }
      const double mag_tol = 4.0 * ldexp(1.0, -24) / sqrt(1.0 - (double)a * a);
      ok(worst_mag < mag_tol, "|A| = 1 across the band, to the recurrence's float32 round-off", worst_mag, mag_tol);
      ok(worst_ph < 5e-5, "the phase is -2 atan(tan(w/2)/t)", worst_ph, 5e-5);
    }
    struct omx_allpass1 st = {0.0f};
    float buf[1024];
    for (int i = 0; i < 1024; i++) buf[i] = rnd(-0.5f, 0.5f);
    for (int b = 0; b < 4; b++) omx_allpass1_block(&st, buf, 1024u, omx_allpass1_coef(800.0f, sr));
    struct omx_allpass1 chain[6] = {{0.0f}}, solo[6] = {{0.0f}};
    const float a800 = omx_allpass1_coef(800.0f, sr);
    int same = 1;
    for (int i = 0; i < 512; i++) {
      const float x = rnd(-0.5f, 0.5f);
      float v = x;
      for (int k = 0; k < 6; k++) v = omx_allpass1_tick(&solo[k], v, a800);
      same &= omx_allpass1_cascade(chain, 6u, x, a800) == v;
    }
    ok(same, "the cascade is its sections in series, bit for bit", same, 1.0);
    struct omx_allpass1 quiet = {0.0f};
    omx_allpass1_tick(&quiet, 1.0f, a800);
    int flushed = 1;
    for (int i = 0; i < 200000; i++) {
      omx_allpass1_tick(&quiet, 0.0f, a800);
      flushed &= quiet.s == 0.0f || fabsf(quiet.s) >= 1e-20f;
    }
    ok(flushed && quiet.s == 0.0f, "a silent section decays to an exact zero, never a subnormal", quiet.s, 0.0);
  }
  ok(omx_allpass1_state_align() > 0u && (omx_allpass1_state_align() & (omx_allpass1_state_align() - 1u)) == 0u,
     "the section's alignment is a power of two", (double)omx_allpass1_state_align(), 0.0);
  expect_clean();
  struct omx_allpass1 bad = {0.0f};
  omx_allpass1_tick(&bad, 0.5f, 1.0f);
  ok(omx_contract_log.count == 1u && strcmp(omx_contract_log.rec[0].token, "coefficient-inside-unity") == 0,
     "a coefficient on the unit circle is refused by the precondition", omx_contract_log.count, 1.0);
  omx_contract_reset();
  omx_allpass1_coef(30000.0f, 48000.0f);
  ok(omx_contract_log.count >= 1u && strcmp(omx_contract_log.rec[0].token, "fc-inside-nyquist") == 0,
     "a crossing above Nyquist is refused by the precondition first", omx_contract_log.count, 1.0);
  omx_contract_reset();
}
