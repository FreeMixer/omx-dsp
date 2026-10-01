// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the zero-crossing divider ------------------------------------------------------------- */

/* |DTFT| of y[n0 .. n0+N) at hz, scaled so a sine of amplitude A reads A. */
static double line_at(const float *y, uint32_t n0, uint32_t N, double hz, double sr) {
  double re = 0.0, im = 0.0;
  const double w = 2.0 * M_PI * hz / sr;
  for (uint32_t n = 0; n < N; n++) {
    re += (double)y[n0 + n] * cos(w * (double)(n0 + n));
    im -= (double)y[n0 + n] * sin(w * (double)(n0 + n));
  }
  return 2.0 * sqrt(re * re + im * im) / (double)N;
}

/* The series of the sub-octaver spec §2: |D(A sin)| at (2j+1)f/2. */
static double divider_series(int j, double a) {
  return j == 0 ? a * 8.0 / (3.0 * M_PI) : a * 8.0 / (M_PI * (2.0 * j + 3.0) * (2.0 * j - 1.0));
}

/* A Schmitt trigger that toggles AT +h: the form the spec refuses, the positive control of arm E. */
static float schmitt_at_threshold(float *q, int *hi, float v, float h) {
  if (!*hi && v > h) { *q = -*q; *hi = 1; }
  if (v < -h) *hi = 0;
  return *q * v;
}

static void arm_divider(void) {
  g_arm = "divider";
  /* three seconds at the top declared rate: the longest arm below runs 2 s + half a period */
  const uint32_t maxs = (uint32_t)(3.0f * declared_rate_max());
  float *const x = malloc(maxs * sizeof *x), *const y = malloc(maxs * sizeof *y);
  ok(x != NULL && y != NULL, "the divider buffers hold three seconds at the top declared rate", maxs, maxs);
  if (!x || !y) { free(x); free(y); return; }
  static const double tones[] = {41.2, 110.0, 440.0};
  const double a = 0.5;
  double worst_line = 0.0, worst_f = 0.0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    /* A: the lines of D(A sin) at (2j+1)f/2 are the series, and the line at f is zero. */
    for (int t = 0; t < 3; t++) {
      const double f = tones[t];
      const uint32_t n0 = (uint32_t)ceil(4.0 * sr / f);
      const uint32_t N = (uint32_t)llround(floor(sr / (2.0 * sr / f)) * (2.0 * sr / f));
      const uint32_t total = n0 + N;
      for (uint32_t n = 0; n < total; n++) x[n] = (float)(a * sin(2.0 * M_PI * f * (double)n / sr));
      struct omx_divider d;
      omx_divider_init(&d);
      omx_divider_block(&d, x, y, total, (float)(0.25 * a));
      for (int j = 0; j <= 4; j++) {
        const double got = line_at(y, n0, N, (2.0 * j + 1.0) * f / 2.0, sr);
        const double err = fabs(got - divider_series(j, a)) / a;
        if (err > worst_line) worst_line = err;
        ok(err < 1e-4, "a line of D(A sin) is the closed-form series to 1e-4 of A", err, 1e-4);
      }
      const double at_f = line_at(y, n0, N, f, sr) / a;
      if (at_f > worst_f) worst_f = at_f;
      ok(at_f < 1e-5, "the line at the input's own f is below -100 dB re A", at_f, 1e-5);
      uint32_t crossings = 0u;
      for (uint32_t k = 1u; ceil((double)k * sr / f) <= (double)(total - 1u); k++) crossings++;
      ok(d.toggles == crossings, "one toggle per input period (L2)", (double)d.toggles, (double)crossings);
    }
    /* C: noise with peak under h/2 never adds a toggle; a harmonic-rich note toggles once a cycle. */
    {
      const double f = 110.0;
      const uint32_t total = (uint32_t)(2.0 * sr + 0.5 * sr / f);
      const double h = 0.25 * a;
      const uint32_t periods = 220u;
      static const double under[] = {-20.0, -18.5};
      for (int k = 0; k < 2; k++) {
        const double peak = a * pow(10.0, under[k] / 20.0);
        for (uint32_t n = 0; n < total; n++)
          x[n] = (float)(a * sin(2.0 * M_PI * f * (double)n / sr + 0.1) + (double)rnd((float)-peak, (float)peak));
        struct omx_divider d;
        omx_divider_init(&d);
        omx_divider_block(&d, x, y, total, (float)h);
        ok(d.toggles == periods, "noise under h/2 adds no toggle", (double)d.toggles, (double)periods);
      }
      {
        const double peak = a * pow(10.0, -12.5 / 20.0);
        for (uint32_t n = 0; n < total; n++)
          x[n] = (float)(a * sin(2.0 * M_PI * f * (double)n / sr + 0.1) + (double)rnd((float)-peak, (float)peak));
        struct omx_divider d;
        omx_divider_init(&d);
        omx_divider_block(&d, x, y, total, (float)h);
        ok(d.toggles > periods, "positive control: noise at -12.5 dB (above h/2) adds a toggle the count sees",
           (double)d.toggles, (double)periods);
      }
      for (int ph = 0; ph < 16; ph++) {
        const double phi = 2.0 * M_PI * ph / 16.0;
        for (uint32_t n = 0; n < total; n++) {
          const double w = 2.0 * M_PI * f * (double)n / sr;
          x[n] = (float)(a * (sin(w + 0.1) + 0.25 * sin(2.0 * w + phi)));
        }
        struct omx_divider d;
        omx_divider_init(&d);
        omx_divider_block(&d, x, y, total, (float)h);
        ok(d.toggles == periods, "H2 at -12 dB at every phase toggles once a cycle", (double)d.toggles, (double)periods);
      }
    }
    /* E: the largest output step never exceeds the input's; the +h Schmitt breaks the bound. */
    {
      const double f = 560.0;
      const uint32_t total = (uint32_t)(sr / 2.0);
      for (uint32_t n = 0; n < total; n++) x[n] = (float)sin(2.0 * M_PI * f * (double)n / sr);
      struct omx_divider d;
      omx_divider_init(&d);
      omx_divider_block(&d, x, y, total, 0.25f);
      float q = 1.0f;
      int hi = 0;
      double din = 0.0, dout = 0.0, dsch = 0.0, prev_s = 0.0;
      for (uint32_t n = 1; n < total; n++) {
        const double s = schmitt_at_threshold(&q, &hi, x[n], 0.25f);
        din = fmax(din, fabs((double)x[n] - x[n - 1]));
        dout = fmax(dout, fabs((double)y[n] - y[n - 1]));
        dsch = fmax(dsch, fabs(s - prev_s));
        prev_s = s;
      }
      ok(dout <= din, "the divider's largest step is within the input's", dout, din);
      ok(dsch > 2.0 * din, "positive control: a toggle at +h steps past twice the input's step", dsch, 2.0 * din);
    }
  }
  printf("divider: worst line error %.3g of A, worst line at f %.3g of A (%.1f dB)\n", worst_line, worst_f,
         20.0 * log10(worst_f + 1e-30));
  ok(omx_divider_state_size() == sizeof(struct omx_divider) && omx_divider_state_align() == _Alignof(struct omx_divider),
     "the state layout is exported", (double)omx_divider_state_size(), sizeof(struct omx_divider));
  free(x); free(y);
  expect_clean();
}
