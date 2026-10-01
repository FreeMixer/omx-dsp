// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the fractional delay line -------------------------------------------------------------- */

static void arm_fdelay(void) {
  g_arm = "fdelay";
  static float ring[4096];
  for (int order = 3; order <= 5; order += 2) {
    /* the kernel is unity at DC at every fraction, and the identity at a zero fraction */
    for (int fi = 0; fi < 50; fi++) {
      float c[OMX_FDELAY_MAX_TAPS];
      omx_fdelay_lagrange(order, (float)fi / 50.0f, c);
      double sum = 0.0;
      for (int k = 0; k <= order; k++) sum += c[k];
      ok(fabs(sum - 1.0) < 1e-5, "the kernel sums to one", sum, 1.0);
      if (fi == 0) ok(c[omx_fdelay_lookbehind(order)] == 1.0f, "a zero fraction is the identity tap", c[omx_fdelay_lookbehind(order)], 1.0);
    }
    /* The kernel IS its documented product, bit for bit, whatever instantiation computes it (the
     * order-3 instantiation a constant order lets the compiler unroll is the chorus lane's cost
     * change, and it must not move one bit): c[k] = (float)(Π(j≠k)(f − (j − off)) / Π(j≠k)(k − j))
     * in double, left to right, over 2^20 grid fractions and 2^16 scattered ones. */
    {
      const int off = (int)omx_fdelay_lookbehind(order);
      long diff = 0, n = 0;
      uint32_t seed = 12345u;
      for (uint32_t i = 0; i < (1u << 20) + (1u << 16); i++) {
        float f;
        if (i < (1u << 20)) f = (float)i / (float)(1u << 20);
        else { seed = seed * 1664525u + 1013904223u; f = (float)(seed >> 8) / 16777216.0f; }
        float c[OMX_FDELAY_MAX_TAPS], want[OMX_FDELAY_MAX_TAPS];
        omx_fdelay_lagrange(order, f, c);
        for (int k = 0; k <= order; k++) {
          double num = 1.0, den = 1.0;
          for (int j = 0; j <= order; j++) {
            if (j == k) continue;
            num *= (double)f - (double)(j - off);
            den *= (double)(k - j);
          }
          want[k] = (float)(num / den);
        }
        n++;
        if (memcmp(c, want, (size_t)(order + 1) * sizeof(float)) != 0) diff++;
      }
      ok(diff == 0, "the kernel is its documented product, bit for bit", (double)diff, 0.0);
      printf("omxdsp_suite fdelay: order %d, %ld/%ld fractions bit-identical to the product\n", order, n - diff, n);
    }
    struct omx_fdelay l;
    ok(omx_fdelay_init(&l, ring, 4096u, order) == OMX_FDELAY_OK, "a legal order arms", order, 0.0);
    ok(l.order == order && l.cap == 4096u, "an armed line carries what it was given", l.order, order);
    /* a whole-sample delay is an exact copy, for any input, at every declared rate's block */
    for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
      const float sr = OMX_DECLARED_RATES[ri];
      memset(ring, 0, sizeof ring);
      l.wpos = 0u;
      const uint32_t n = 256u, d = (uint32_t)(sr * 0.001f); /* 1 ms */
      float x[512], y[512];
      for (uint32_t i = 0; i < 512u; i++) { x[i] = rnd(-1.0f, 1.0f) * (i % 7 == 0 ? 1e-25f : 1.0f); y[i] = x[i]; }
      omx_fdelay_process(y, 512u, &l, (float)d);
      int exact = 1;
      for (uint32_t i = d; i < 512u; i++) if (y[i] != x[i - d]) exact = 0;
      ok(exact, "a whole-sample delay is bit-exact, below the flush floor too", d, 0.0);
      (void)n;
      /* a fractional delay of a tone reads the tone at the delayed phase within the kernel's loss */
      memset(ring, 0, sizeof ring); l.wpos = 0u;
      const float delay = 10.5f;
      const double w = 2.0 * M_PI * 1000.0 / sr;
      double worst = 0.0;
      for (uint32_t i = 0; i < 2000u; i++) {
        const float out = omx_fdelay_tick(&l, sinf((float)(w * i)), delay);
        if (i > 100u) { const double want = sin(w * ((double)i - delay)); if (fabs(out - want) > worst) worst = fabs(out - want); }
      }
      ok(worst < 2e-3, "a fractional read of a 1 kHz tone lands on the delayed phase", worst, 2e-3);
      /* latency: the clamp's answer is what the line delivers */
      ok(omx_fdelay_latency(&l, -3.0f) == 0.0f, "a negative request delivers 0", omx_fdelay_latency(&l, -3.0f), 0.0);
      ok(omx_fdelay_latency(&l, 1e9f) == omx_fdelay_max_delay(4096u, order), "a huge request is clamped to the ring", omx_fdelay_latency(&l, 1e9f), omx_fdelay_max_delay(4096u, order));
      ok(omx_fdelay_latency(&l, 0.3f) == omx_fdelay_min_delay(order), "a fraction below the reach is raised to the shortest whole delay", omx_fdelay_latency(&l, 0.3f), omx_fdelay_min_delay(order));
    }
  }
  ok(omx_fdelay_cap_for(100.0f, 5) == 100u + 2u + 2u, "the cap is the delay plus the reach plus the slot", omx_fdelay_cap_for(100.0f, 5), 104.0);
  ok(omx_fdelay_max_delay(omx_fdelay_cap_for(100.0f, 3), 3) >= 100.0f, "a cap sized for a delay serves it", omx_fdelay_max_delay(omx_fdelay_cap_for(100.0f, 3), 3), 100.0);
  ok(omx_fdelay_state_size() == sizeof(struct omx_fdelay), "the state layout is exported", (double)omx_fdelay_state_size(), sizeof(struct omx_fdelay));
  expect_clean();
}
