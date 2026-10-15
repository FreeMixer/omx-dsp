// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_rt_load.h>

/* ---- the real-time load meter's measure half (mix/omx_rt_load.h, FreeMixer/omx-dsp#98). At each
 * of the nine RME rates, over a 10 ms quantum: half the budget reads 0.5, the whole budget reads 1
 * and overruns, just under it does not; a cycle with no frames or no rate reads 0; the smoothed
 * reading starts on the first ratio, holds a constant input, and after k cycles of a unit step
 * reads 1 − 0.9^k; the peak keeps the largest ratio and an equal or NaN one moves nothing; the
 * driver's xrun total counts nothing on its baseline, one event when it grows, none when it holds
 * or falls. ----------------------------------------------------------------------------------- */

static const float MIX_RT_LOAD_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;

static void arm_mix_rt_load(void) {
  g_arm = "mix_rt_load";
  for (uint32_t ri = 0; ri < OMX_RME_RATES_COUNT; ri++) {
    const float sr = MIX_RT_LOAD_RATES[ri];
    const uint32_t frames = (uint32_t)(sr / 100.0f); /* 10 ms, a whole number of frames at every rate */
    const float half = omx_rt_load_ratio(5000000u, frames, sr);
    ok(fabsf(half - 0.5f) < 1e-6f, "half the budget reads 0.5", half, 0.5);
    const float whole = omx_rt_load_ratio(10000000u, frames, sr);
    ok(fabsf(whole - 1.0f) < 1e-6f, "the whole budget reads 1", whole, 1.0);
    ok(omx_rt_load_overrun(1.0f) && !omx_rt_load_overrun(0.999f), "1 overruns, just under does not", 0, 0);
    ok(omx_rt_load_ratio(1000u, 0u, sr) == 0.0f && omx_rt_load_ratio(1000u, frames, 0.0f) == 0.0f &&
           omx_rt_load_ratio(1000u, frames, NAN) == 0.0f,
       "no frames or no rate reads 0", 0, 0);

    float s = omx_rt_load_smooth(123.0f, 0u, half);
    ok(s == half, "the first cycle seeds the reading", s, half);
    for (uint32_t c = 1; c < 50u; c++) s = omx_rt_load_smooth(s, c, half);
    ok(fabsf(s - half) < 1e-6f, "a constant input holds", s, half);
    s = omx_rt_load_smooth(0.0f, 0u, 0.0f);
    double worst = 0.0;
    for (uint32_t k = 1; k <= 40u; k++) {
      s = omx_rt_load_smooth(s, k, 1.0f);
      const double want = 1.0 - pow(0.9, (double)k);
      if (fabs(s - want) > worst) worst = fabs(s - want);
    }
    ok(worst < 1e-5, "after k cycles of a unit step the reading is 1 - 0.9^k", worst, 1e-5);

    float p = 0.0f;
    p = omx_rt_load_peak(p, 0.3f);
    p = omx_rt_load_peak(p, 0.7f);
    p = omx_rt_load_peak(p, 0.2f);
    p = omx_rt_load_peak(p, NAN);
    p = omx_rt_load_peak(p, 0.7f);
    ok(p == 0.7f, "the peak keeps the largest ratio", p, 0.7);

    ok(omx_rt_load_xrun_events(false, 0u, 5000u) == 0u, "the baseline counts nothing", 0, 0);
    ok(omx_rt_load_xrun_events(true, 5000u, 6000u) == 1u, "a growing total is one event", 0, 0);
    ok(omx_rt_load_xrun_events(true, 6000u, 6000u) == 0u, "an unchanged total counts nothing", 0, 0);
    ok(omx_rt_load_xrun_events(true, 6000u, 10u) == 0u, "a total that falls is a new driver, not an xrun", 0, 0);
  }
  expect_clean();
}
