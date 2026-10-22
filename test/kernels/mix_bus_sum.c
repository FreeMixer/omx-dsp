// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_bus_sum.h>

/* ---- one source's contribution to a summing bus (mix/omx_bus_sum.h, FreeMixer/omx-dsp#98). At
 * each of the nine oracle rates, a 10 ms block: a settled send into a stereo bus is in × level per
 * leg exactly, and a second send adds to it; a move from 0 to 1 ramps linearly across the block;
 * a hard-left image leaves the right leg alone; a correlated source folded into a mono bus at
 * 0.5 lands at unity and both cursors take the fold's target; a silent, settled send writes
 * nothing, one ramping down to 0 still writes; a NaN level mutes the contribution and leaves
 * finite cursors; the entries through omx_mixmatrix_sparse() give the same bits on a zeroed
 * bus. -------------------------------------------------------------------------------------- */

static const float MIX_BUS_SUM_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

static void arm_mix_bus_sum(void) {
  g_arm = "mix_bus_sum";
  static float l[1920], r[1920], bl[1920], br[1920], y0[1920], y1[1920];
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const uint32_t n = (uint32_t)(MIX_BUS_SUM_RATES[ri] / 100.0f);
    for (uint32_t i = 0; i < n; i++) {
      l[i] = 0.8f;
      r[i] = 0.4f;
      bl[i] = br[i] = 0.0f;
    }

    /* settled, then a second send */
    float cl = 0.5f, cr = 0.5f;
    int wrote = omx_bus_sum_add(l, r, bl, br, n, 0.5f, 1.0f, 1.0f, &cl, &cr, OMX_BUS_SUM_FOLLOW, 0.5f);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.8f * 0.5f || br[i] != 0.4f * 0.5f) bad++;
    ok(wrote == 1 && bad == 0, "a settled send is in × level on each leg exactly", bad, 0);
    cl = cr = 0.25f;
    omx_bus_sum_add(l, r, bl, br, n, 0.25f, 1.0f, 1.0f, &cl, &cr, OMX_BUS_SUM_FOLLOW, 0.5f);
    double worst = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      if (fabs(bl[i] - 0.8 * 0.75) > worst) worst = fabs(bl[i] - 0.8 * 0.75);
      if (fabs(br[i] - 0.4 * 0.75) > worst) worst = fabs(br[i] - 0.4 * 0.75);
    }
    ok(worst < 1e-7, "a second send adds to the bus", worst, 1e-7);

    /* a 0 → 1 move */
    for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.0f;
    cl = cr = 0.0f;
    omx_bus_sum_add(l, r, bl, br, n, 1.0f, 1.0f, 1.0f, &cl, &cr, OMX_BUS_SUM_FOLLOW, 0.5f);
    worst = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const double t = (double)i / (double)n;
      if (fabs(bl[i] - 0.8 * t) > worst) worst = fabs(bl[i] - 0.8 * t);
      if (fabs(br[i] - 0.4 * t) > worst) worst = fabs(br[i] - 0.4 * t);
    }
    ok(worst < 1e-6, "a 0 → 1 move ramps linearly across the block", worst, 1e-6);
    ok(cl == 1.0f && cr == 1.0f, "the cursors end on the targets", cl, 1.0);

    /* a hard-left image */
    for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.0f;
    cl = 1.0f;
    cr = 0.0f;
    omx_bus_sum_add(l, r, bl, br, n, 1.0f, 1.0f, 0.0f, &cl, &cr, OMX_BUS_SUM_FOLLOW, 0.5f);
    bad = 0;
    for (uint32_t i = 0; i < n; i++) if (br[i] != 0.0f || bl[i] != 0.8f) bad++;
    ok(bad == 0, "a hard-left image leaves the right leg alone", bad, 0);

    /* a correlated source folded into a mono bus */
    for (uint32_t i = 0; i < n; i++) bl[i] = 0.0f;
    cl = cr = 0.5f;
    omx_bus_sum_add(l, l, bl, NULL, n, 1.0f, 1.0f, 1.0f, &cl, &cr, OMX_BUS_SUM_FOLD, 0.5f);
    bad = 0;
    for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.8f) bad++;
    ok(bad == 0, "a correlated source folded at 0.5 lands at unity", bad, 0);
    ok(cl == 0.5f && cr == 0.5f, "both cursors take the fold's target", cr, 0.5);

    /* the skip, and a ramp down that is not skipped */
    for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.125f;
    cl = cr = 0.0f;
    wrote = omx_bus_sum_add(l, r, bl, br, n, 0.0f, 1.0f, 1.0f, &cl, &cr, OMX_BUS_SUM_FOLLOW, 0.5f);
    bad = 0;
    for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.125f || br[i] != 0.125f) bad++;
    ok(wrote == 0 && bad == 0, "a silent, settled send writes nothing", bad, 0);
    cl = 0.5f;
    wrote = omx_bus_sum_add(l, r, bl, br, n, 0.0f, 1.0f, 1.0f, &cl, &cr, OMX_BUS_SUM_FOLLOW, 0.5f);
    ok(wrote == 1 && bl[0] == 0.125f + 0.8f * 0.5f && cl == 0.0f, "a send ramping down to 0 still writes", bl[0], 0.525);

    /* a NaN level */
    for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.0f;
    cl = cr = NAN;
    omx_bus_sum_add(l, r, bl, br, n, NAN, 1.0f, 1.0f, &cl, &cr, OMX_BUS_SUM_FOLLOW, 0.5f);
    bad = 0;
    for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.0f || br[i] != 0.0f) bad++;
    ok(bad == 0 && cl == 0.0f && cr == 0.0f, "a NaN level mutes the contribution and the cursors end finite", bad, 0);

    /* a raw entry whose target or cursor is not finite: the accumulate itself takes it as 0 */
    const float bad_g[3] = {NAN, INFINITY, -INFINITY};
    for (uint32_t b = 0; b < 3; b++) {
      struct omx_mixmatrix_entry raw = {0u, 0u, 0.0f, bad_g[b]};
      for (uint32_t i = 0; i < n; i++) bl[i] = 0.25f;
      omx_bus_sum_accumulate(bl, l, n, &raw);
      bad = 0;
      for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.25f) bad++;
      ok(bad == 0, "a non-finite target in a raw entry gives 0 gain, the bus untouched", bad, 0);
      raw.g_prev = bad_g[b];
      raw.g_cur = 1.0f;
      for (uint32_t i = 0; i < n; i++) bl[i] = 0.25f;
      omx_bus_sum_accumulate(bl, l, n, &raw);
      bad = 0;
      for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.25f + l[i] * ((1.0f / (float)n) * (float)i)) bad++;
      ok(bad == 0, "a non-finite cursor in a raw entry starts the ramp from 0", bad, 0);
      for (uint32_t i = 0; i < n; i++) bl[i] = 0.25f;
      raw.g_prev = 0.0f;
      raw.g_cur = bad_g[b];
      omx_bus_sum_accumulate_fold(bl, l, r, n, &raw);
      bad = 0;
      for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.25f) bad++;
      ok(bad == 0, "a non-finite target in a raw fold entry gives 0 gain, the bus untouched", bad, 0);
    }
    /* finite entries: the bits of the plain ramp y += x · (g_prev + step · i), no heal in the way */
    {
      const struct omx_mixmatrix_entry fin = {0u, 0u, 0.3f, -0.7f};
      for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.125f;
      omx_bus_sum_accumulate(bl, l, n, &fin);
      omx_bus_sum_accumulate_fold(br, l, r, n, &fin);
      const float step = (fin.g_cur - fin.g_prev) / (float)n;
      bad = 0;
      for (uint32_t i = 0; i < n; i++) {
        const float g = fin.g_prev + step * (float)i;
        if (bl[i] != 0.125f + l[i] * g || br[i] != 0.125f + (l[i] + r[i]) * g) bad++;
      }
      ok(bad == 0, "a finite entry accumulates bit for bit as before", bad, 0);
    }

    /* the same entries through the mix matrix */
    const struct omx_mixmatrix_entry e[2] = {omx_bus_sum_entry(0u, 0u, 0.1f, 0.9f),
                                             omx_bus_sum_entry(1u, 1u, 0.7f, 0.7f)};
    for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.0f;
    omx_bus_sum_accumulate(bl, l, n, &e[0]);
    omx_bus_sum_accumulate(br, r, n, &e[1]);
    float *out[2] = {y0, y1};
    const float *in[2] = {l, r};
    omx_mixmatrix_sparse(out, in, e, 2u, 2u, 2u, n);
    ok(memcmp(bl, y0, n * sizeof(float)) == 0 && memcmp(br, y1, n * sizeof(float)) == 0,
       "the entries through omx_mixmatrix_sparse give the same bits", 0, 0);
  }
  expect_clean();
}
