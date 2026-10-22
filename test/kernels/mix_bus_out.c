// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_bus_out.h>

/* ---- a bus's out stage (mix/omx_bus_out.h, FreeMixer/omx-dsp#98). At each of the nine oracle rates,
 * a 10 ms block of a 0.8 DC bus on both legs through the stage: settled at a target, each leg is
 * in × gain exactly and the measured peak is 0.8 × gain; at centre balance the gain is the fader
 * on both legs; the linear balance never boosts a leg above the fader across a sweep; the mute
 * settles at 0; a move from 0 to 1 ramps linearly across the block (in × i/n) and the cursors end
 * on the targets. A mono bus takes the stereo pan law instead. ---------------------------------- */

static const float MIX_BUS_OUT_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

static void mix_bus_out_fill(float *l, float *r, uint32_t n, float v) {
  for (uint32_t i = 0; i < n; i++) l[i] = r[i] = v;
}

static void arm_mix_bus_out(void) {
  g_arm = "mix_bus_out";
  static float l[1920], r[1920];
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const float sr = MIX_BUS_OUT_RATES[ri];
    const uint32_t n = (uint32_t)(sr / 100.0f);
    uint32_t settled_bits = 0, boost = 0, peak_bits = 0;
    for (uint32_t s = 0; s <= 40; s++) {
      const float bal = -1.0f + (float)s / 20.0f;
      const float fader = 0.6f;
      float tl, tr;
      omx_bus_out_gains(2, fader, 0, bal, &tl, &tr);
      if (tl > fader || tr > fader || tl < 0.0f || tr < 0.0f) boost++;
      float cl = tl, cr = tr, pl = -1.0f, pr = -1.0f;
      mix_bus_out_fill(l, r, n, 0.8f);
      omx_bus_out_run(l, r, n, 2, fader, 0, bal, &cl, &cr, &pl, &pr);
      for (uint32_t i = 0; i < n; i++)
        if (l[i] != 0.8f * tl || r[i] != 0.8f * tr) settled_bits++;
      if (pl != 0.8f * tl || pr != 0.8f * tr) peak_bits++;
      if (s == 20 && (tl != fader || tr != fader)) boost++;
    }
    ok(settled_bits == 0, "settled, each leg is in × gain exactly", settled_bits, 0);
    ok(boost == 0, "the balance never boosts a leg above the fader; centre is the fader", boost, 0);
    ok(peak_bits == 0, "the measured peak is the leg's own post-fader peak", peak_bits, 0);

    /* the mute settles at 0 */
    float cl = 0.0f, cr = 0.0f, pl, pr;
    mix_bus_out_fill(l, r, n, 0.8f);
    omx_bus_out_run(l, r, n, 2, 1.0f, 1, 0.0f, &cl, &cr, &pl, &pr);
    ok(pl == 0.0f && pr == 0.0f && l[n / 2] == 0.0f, "a muted, settled bus is silent", pl, 0.0);

    /* a move from 0 to 1 is a linear ramp across the block */
    cl = 0.0f;
    cr = 0.0f;
    mix_bus_out_fill(l, r, n, 0.8f);
    omx_bus_out_run(l, r, n, 2, 1.0f, 0, 0.0f, &cl, &cr, &pl, &pr);
    double worst = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const double want = 0.8 * (double)i / (double)n;
      if (fabs(l[i] - want) > worst) worst = fabs(l[i] - want);
      if (fabs(r[i] - want) > worst) worst = fabs(r[i] - want);
    }
    ok(worst < 1e-6, "a 0 → 1 move ramps linearly across the block", worst, 1e-6);
    ok(cl == 1.0f && cr == 1.0f, "the cursors end on the targets", cl, 1.0);

    /* a mono bus: the stereo pan law, −3 dB per leg at centre */
    float tl, tr;
    omx_bus_out_gains(1, 1.0f, 0, 0.0f, &tl, &tr);
    ok(fabs(tl - M_SQRT1_2) < 1e-7 && tl == tr, "a mono bus at centre is −3 dB per leg", tl, M_SQRT1_2);
  }
  expect_clean();
}
