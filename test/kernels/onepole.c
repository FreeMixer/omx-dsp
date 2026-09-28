
/* ---- one-pole ------------------------------------------------------------------------------- */

static void arm_onepole(void) {
  g_arm = "onepole";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    /* time constant: the step response covers 1 - 1/e after tau */
    const float tau_ms = 5.0f;
    const float p = omx_pole_from_time_ms(tau_ms, sr);
    ok(p > 0.0f && p < 1.0f, "a positive time gives a pole inside (0, 1)", p, 1.0);
    float y = 0.0f;
    const uint32_t n_tau = (uint32_t)(tau_ms * 0.001f * sr + 0.5f);
    for (uint32_t i = 0; i < n_tau; i++) omx_onepole(&y, 1.0f, p);
    ok(fabsf(y - (1.0f - expf(-1.0f))) < 2e-3f, "the step response covers 1-1/e after one time constant", y, 1.0 - exp(-1.0));
    /* corner: |H| at fc for the impulse-invariant pole is (1-p)/sqrt(1 - 2p cos w + p^2) */
    const float fc = 1000.0f;
    const float pc = omx_pole_from_cutoff_hz(fc, sr);
    const double w = 2.0 * M_PI * fc / sr;
    const double closed = (1.0 - pc) / sqrt(1.0 - 2.0 * pc * cos(w) + (double)pc * pc);
    float st = 0.0f;
    double re = 0.0, im = 0.0;
    const uint32_t settle = (uint32_t)(sr * 0.2f), meas = (uint32_t)(sr * 0.1f);
    for (uint32_t i = 0; i < settle + meas; i++) {
      const float x = sinf((float)(w * i));
      const float o = omx_onepole(&st, x, pc);
      if (i >= settle) { re += o * cos(w * i); im += o * sin(w * i); }
    }
    const double mag = 2.0 * sqrt(re * re + im * im) / meas;
    ok(fabs(mag - closed) < 2e-3, "the corner's magnitude matches the closed form", mag, closed);
    ok(fabs(20.0 * log10(closed) + 3.0) < 0.2, "the corner sits near -3 dB", 20.0 * log10(closed), -3.0);
    /* wires and exactness */
    ok(omx_pole_from_time_ms(0.0f, sr) == 0.0f, "ms <= 0 is a wire", omx_pole_from_time_ms(0.0f, sr), 0.0);
    ok(omx_pole_from_cutoff_hz(-1.0f, sr) == 0.0f, "hz <= 0 is a wire", omx_pole_from_cutoff_hz(-1.0f, sr), 0.0);
    float wire = 0.25f;
    ok(omx_onepole(&wire, 0.75f, 0.0f) == 0.75f, "pole 0 passes the input through", wire, 0.75);
    float held = 0.3125f;
    omx_onepole_toward(&held, 0.3125f, p);
    ok(held == 0.3125f, "the increment form holds a state equal to its target bit for bit", held, 0.3125);
    float conv = 0.0f, incr = 0.0f;
    for (int i = 0; i < 100; i++) { omx_onepole(&conv, 0.8f, p); omx_onepole_toward(&incr, 0.8f, p); }
    ok(fabsf(conv - incr) < 1e-5f, "both forms are the same filter", conv, incr);
  }
  expect_clean();
}
