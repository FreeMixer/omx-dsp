
/* ---- the oversampler ------------------------------------------------------------------------ */

static void arm_oversampler(void) {
  g_arm = "oversampler";
  /* the table: the centre and the odd taps sum to unity at DC; the dot is that sum on a DC window */
  double dc = OMX_HALFBAND_CENTER;
  for (int i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) dc += 2.0 * OMX_HALFBAND_ODD_COEF[i];
  ok(fabs(dc - 1.0) < 1e-6, "the half-band is unity at DC", dc, 1.0);
  float win[4 * OMX_HALFBAND_ODD_TAPS - 1];
  for (int i = 0; i < 4 * OMX_HALFBAND_ODD_TAPS - 1; i++) win[i] = 0.5f;
  ok(fabsf(omx_halfband_dot(win + 2 * OMX_HALFBAND_ODD_TAPS - 1) - 0.5f) < 1e-6f, "the dot over a DC window is the level", omx_halfband_dot(win + 2 * OMX_HALFBAND_ODD_TAPS - 1), 0.5);
  ok(omx_oversampler_latency_for(1u) == 0u && omx_oversampler_latency_for(2u) == 48u && omx_oversampler_latency_for(4u) == 72u, "the declared latencies", omx_oversampler_latency_for(4u), 72.0);
  ok(omx_oversampler_latency_for(4u) == OMX_OVS_LATENCY_4X, "the macro and the function agree", OMX_OVS_LATENCY_4X, 72.0);
  ok(omx_oversampler_state_size() == sizeof(struct omx_oversampler), "the state layout is exported", (double)omx_oversampler_state_size(), sizeof(struct omx_oversampler));
  for (uint32_t factor = 1u; factor <= 4u; factor *= 2u) {
    struct omx_oversampler o;
    omx_oversampler_init(&o, factor);
    ok(o.factor == factor && omx_oversampler_latency(&o) == omx_oversampler_latency_for(factor), "init arms the factor", o.factor, factor);
    for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
      const float sr = OMX_DECLARED_RATES[ri];
      /* round trip of a tone: the output is the input delayed by the declared latency, within
       * the half-band's passband ripple; measured over a second of audio in uneven blocks */
      const uint32_t total = 4096u, lat = omx_oversampler_latency_for(factor);
      static float x[4096], up[4096 * 4], back[4096];
      const double w = 2.0 * M_PI * 1000.0 / sr;
      for (uint32_t i = 0; i < total; i++) x[i] = 0.5f * sinf((float)(w * i));
      omx_oversampler_init(&o, factor);
      uint32_t done = 0u;
      const uint32_t blocks[] = {64u, 37u, 128u, 1u, 256u, 100u};
      uint32_t bi = 0u;
      while (done < total) {
        uint32_t n = blocks[bi++ % 6u];
        if (n > total - done) n = total - done;
        omx_oversampler_up(&o, x + done, n, up);
        omx_oversampler_down(&o, up, n, back + done);
        done += n;
      }
      double worst = 0.0;
      for (uint32_t i = lat + 512u; i < total; i++) { const double e = fabs((double)back[i] - (double)x[i - lat]); if (e > worst) worst = e; }
      ok(worst < 0.5 * 0.0025, "up then down is the input delayed by the declared latency within the ripple", worst, 0.00125);
      /* quantum invariance: the same signal in one block equals the uneven blocks bit for bit */
      static float once[4096];
      omx_oversampler_init(&o, factor);
      omx_oversampler_up(&o, x, total, up);
      omx_oversampler_down(&o, up, total, once);
      ok(memcmp(once, back, sizeof once) == 0, "the block size does not touch a bit of the output", 0.0, 0.0);
      if (factor == 1u) ok(memcmp(once, x, sizeof once) == 0, "factor 1 is a copy", 0.0, 0.0);
      /* the interpolated tone is the tone: its level at the higher rate is the level in */
      if (factor > 1u) {
        omx_oversampler_init(&o, factor);
        omx_oversampler_up(&o, x, total, up);
        float peak = 0.0f;
        for (uint32_t i = 1024u; i < total * factor; i++) if (fabsf(up[i]) > peak) peak = fabsf(up[i]);
        ok(fabsf(peak - 0.5f) < 0.5f * 0.003f, "the interpolated tone keeps its level", peak, 0.5);
      }
    }
  }
  expect_clean();
}
