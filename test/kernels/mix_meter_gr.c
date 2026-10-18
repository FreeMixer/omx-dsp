// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_meter.h>

/* ---- the gain-reduction reading's hold and recovery (mix/omx_meter.h, FreeMixer/omx-dsp#98). At
 * each of the nine oracle rates, in 10 ms blocks with a 500 ms release pole: a deeper block shows at
 * once; the reading stays put for the declared hold, a shallower block never lifts it and a deeper
 * one replaces it; once the hold is spent the linear gain closes on the block by pole^n per block,
 * against the closed form within 0.01 dB, lands on 0 dB exactly, and on a −5 dB block never passes
 * it. A pole outside [0, 1) lands the first block after the hold. ------------------------------ */

static const float MIX_GR_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

static void arm_mix_meter_gr(void) {
  g_arm = "mix_meter_gr";
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const float sr = MIX_GR_RATES[ri];
    const uint32_t n = (uint32_t)sr / 100u;
    const float pole = expf(-1.0f / (0.5f * sr)); /* a 500 ms release */
    const uint32_t hold_blocks = (uint32_t)(OMX_METER_GR_HOLD_MS / 10.0f); /* 30 */
    struct omx_meter_gr m = {0.0f, 0.0f};

    float r = omx_meter_gr_step(&m, -12.0f, n, sr, pole);
    ok(r == -12.0f, "a deeper block shows at once", r, -12.0);
    uint32_t moved = 0;
    for (uint32_t b = 1; b < hold_blocks; b++)
      if (omx_meter_gr_step(&m, 0.0f, n, sr, pole) != -12.0f) moved++;
    ok(moved == 0, "the reading stays put for the declared hold", moved, 0);
    r = omx_meter_gr_step(&m, -6.0f, n, sr, pole);
    ok(r == -12.0f, "a shallower block does not lift the hold", r, -12.0);
    r = omx_meter_gr_step(&m, -18.0f, n, sr, pole);
    ok(r == -18.0f, "a deeper block replaces the hold", r, -18.0);

    /* spend the hold: the first block that moves is the first of the recovery */
    uint32_t held = 0;
    while ((r = omx_meter_gr_step(&m, 0.0f, n, sr, pole)) == -18.0f && held < 4u * hold_blocks) held++;
    ok(held + 1u >= hold_blocks && held <= hold_blocks + 1u, "the hold lasts the declared 300 ms", held + 1u,
       hold_blocks);
    const double k = pow((double)pole, (double)n);
    double g = 1.0 + (pow(10.0, -18.0 / 20.0) - 1.0) * k;
    double worst = fabs((double)r - 20.0 * log10(g));
    for (uint32_t b = 0; b < 40; b++) {
      r = omx_meter_gr_step(&m, 0.0f, n, sr, pole);
      g = 1.0 + (g - 1.0) * k;
      const double e = fabs((double)r - 20.0 * log10(g));
      if (e > worst) worst = e;
    }
    ok(worst < 0.01, "the recovery follows the release pole's closed form", worst, 0.01);
    for (uint32_t b = 0; b < 4000 && r != 0.0f; b++) r = omx_meter_gr_step(&m, 0.0f, n, sr, pole);
    ok(r == 0.0f, "it recovers all the way to 0 dB", r, 0.0);

    /* on a −5 dB block the recovery lands on the block and never passes it */
    m = (struct omx_meter_gr){0.0f, 0.0f};
    omx_meter_gr_step(&m, -12.0f, n, sr, pole);
    uint32_t past = 0;
    for (uint32_t b = 0; b < 4000; b++) {
      r = omx_meter_gr_step(&m, -5.0f, n, sr, pole);
      if (r > -5.0f) past++;
    }
    ok(past == 0 && r == -5.0f, "the recovery lands on the current block, never past it", r, -5.0);

    /* a pole outside [0, 1) lands the first block after the hold */
    static const float bad[] = {0.0f, 1.0f, -0.25f, NAN};
    for (uint32_t p = 0; p < 4; p++) {
      m = (struct omx_meter_gr){0.0f, 0.0f};
      omx_meter_gr_step(&m, -30.0f, n, sr, bad[p]);
      uint32_t b = 0;
      while ((r = omx_meter_gr_step(&m, -2.0f, n, sr, bad[p])) == -30.0f && b < 4u * hold_blocks) b++;
      ok(r == -2.0f, "a pole outside [0, 1) lands at once once the hold is spent", r, -2.0);
    }
  }
  expect_clean();
}
