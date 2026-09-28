/* ---- true peak ------------------------------------------------------------------------------ */

static void arm_truepeak(void) {
  g_arm = "truepeak";
  enum { N = 2048 };
  static float x[N], p[N];
  const uint32_t u = omx_truepeak_delay();
  ok(u == 36u, "the reading lags by the x4 interpolator's group delay", u, 36.0);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    struct omx_truepeak t;
    /* an impulse reads its peak exactly U frames later */
    memset(x, 0, sizeof x);
    x[100] = 0.9f;
    omx_truepeak_init(&t);
    omx_truepeak_block(&t, x, N, p);
    uint32_t at = 0u;
    for (uint32_t i = 0; i < N; i++) if (p[i] > p[at]) at = i;
    ok(at == 100u + u, "an impulse's reading lands U frames late", (double)at, 100.0 + u);
    ok(fabsf(p[at] - 0.9f) < 1e-6f, "an impulse reads its own level", p[at], 0.9);
    /* a DC block reads its own level */
    for (int i = 0; i < N; i++) x[i] = 0.5f;
    omx_truepeak_init(&t);
    omx_truepeak_block(&t, x, N, p);
    double worst = 0.0;
    for (int i = 512; i < N; i++) worst = fmax(worst, fabs(p[i] - 0.5));
    ok(worst < 1e-6, "a DC block reads its own level", worst, 1e-6);
    /* the inter-sample peak: a tone at rate/4, phase pi/4, samples at A/sqrt2, true peak A */
    const float a = 0.8f;
    for (int i = 0; i < N; i++) x[i] = a * sinf((float)(M_PI / 2.0) * (float)(i % 4) + (float)(M_PI / 4.0));
    omx_truepeak_init(&t);
    uint32_t done = 0u;
    const uint32_t blocks[] = {64u, 37u, 128u, 1u, 256u, 100u};
    for (uint32_t bi = 0; done < N; bi++) {
      uint32_t n = blocks[bi % 6u];
      if (n > N - done) n = N - done;
      omx_truepeak_block(&t, x + done, n, p + done);
      done += n;
    }
    double lo = 1e9, hi = 0.0, sample_peak = 0.0;
    for (int i = 512; i < N; i++) { lo = fmin(lo, p[i]); hi = fmax(hi, p[i]); sample_peak = fmax(sample_peak, fabsf(x[i])); }
    ok(fabs(hi - a) < a * 0.0023, "the inter-sample peak reads the tone's level within 0.02 dB", hi, a);
    ok(fabs(hi / sample_peak - M_SQRT2) < M_SQRT2 * 0.0023, "the true peak reads the closed-form overshoot, sqrt 2, above the sample peak", hi / sample_peak, M_SQRT2);
    ok(fabs(lo - sample_peak) < sample_peak * 1e-6, "a frame whose interval holds no crest reads the sample peak", lo, sample_peak);
  }
  ok(omx_truepeak_state_size() == sizeof(struct omx_truepeak), "the state layout is exported", (double)omx_truepeak_state_size(), sizeof(struct omx_truepeak));
  expect_clean();
}
