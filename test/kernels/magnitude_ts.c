// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the magnitude evaluator against today's TypeScript (FreeMixer/omx-dsp#69) ------------- */

#include "fixtures/eq_curve_ts.h"

/* The largest gap the C evaluator over omx_eq_design() may keep from core's eqCurveDb at a point:
 * the bound of the DSP-results-from-C design, §4.3. Both sides evaluate one formula in double
 * over sections that differ by a few ulp (Math.pow against exp for the gain, V8's cos and sin
 * against the libm's); measured, the gap is under 2e-12 dB, the floored nulls included, so the
 * bound leaves the libm of another platform some 500 times that room and still catches any change
 * of law (test/magnitude-sabotage.sh). */
#define MAGNITUDE_TS_TOL_DB 1e-9

/* eqCurveDb's points, captured by tools/ts-golden.mjs, against the C evaluator over
 * omx_eq_design() at the same frequencies, and its anchors against omx_eq_band_anchors(). */
static void arm_magnitude_ts(void) {
  g_arm = "magnitude-ts";
  double worst = 0.0;
  const uint32_t ncurves = (uint32_t)(sizeof TS_CURVES / sizeof TS_CURVES[0]);
  for (uint32_t k = 0; k < ncurves; k++) {
    const struct ts_curve *tc = &TS_CURVES[k];
    double bank[4][5], db[64];
    ok(tc->n <= 64u, "the fixture curve fits the buffer", tc->n, 64.0);
    if (tc->n > 64u) continue;
    for (uint32_t b = 0; b < tc->nb; b++)
      omx_eq_design(tc->b[b].kind, tc->b[b].freq, tc->b[b].q, tc->b[b].gain_db, tc->sr, bank[b]);
    omx_biquad_cascade_magnitude_db_d((const double(*)[5])bank, NULL, tc->nb, &TS_CURVE_F[tc->off], tc->n, tc->sr,
                                      TS_CURVE_PLOT_FLOOR_DB, db);
    for (uint32_t i = 0; i < tc->n; i++) {
      const double ts = TS_CURVE_DB[tc->off + i], gap = fabs(db[i] - ts);
      if (gap > worst) worst = gap;
      ok(gap <= MAGNITUDE_TS_TOL_DB, "a point agrees with eqCurveDb", db[i], ts);
    }
    for (uint32_t b = 0; b < tc->nb; b++) {
      double a[3];
      omx_eq_band_anchors(tc->b[b].freq, tc->b[b].q, a);
      for (int j = 0; j < 3; j++) {
        if (!(a[j] > 20.0 && a[j] < 20000.0)) continue;
        int found = 0;
        for (uint32_t i = 0; i < tc->n; i++) found |= TS_CURVE_F[tc->off + i] == a[j];
        ok(found, "each anchor eqCurveDb added is omx_eq_band_anchors()'s, bit for bit", a[j], 0.0);
      }
    }
    magnitude_drain(tc->sr);
  }
  printf("magnitude-ts: %u curves at the nine RME rates, largest gap from eqCurveDb %.3g dB (bound %.0e)\n",
         ncurves, worst, MAGNITUDE_TS_TOL_DB);
  expect_clean();
}
