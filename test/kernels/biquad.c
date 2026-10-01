// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- biquad --------------------------------------------------------------------------------- */

static void arm_biquad(void) {
  g_arm = "biquad";
  const float unity[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float s[4] = {0};
  float in[64], out[64];
  for (int i = 0; i < 64; i++) { in[i] = rnd(-1.0f, 1.0f); out[i] = omx_biquad(in[i], unity, s); }
  ok(memcmp(in, out, sizeof in) == 0, "unity coefficients are the identity bit for bit", 0.0, 0.0);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    /* a cookbook low-pass at 1 kHz, Q 1/sqrt2, designed here in double; the section's impulse
     * response against the same recursion run in double */
    const double w0 = 2.0 * M_PI * 1000.0 / sr, alpha = sin(w0) / (2.0 * M_SQRT1_2), cw = cos(w0), a0 = 1.0 + alpha;
    const float c[5] = {(float)((1.0 - cw) / 2.0 / a0), (float)((1.0 - cw) / a0), (float)((1.0 - cw) / 2.0 / a0),
                        (float)(-2.0 * cw / a0), (float)((1.0 - alpha) / a0)};
    float st[4] = {0};
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0, worst = 0.0;
    for (int i = 0; i < 2000; i++) {
      const float x = i == 0 ? 1.0f : 0.0f;
      const float y = omx_biquad(x, c, st);
      const double yd = c[0] * x + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
      x2 = x1; x1 = x; y2 = y1; y1 = yd;
      if (fabs(yd - y) > worst) worst = fabs(yd - y);
    }
    ok(worst < 1e-5, "the section's impulse response follows its recursion", worst, 1e-5);
    ok(omx_block_finite(st, 4u), "the state stays finite", 0.0, 1.0);
    /* the cascade equals the sections run in series, and a parked section leaves its state alone */
    float coeffs[3][5]; memcpy(coeffs[0], c, sizeof c); memcpy(coeffs[1], c, sizeof c); memcpy(coeffs[2], c, sizeof c);
    float sa[3][4] = {{0}}, sb1[4] = {0}, sb2[4] = {0};
    const uint8_t on[3] = {1, 0, 1};
    float a[128], b[128];
    for (int i = 0; i < 128; i++) { a[i] = rnd(-0.7f, 0.7f); b[i] = a[i]; }
    omx_biquad_cascade(a, 128u, 3u, coeffs, on, sa);
    for (int i = 0; i < 128; i++) b[i] = omx_biquad(omx_biquad(b[i], c, sb1), c, sb2);
    ok(memcmp(a, b, sizeof a) == 0, "the cascade is its sections in series, bit for bit", 0.0, 0.0);
    ok(sa[1][0] == 0.0f && sa[1][2] == 0.0f, "a parked section's state does not advance", sa[1][0], 0.0);
    ok(memcmp(sa[0], sb1, sizeof sb1) == 0 && memcmp(sa[2], sb2, sizeof sb2) == 0, "each section keeps its own slot", 0.0, 0.0);
    /* a 0 dB cascade is the identity over a block */
    float d[64]; for (int i = 0; i < 64; i++) d[i] = rnd(-1.0f, 1.0f);
    float e[64]; memcpy(e, d, sizeof d);
    float un[2][5] = {{1, 0, 0, 0, 0}, {1, 0, 0, 0, 0}}; float su[2][4] = {{0}};
    omx_biquad_cascade(e, 64u, 2u, un, NULL, su);
    ok(memcmp(d, e, sizeof d) == 0, "a unity cascade is the identity", 0.0, 0.0);
  }
  {
    /* THE COST DOOR'S ORACLE (spec §1 row 2): omx_biquad_cascade_stereo — parked bands dropped
     * once, two sections and both legs per pass, state in locals — is the band-outer loop of
     * omx_biquad over the live bands, BIT FOR BIT, output and state, for every band count to the
     * cap, odd and even live counts, one and two legs, and block lengths 1, 7, 64 and 513. */
    static float cf[OMX_EQ_MAX_BANDS][5];
    for (int b = 0; b < OMX_EQ_MAX_BANDS; b++)
      omx_eq_design_f((enum omx_eq_kind)(b % 6), 40.0 * pow(400.0, b / (double)(OMX_EQ_MAX_BANDS - 1)), 0.5 + 0.1 * b,
                      (b & 1) ? 6.0 : -9.0, 96000.0, cf[b]);
    static const uint32_t ns[4] = {1u, 7u, 64u, 513u};
    int bad = 0, cases = 0;
    for (uint32_t nb = 0; nb <= OMX_EQ_MAX_BANDS; nb++)
      for (int mask = 0; mask < 4; mask++)
        for (int ni = 0; ni < 4; ni++)
          for (int legs = 1; legs <= 2; legs++) {
            const uint32_t n = ns[ni];
            uint8_t en[OMX_EQ_MAX_BANDS];
            for (uint32_t b = 0; b < OMX_EQ_MAX_BANDS; b++)
              en[b] = mask == 0 ? 1 : mask == 1 ? (b % 3 != 1) : mask == 2 ? (b & 1) : (b * 7 % 5 < 2);
            static float l0[513], r0[513], l1[513], r1[513];
            float sl0[OMX_EQ_MAX_BANDS][4], sr0[OMX_EQ_MAX_BANDS][4], sl1[OMX_EQ_MAX_BANDS][4], sr1[OMX_EQ_MAX_BANDS][4];
            for (uint32_t b = 0; b < OMX_EQ_MAX_BANDS; b++)
              for (int j = 0; j < 4; j++) sl0[b][j] = sl1[b][j] = rnd(-0.3f, 0.3f), sr0[b][j] = sr1[b][j] = rnd(-0.3f, 0.3f);
            for (uint32_t i = 0; i < n; i++) l0[i] = l1[i] = rnd(-1.0f, 1.0f), r0[i] = r1[i] = rnd(-1.0f, 1.0f);
            for (uint32_t b = 0; b < nb; b++) {
              if (mask && !en[b]) continue;
              for (uint32_t i = 0; i < n; i++) l0[i] = omx_biquad(l0[i], cf[b], sl0[b]);
              if (legs == 2) for (uint32_t i = 0; i < n; i++) r0[i] = omx_biquad(r0[i], cf[b], sr0[b]);
            }
            if (legs == 2) omx_biquad_cascade_stereo(l1, r1, n, nb, cf, mask ? en : NULL, sl1, sr1);
            else omx_biquad_cascade(l1, n, nb, cf, mask ? en : NULL, sl1);
            cases++;
            if (memcmp(l0, l1, n * sizeof(float)) || memcmp(sl0, sl1, sizeof sl0) ||
                (legs == 2 && (memcmp(r0, r1, n * sizeof(float)) || memcmp(sr0, sr1, sizeof sr0))))
              bad++;
          }
    ok(bad == 0 && cases == (OMX_EQ_MAX_BANDS + 1) * 4 * 4 * 2,
       "the fused two-leg cascade is the band-outer omx_biquad loop, bit for bit (output and state)", bad, 0.0);
  }
  ok(OMX_OPERATOR_EQ_BANDS_RESERVE + OMX_FBS_DEFAULT_MAX_AUTO_BANDS + OMX_HRP_DEFAULT_MAX_AUTO_BANDS <= OMX_EQ_MAX_BANDS,
     "the cascade cap holds the declared joint budget (operator reserve + FBS + HRP)",
     OMX_OPERATOR_EQ_BANDS_RESERVE + OMX_FBS_DEFAULT_MAX_AUTO_BANDS + OMX_HRP_DEFAULT_MAX_AUTO_BANDS, OMX_EQ_MAX_BANDS);
  expect_clean();
}
