// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <float.h>

#include <omxdsp/mix/omx_route_gain.h>

/* ---- an output route's gain stages (mix/omx_route_gain.h). At each of the nine oracle rates, 10 ms
 * blocks of a 997 Hz tone through a run of gain moves (up, down, through zero to a polarity
 * invert, held): each sample is the closed form in·(cur + i·(tgt − cur)/n) within 4 ulp of the
 * product, the gains are linear across the block (equal steps within float rounding), and every
 * cursor leaves the block exactly on its target. A held gain is the plain product, bit for bit. A
 * correlated pair folded at 0.5 lands exactly on its own value, an anti-correlated pair cancels
 * exactly, and the in-place fold writes both legs alike from the legs as they were. n = 0 touches
 * nothing; a non-finite target reads as 0 on the omx_ramp path. The engine's own cases
 * (mix_dsp.test.c: fill, polarity invert, fold, fold-fill) are kept as exact checks. ---------------- */

static const float MIX_ROUTE_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

/* `got` against the closed form `want`, within `ulps` of the magnitude the product is formed at. */
static int route_near(float got, double want, double scale, double ulps) {
  return fabs((double)got - want) <= ulps * (double)FLT_EPSILON * (scale > 1e-30 ? scale : 1e-30);
}

static void arm_mix_route_gain(void) {
  g_arm = "mix_route_gain";
  /* the engine's cases, exact */
  {
    float in[4] = {1, 1, 1, 1}, out[4] = {0}, cur = 0.0f;
    omx_route_fill_gain(in, out, 4, 2.0f, &cur);
    ok(out[0] == 0.0f && out[1] == 0.5f && out[2] == 1.0f && out[3] == 1.5f && cur == 2.0f,
       "fill: 0 to 2 over 4 samples is 0, 0.5, 1, 1.5, the cursor ends on 2", out[3], 1.5);
    omx_route_fill_gain(in, out, 4, 2.0f, &cur);
    ok(out[0] == 2.0f && out[3] == 2.0f, "fill: a held gain is a flat copy times the gain", out[0], 2.0);
    float c = 0.7f;
    omx_route_fill_gain(in, out, 0, 1.0f, &c);
    ok(c == 0.7f, "fill: n = 0 leaves the cursor", c, 0.7);
    float pin[4] = {0.5f, -0.25f, 1.0f, -1.0f}, pout[4], pc = -2.0f;
    omx_route_fill_gain(pin, pout, 4, -2.0f, &pc);
    ok(pout[0] == -1.0f && pout[1] == 0.5f && pout[2] == -2.0f && pout[3] == 2.0f,
       "fill: a held negative gain is an exact polarity invert", pout[0], -1.0);
    float ramp[4], pc2 = 1.0f;
    omx_route_fill_gain(in, ramp, 4, -1.0f, &pc2);
    ok(ramp[0] == 1.0f && ramp[1] == 0.5f && ramp[2] == 0.0f && ramp[3] == -0.5f && pc2 == -1.0f,
       "fill: engaging the invert ramps +g to -g through zero", ramp[2], 0.0);
    float nf = 0.5f;
    omx_route_fill_gain(in, out, 4, NAN, &nf);
    ok(nf == 0.0f && out[0] == 0.5f && out[3] == 0.125f, "fill: a NaN target reads as 0", out[3], 0.125);

    float bl[4] = {1.0f, 0.5f, -0.25f, 0.0f}, br[4] = {1.0f, 0.5f, 0.75f, -0.4f}, fo[4], fc = 0.5f;
    omx_route_fold_gain(bl, br, fo, 4, 0.5f, &fc);
    ok(fo[0] == 1.0f && fo[1] == 0.5f && fo[2] == 0.25f && fo[3] == -0.2f && fc == 0.5f,
       "fold: (L + R)·0.5 per sample, the cursor ends on its target", fo[3], -0.2);
    float mo[3] = {9, 9, 9}, mc = 0.0f;
    omx_route_fold_gain(bl, br, mo, 3, 0.0f, &mc);
    ok(mo[0] == 0.0f && mo[1] == 0.0f && mo[2] == 0.0f, "fold: a muted fold zeroes the destination", mo[0], 0.0);
    float c0 = 0.42f;
    omx_route_fold_gain(bl, br, fo, 0, 1.0f, &c0);
    ok(c0 == 0.42f, "fold: n = 0 leaves the cursor", c0, 0.42);

    float rl[4] = {1, 1, 1, 1}, rr[4] = {1, 1, 1, 1}, rcl = 0.0f, rcr = 0.0f;
    omx_route_fold_fill(rl, rr, 4, 1.0f, &rcl, &rcr);
    ok(rl[0] == 0.0f && rl[1] == 0.5f && rl[2] == 1.0f && rl[3] == 1.5f && rr[3] == 1.5f && rcl == 1.0f && rcr == 1.0f,
       "fold-fill: 0 to 1 over 4 samples of (1 + 1) is 0, 0.5, 1, 1.5 on both legs", rl[3], 1.5);
    float ffl[4] = {1.0f, 0.5f, -0.25f, 0.2f}, ffr[4] = {1.0f, 0.5f, 0.75f, -0.2f}, fl = 0.5f, fr = 0.5f;
    omx_route_fold_fill(ffl, ffr, 4, 0.5f, &fl, &fr);
    ok(ffl[0] == 1.0f && ffl[2] == 0.25f && ffl[3] == 0.0f && ffr[2] == 0.25f,
       "fold-fill: the fold of the legs as they were, anti-correlated legs cancel", ffl[3], 0.0);
    float zl = 0.3f, zr = 0.6f;
    omx_route_fold_fill(ffl, ffr, 0, 1.0f, &zl, &zr);
    ok(zl == 0.3f && zr == 0.6f, "fold-fill: n = 0 leaves both cursors", zl, 0.3);
  }

  static float x[1920], y[1920], l[1920], r[1920], o[1920], l0[1920], r0[1920];
  static const float tgts[] = {0.0f, 1.0f, 0.5f, -0.5f, -0.5f, 2.0f, 0.70710678f, 0.0f, 0.0f, 1.0f};
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const float sr = MIX_ROUTE_RATES[ri];
    const uint32_t n = (uint32_t)sr / 100u;
    uint32_t sample_bad = 0, linear_bad = 0, end_bad = 0, held_bad = 0, fold_exact_bad = 0, both_bad = 0;
    double worst = 0.0;
    float cf = 0.0f, cg = 0.0f, ccl = 0.0f, ccr = 0.25f;
    for (uint32_t b = 0; b < sizeof tgts / sizeof tgts[0]; b++) {
      const float t = tgts[b];
      for (uint32_t i = 0; i < n; i++) {
        const double ph = 2.0 * M_PI * 997.0 * (double)(b * n + i) / sr;
        x[i] = 0.8f * (float)sin(ph);
        y[i] = 0.3f * (float)cos(ph);
      }
      /* fill */
      const float c0 = cf;
      omx_route_fill_gain(x, o, n, t, &cf);
      for (uint32_t i = 0; i < n; i++) {
        const double g = (double)c0 + ((double)t - (double)c0) * (double)i / (double)n;
        const double want = (double)x[i] * g;
        const double scale = fabs((double)x[i]) * (fabs((double)c0) + fabs((double)t));
        if (!route_near(o[i], want, scale, 4.0)) sample_bad++;
        if (fabs((double)o[i] - want) > worst) worst = fabs((double)o[i] - want);
        if (c0 == t && o[i] != x[i] * t) held_bad++;
      }
      if (cf != t) end_bad++;
      /* the gains are linear: unit input, equal steps */
      for (uint32_t i = 0; i < n; i++) l[i] = 1.0f;
      float cu = c0;
      omx_route_fill_gain(l, l, n, t, &cu); /* in place */
      const double step = ((double)t - (double)c0) / (double)n;
      for (uint32_t i = 1; i < n; i++)
        if (fabs(((double)l[i] - (double)l[i - 1u]) - step) > 4.0 * (double)FLT_EPSILON * (fabs((double)c0) + fabs((double)t)))
          linear_bad++;
      /* fold */
      const float g0 = cg;
      omx_route_fold_gain(x, y, o, n, t, &cg);
      for (uint32_t i = 0; i < n; i++) {
        const double g = (double)g0 + ((double)t - (double)g0) * (double)i / (double)n;
        const double s = (double)(x[i] + y[i]);
        if (!route_near(o[i], s * g, (fabs((double)x[i]) + fabs((double)y[i])) * (fabs((double)g0) + fabs((double)t)), 6.0))
          sample_bad++;
      }
      if (cg != t) end_bad++;
      float hc = 0.5f;
      omx_route_fold_gain(x, x, o, n, 0.5f, &hc);
      for (uint32_t i = 0; i < n; i++)
        if (o[i] != x[i]) fold_exact_bad++;
      for (uint32_t i = 0; i < n; i++) l[i] = -x[i];
      omx_route_fold_gain(x, l, o, n, 0.5f, &hc);
      for (uint32_t i = 0; i < n; i++)
        if (o[i] != 0.0f) fold_exact_bad++;
      /* fold-fill: the two cursors start apart and both land on the target */
      memcpy(l, x, n * sizeof(float));
      memcpy(r, y, n * sizeof(float));
      memcpy(l0, x, n * sizeof(float));
      memcpy(r0, y, n * sizeof(float));
      const float fl0 = ccl, fr0 = ccr;
      omx_route_fold_fill(l, r, n, t, &ccl, &ccr);
      for (uint32_t i = 0; i < n; i++) {
        const double s = (double)(l0[i] + r0[i]);
        const double gl = (double)fl0 + ((double)t - (double)fl0) * (double)i / (double)n;
        const double gr = (double)fr0 + ((double)t - (double)fr0) * (double)i / (double)n;
        /* the accumulated step drifts by at most one rounding per sample */
        const double tol = (double)n * 2.0 * (double)FLT_EPSILON * (fabs((double)fl0) + fabs((double)fr0) + fabs((double)t)) * fabs(s) +
                           2.0 * (double)FLT_EPSILON * fabs(s);
        if (fabs((double)l[i] - s * gl) > tol || fabs((double)r[i] - s * gr) > tol) sample_bad++;
        if (fl0 == fr0 && l[i] != r[i]) both_bad++;
      }
      if (ccl != t || ccr != t) end_bad++;
      memcpy(l, x, n * sizeof(float));
      memcpy(r, x, n * sizeof(float));
      float hl = 0.5f, hr = 0.5f;
      omx_route_fold_fill(l, r, n, 0.5f, &hl, &hr);
      for (uint32_t i = 0; i < n; i++)
        if (l[i] != x[i] || r[i] != x[i]) fold_exact_bad++;
    }
    ok(sample_bad == 0, "every sample is the closed form of its ramp", worst, 0.0);
    ok(linear_bad == 0, "the gain steps equally across the block", linear_bad, 0);
    ok(end_bad == 0, "every cursor leaves the block exactly on its target", end_bad, 0);
    ok(held_bad == 0, "a held gain is the plain product, bit for bit", held_bad, 0);
    ok(fold_exact_bad == 0, "a correlated pair at 0.5 lands on itself, an anti-correlated one cancels", fold_exact_bad, 0);
    ok(both_bad == 0, "the in-place fold writes both legs alike", both_bad, 0);
  }
  expect_clean();
}
