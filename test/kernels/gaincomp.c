// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- gain computer -------------------------------------------------------------------------- */

static double gaincomp_closed(int mode, double t, double r, double k, double range, double l) {
  const double x = l - t, h = 0.5 * k;
  if (mode == OMX_DYN_ABOVE) {
    if (x <= -h) return 0.0;
    if (k > 0.0 && x < h) return (1.0 / r - 1.0) * (x + h) * (x + h) / (2.0 * k);
    return (1.0 / r - 1.0) * x;
  }
  double g;
  if (x >= h) g = 0.0;
  else if (k > 0.0 && x > -h) g = (1.0 - r) * (x - h) * (x - h) / (2.0 * k);
  else g = (r - 1.0) * x;
  return g < range ? range : g;
}

static void arm_gaincomp(void) {
  g_arm = "gaincomp";
  static const struct omx_gaincomp_params cases[] = {
      /* the declared defaults, the travel's corners (hardest ratio at the hardest knee, the
       * softest ratio), and interior points — every edge read off the declaration */
      {OMX_DYN_ABOVE, -20.0f, OMX_COMP_RATIO_DEFAULT, OMX_COMP_KNEE_DB_DEFAULT, 0.0f, 1.0f},
      {OMX_DYN_ABOVE, -12.0f, OMX_COMP_RATIO_MAX, OMX_COMP_KNEE_DB_MIN, 0.0f, 2.0f},
      {OMX_DYN_ABOVE, -30.0f, OMX_COMP_RATIO_MIN, 12.0f, 0.0f, 1.0f},
      {OMX_DYN_BELOW, OMX_GATE_THRESHOLD_DB_DEFAULT, OMX_GATE_RATIO_DEFAULT, 6.0f, -60.0f, 1.0f},
      {OMX_DYN_BELOW, -50.0f, OMX_GATE_RATIO_MAX, OMX_COMP_KNEE_DB_MIN, OMX_GATE_RANGE_DB_MIN, 1.0f},
      {OMX_DYN_BELOW, -35.0f, 2.0f, 10.0f, -20.0f, 1.5f},
  };
  for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    const struct omx_gaincomp_params *p = &cases[c];
    double worst = 0.0, lo = 1e9, hi = -1e9;
    for (int i = 0; i < 200; i++) {
      const float l = -100.0f + 0.5f * (float)i;
      const double d = fabs((double)omx_gaincomp_db(p, l) - gaincomp_closed(p->mode, p->thresh_db, p->ratio, p->knee_db, p->range_db, l));
      if (d > worst) worst = d;
      const float g = omx_gaincomp_gain(p, omx_db_to_lin(l));
      if (g < lo) lo = g;
      if (g > hi) hi = g;
    }
    ok(worst <= 1e-4, "the characteristic is the closed form at 200 levels", worst, 1e-4);
    ok(lo >= 0.0 && hi <= p->makeup_lin * (1.0 + 1e-6), "the gain lies in [0, makeup]", hi, p->makeup_lin);
    if (p->knee_db > 0.0f) {
      const float h = 0.5f * p->knee_db, eps = 1e-3f;
      for (int side = -1; side <= 1; side += 2) {
        const float edge = p->thresh_db + (float)side * h;
        const float in = omx_gaincomp_db(p, edge - (float)side * eps), out = omx_gaincomp_db(p, edge + (float)side * eps);
        ok(fabsf(in - out) <= 2.0f * eps * p->ratio + 1e-4f, "the knee joins its neighbour without a step", fabsf(in - out), 2.0f * eps * p->ratio + 1e-4f);
      }
    }
  }
  const struct omx_gaincomp_params unity = {OMX_DYN_ABOVE, OMX_COMP_THRESHOLD_DB_MAX, OMX_COMP_RATIO_DEFAULT,
                                           OMX_COMP_KNEE_DB_MIN, 0.0f, 1.0f};
  ok(omx_gaincomp_gain(&unity, 0.5f) == 1.0f, "below the threshold the comp is unity bit for bit", omx_gaincomp_gain(&unity, 0.5f), 1.0);
  expect_clean();
}
