// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the oversampler ------------------------------------------------------------------------ */

/* ---- the output-parallel passes are the scalar passes ---------------------------------------
 * omx_halfband_decimate computes sixteen outputs at once and the interpolating pass does the same;
 * neither may change a bit, each output's taps being summed in the dot's own order. Held two ways:
 * (1) the run against omx_halfband_dot per output, every length 0..80 at four alignments of the
 * window; (2) the whole element, x2 and x4, up then down, streamed in blocks of 1..67 samples,
 * against the scalar passes re-spelled here verbatim -- memcmp, never a tolerance. Both at every
 * declared rate: the signal is band-limited differently at each, so a rate is a different input. */
struct ovs_ref { uint32_t stages; float up[2][OMX_OVS_UP_HIST]; float down[2][OMX_OVS_DOWN_HIST]; };

static void ovs_ref_up2(float *hist, const float *in, uint32_t n, float *out) {
  for (uint32_t i = 0; i < n; i++) {
    float win[OMX_OVS_UP_HIST + 1u];
    memcpy(win, hist, OMX_OVS_UP_HIST * sizeof *win);
    win[OMX_OVS_UP_HIST] = in[i];
    const float *c = win + OMX_OVS_UP_HIST - OMX_OVS_UP_DELAY;
    out[2u * i] = 2.0f * OMX_HALFBAND_CENTER * c[0];
    float acc = 0.0f;
    for (uint32_t j = 0; j < OMX_HALFBAND_ODD_TAPS; j++) acc += OMX_HALFBAND_ODD_COEF[j] * (c[-(int)j] + c[j + 1u]);
    out[2u * i + 1u] = 2.0f * acc;
    memcpy(hist, win + 1, OMX_OVS_UP_HIST * sizeof *hist);
  }
}

static void ovs_ref_down2(float *hist, const float *in, uint32_t n_out, float *out) {
  for (uint32_t o = 0; o < n_out; o++) {
    float win[OMX_OVS_DOWN_HIST + 2u];
    memcpy(win, hist, OMX_OVS_DOWN_HIST * sizeof *win);
    win[OMX_OVS_DOWN_HIST] = in[2u * o];
    win[OMX_OVS_DOWN_HIST + 1u] = in[2u * o + 1u];
    const float *c = win + OMX_OVS_DOWN_HIST - OMX_OVS_DOWN_DELAY;
    float acc = OMX_HALFBAND_CENTER * c[0];
    for (uint32_t i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) {
      const int32_t k = (int32_t)(2u * i + 1u);
      acc += OMX_HALFBAND_ODD_COEF[i] * (c[-k] + c[k]);
    }
    out[o] = acc;
    memcpy(hist, win + 2, OMX_OVS_DOWN_HIST * sizeof *hist);
  }
}

static void ovs_ref_updown(struct ovs_ref *r, const float *in, uint32_t n, float *up, float *out) {
  if (r->stages == 1u) { ovs_ref_up2(r->up[0], in, n, up); ovs_ref_down2(r->down[0], up, n, out); return; }
  float mid[2u * 80u];
  ovs_ref_up2(r->up[0], in, n, mid);
  ovs_ref_up2(r->up[1], mid, 2u * n, up);
  ovs_ref_down2(r->down[0], up, 2u * n, mid);
  ovs_ref_down2(r->down[1], mid, n, out);
}

/* A broadband, deterministic test signal at sample rate sr: two tones plus a hashed dither. */
static void ovs_gen(float *x, uint32_t n, double sr) {
  for (uint32_t i = 0; i < n; i++)
    x[i] = 0.4f * sinf((float)(2.0 * M_PI * 997.0 * i / sr)) + 0.3f * sinf((float)(2.0 * M_PI * 0.31 * sr / 2.0 * i / sr)) +
           (float)((i * 2654435761u) >> 20) * 1e-4f;
}

static void arm_oversampler_parallel(float sr) {
  static float buf[4 + 2u * 80u + 4u * OMX_HALFBAND_ODD_TAPS + 8u];
  ovs_gen(buf, sizeof buf / sizeof buf[0], sr);
  int run_equal = 1;
  for (uint32_t align = 0; align < 4u; align++)
    for (uint32_t n = 0; n <= 80u; n++) {
      const float *c = buf + align + 2u * OMX_HALFBAND_ODD_TAPS - 1u;
      float got[81], want[81];
      memset(got, 0xA5, sizeof got);
      memcpy(want, got, sizeof want);
      omx_halfband_decimate(c, n, got);
      for (uint32_t o = 0; o < n; o++) want[o] = omx_halfband_dot(c + 2u * o);
      if (memcmp(got, want, sizeof got) != 0) run_equal = 0;
    }
  ok(run_equal, "omx_halfband_decimate is bit-identical to omx_halfband_dot at every length 0..80 and 4 alignments", 0.0, 0.0);

  for (uint32_t factor = 2u; factor <= 4u; factor += 2u) {
    struct omx_oversampler o;
    omx_oversampler_init(&o, factor);
    struct ovs_ref r;
    memset(&r, 0, sizeof r);
    r.stages = factor == 4u ? 2u : 1u;
    static float x[4096];
    ovs_gen(x, 4096u, sr);
    int equal = 1;
    uint32_t done = 0u, blk = 1u;
    while (done + 80u <= 4096u) {
      float up_a[4u * 80u], up_b[4u * 80u], out_a[80], out_b[80];
      omx_oversampler_up(&o, x + done, blk, up_a);
      omx_oversampler_down(&o, up_a, blk, out_a);
      ovs_ref_updown(&r, x + done, blk, up_b, out_b);
      if (memcmp(up_a, up_b, factor * blk * sizeof(float)) != 0 || memcmp(out_a, out_b, blk * sizeof(float)) != 0) equal = 0;
      done += blk;
      blk = blk % 67u + 1u;
    }
    ok(equal, "up AND down are bit-identical to the scalar passes, streamed in blocks of 1..67", (double)factor, sr);
  }
}

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
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) arm_oversampler_parallel(OMX_DECLARED_RATES[ri]);
  expect_clean();
}
