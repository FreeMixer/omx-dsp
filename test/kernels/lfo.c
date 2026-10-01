// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the LFO -------------------------------------------------------------------------------- */

static void arm_lfo(void) {
  g_arm = "lfo";
  double worst = 0.0;
  for (int i = 0; i < 1000; i++) {
    const float u = (float)i / 1000.0f;
    const double err = fabs((double)omx_lfo_shape(u) + sin(2.0 * M_PI * u));
    if (err > worst) worst = err;
  }
  ok(worst < 0.0562, "the parabola is -sin(2 pi u) to within 0.0561", worst, 0.0561);
  ok(omx_lfo_shape(0.0f) == 0.0f && omx_lfo_shape(0.5f) == 0.0f, "the shape crosses zero at 0 and half a turn", omx_lfo_shape(0.5f), 0.0);
  ok(omx_lfo_shape(0.25f) == -1.0f && omx_lfo_shape(0.75f) == 1.0f, "the shape peaks at the quarter turns", omx_lfo_shape(0.75f), 1.0);
  ok(omx_lfo_wrap(1.25f) == 0.25f && omx_lfo_wrap(0.9f) == 0.9f, "wrap folds one turn", omx_lfo_wrap(1.25f), 0.25);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    struct omx_lfo l = {0.0f, omx_lfo_inc(2.0f, sr), 0.0f};
    ok(fabsf(l.inc * sr - 2.0f) < 1e-4f, "the increment is the rate in turns per sample", l.inc * sr, 2.0);
    ok(omx_lfo_inc(0.0f, sr) == 0.0f && omx_lfo_inc(-1.0f, sr) == 0.0f, "a non-positive rate freezes", omx_lfo_inc(-1.0f, sr), 0.0);
    ok(omx_lfo_inc(sr * 0.5f, sr) == 0.0f && omx_lfo_inc(sr, sr) == 0.0f, "a rate at or above Nyquist freezes", omx_lfo_inc(sr, sr), 0.0);
    /* one second of advances lands two turns later, and the phase never leaves [0, 1) */
    int inside = 1;
    for (uint32_t i = 0; i < (uint32_t)sr; i++) { omx_lfo_advance(&l); if (!(l.phase >= 0.0f && l.phase < 1.0f)) inside = 0; }
    ok(inside, "the phase stays inside one turn across a second", l.phase, 1.0);
    ok(fabsf(l.phase) < 1e-2f || fabsf(l.phase - 1.0f) < 1e-2f, "two whole turns return to phase zero within float accumulation", l.phase, 0.0);
    /* N reads of one oscillator at offsets k/N are the shape at those phases */
    l.phase = 0.3f;
    for (int k = 0; k < 4; k++) {
      const float off = (float)k / 4.0f;
      ok(omx_lfo_at(&l, off) == omx_lfo_shape(omx_lfo_wrap(0.3f + off)), "a read at an offset is the shape at that phase", off, 0.0);
    }
    ok(l.phase == 0.3f, "a read does not move the oscillator", l.phase, 0.3);
    ok(omx_lfo_sweep(10.0f, 4.0f, -1.0f) == 10.0f && omx_lfo_sweep(10.0f, 4.0f, 1.0f) == 14.0f, "the sweep runs from base to base + depth", omx_lfo_sweep(10.0f, 4.0f, 1.0f), 14.0);
  }
  /* THE PERIOD IS THE RATE's (dsp-primitives §1 row 12): 1/rate seconds of advances land back
   * on the phase they started from, at the slow end of every consumer's travel, every rate. A bare
   * float add rounds each step to the phase's ulp and missed by up to 0.024 of a turn here. */
  static const float SLOW[4] = {0.05f, 0.1f, 1.0f, 20.0f};
  double worst_turns = 0.0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++)
    for (int k = 0; k < 4; k++) {
      const float sr = OMX_DECLARED_RATES[ri];
      struct omx_lfo l = {0.0f, omx_lfo_inc(SLOW[k], sr), 0.0f};
      const uint64_t turn = (uint64_t)llround((double)sr / (double)SLOW[k]);
      for (uint64_t i = 0; i < turn; i++) omx_lfo_advance(&l);
      const double off = l.phase > 0.5f ? 1.0 - (double)l.phase : (double)l.phase;
      if (off > worst_turns) worst_turns = off;
    }
  ok(worst_turns < 1e-5, "one period of advances returns to the start, every rate (turns)", worst_turns, 1e-5);
  struct omx_lfo frozen = {0.25f, 0.0f, 0.0f};
  for (int i = 0; i < 1000; i++) omx_lfo_advance(&frozen);
  ok(frozen.phase == 0.25f, "a frozen oscillator does not move", frozen.phase, 0.25);
  ok(omx_lfo_state_size() == sizeof(struct omx_lfo) && omx_lfo_state_align() == _Alignof(struct omx_lfo), "the state layout is exported", (double)omx_lfo_state_size(), sizeof(struct omx_lfo));
  expect_clean();
}
