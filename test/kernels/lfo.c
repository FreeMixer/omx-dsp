// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "../fx/fx_rates.h"

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
  /* ---- moved from openmixer's mix_lfo.test.c (#1603), arm by arm ---- */

  /* THE SHAPE (engine arm A), on 10001 points of the closed turn [0, 1]: the range is [-1, 1],
   * the shape is odd about the half turn, s(0.5 + v) = -s(0.5 - v), and the deviation from
   * -sin(2 pi u) is bounded FROM BOTH SIDES: <= 0.0561 (the claim) and >= 0.050, so a shape that
   * silently became the sine itself (or a triangle) fails the claim it stands on. */
  double worst_range = 0.0, worst_sym = 0.0, worst_sine = 0.0;
  for (int i = 0; i <= 10000; i++) {
    const double u = (double)i / 10000.0;
    const double s = (double)omx_lfo_shape((float)u);
    if (fabs(s) > worst_range) worst_range = fabs(s);
    if (u <= 0.5) {
      const double a = (double)omx_lfo_shape((float)(0.5 + u));
      const double b = (double)omx_lfo_shape((float)(0.5 - u));
      if (fabs(a + b) > worst_sym) worst_sym = fabs(a + b);
    }
    const double e = fabs(s + sin(2.0 * M_PI * u));
    if (e > worst_sine) worst_sine = e;
  }
  ok(worst_range <= 1.0, "the shape never leaves [-1, 1]", worst_range, 1.0);
  ok(worst_sym <= 1e-6, "the shape is odd about the half turn", worst_sym, 1e-6);
  ok(fabs((double)omx_lfo_shape(0.5f)) <= 1e-7, "the half turn is back at zero (1e-7)",
     omx_lfo_shape(0.5f), 0.0);
  ok(worst_sine <= 0.0561, "the parabola is -sin(2 pi u) to within 0.0561 (10001 points)",
     worst_sine, 0.0561);
  ok(worst_sine >= 0.050, "and it IS the parabola, not the sine (deviation above 0.05)",
     worst_sine, 0.050);

  /* THE INCREMENT (engine arm B), at the nine RME rates: rate/sr turns per sample to 1e-6
   * relative at 0.05, 0.6 and 8 Hz; zero, negative, Nyquist and above freeze; and so does a
   * non-positive sample rate. omx_lfo_inc carries no rate-is-declared precondition, so the
   * ledger stays empty at 32, 64 and 128 kHz too. */
  static const double INC_HZ[3] = {0.05, 0.6, 8.0};
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    for (int k = 0; k < 3; k++) {
      const double want = INC_HZ[k] / (double)sr;
      const double got = (double)omx_lfo_inc((float)INC_HZ[k], sr);
      ok(fabs(got - want) <= want * 1e-6, "the increment is rate/sr turns to 1e-6, RME rates",
         got, want);
    }
    ok(omx_lfo_inc(0.0f, sr) == 0.0f && omx_lfo_inc(-1.0f, sr) == 0.0f,
       "a non-positive rate freezes, RME rates", omx_lfo_inc(-1.0f, sr), 0.0);
    ok(omx_lfo_inc(sr * 0.5f, sr) == 0.0f && omx_lfo_inc(sr, sr) == 0.0f,
       "a rate at or above Nyquist freezes, never aliased, RME rates", omx_lfo_inc(sr, sr), 0.0);
  }
  ok(omx_lfo_inc(1.0f, 0.0f) == 0.0f, "no sample rate at all freezes", omx_lfo_inc(1.0f, 0.0f),
     0.0);

  /* THE ACCUMULATOR (engine arm C, there one minute at 96 kHz; here one minute at each of the
   * nine RME rates): at 0.6 Hz the phase never leaves [0, 1), and its drift from the exact
   * k * inc mod 1 (the wrap is not a drift), sampled every 4096 advances, stays under 0.05 turns.
   * Then 10 Hz for sr/10 samples (one period) lands within 1e-3 of the start. */
  double worst_drift = 0.0, worst_period = 0.0;
  int inside_min = 1;
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    struct omx_lfo m = {0.0f, omx_lfo_inc(0.6f, sr), 0.0f};
    const double inc = (double)m.inc;
    const uint32_t n = (uint32_t)((double)sr * 60.0);
    for (uint32_t i = 1; i <= n; i++) {
      omx_lfo_advance(&m);
      if (!(m.phase >= 0.0f && m.phase < 1.0f)) inside_min = 0;
      if ((i % 4096u) == 0u) {
        double d = fabs((double)m.phase - fmod((double)i * inc, 1.0));
        if (d > 0.5) d = 1.0 - d;
        if (d > worst_drift) worst_drift = d;
      }
    }
    struct omx_lfo p = {0.0f, omx_lfo_inc(10.0f, sr), 0.0f};
    const uint32_t period = (uint32_t)((double)sr / 10.0);
    for (uint32_t i = 0; i < period; i++) omx_lfo_advance(&p);
    const double off = p.phase > 0.5f ? 1.0 - (double)p.phase : (double)p.phase;
    if (off > worst_period) worst_period = off;
  }
  ok(inside_min, "a phase never leaves its turn over a minute, RME rates", 0.0, 0.0);
  ok(worst_drift <= 0.05, "the phase drifts from k*inc mod 1 by under 0.05 turns a minute",
     worst_drift, 0.05);
  ok(worst_period < 1e-3, "one period of samples at 10 Hz is one turn, RME rates", worst_period,
     1e-3);

  /* THE OFFSET READ (engine arm D, 96 kHz: a phase walk, not a rate property): over 1000
   * advancing phases, omx_lfo_at(l, k/4) is exactly shape(wrap(phase + k/4)); and after a million
   * advances at 1.1 Hz three reads at k/3 are still a third of a turn apart to 1e-6, which two
   * separately advanced accumulators would not be. */
  struct omx_lfo w = {0.0f, omx_lfo_inc(0.6f, 96000.0f), 0.0f};
  double worst_at = 0.0;
  for (int i = 0; i < 1000; i++) {
    for (int k = 0; k < 4; k++) {
      const float off = (float)k / 4.0f;
      const double want = (double)omx_lfo_shape(omx_lfo_wrap(w.phase + off));
      const double e = fabs((double)omx_lfo_at(&w, off) - want);
      if (e > worst_at) worst_at = e;
    }
    omx_lfo_advance(&w);
  }
  ok(worst_at == 0.0, "a read at an offset IS the shape at the offset phase, 1000 phases",
     worst_at, 0.0);
  struct omx_lfo v = {0.0f, omx_lfo_inc(1.1f, 96000.0f), 0.0f};
  for (uint32_t i = 0; i < 1000000u; i++) omx_lfo_advance(&v);
  double sep_worst = 0.0;
  for (int k = 0; k < 3; k++) {
    double sep = (double)omx_lfo_wrap(v.phase + (float)k / 3.0f) - (double)v.phase;
    if (sep < 0.0) sep += 1.0;
    double e = fabs(sep - (double)k / 3.0);
    if (e > 0.5) e = 1.0 - e;
    if (e > sep_worst) sep_worst = e;
  }
  ok(sep_worst <= 1e-6, "three voices stay a third of a turn apart after a million samples",
     sep_worst, 1e-6);

  /* THE SWEEP (engine arm F; 1 ms base and 4 ms depth in samples at 96 kHz): -1 is base, +1 is
   * base + depth to 1e-3, monotone over 2001 shape values, never below base, and no depth is no
   * sweep. */
  const float base = 96.0f, depth = 384.0f;
  ok(omx_lfo_sweep(base, depth, -1.0f) == base, "the sweep's trough is the base",
     omx_lfo_sweep(base, depth, -1.0f), base);
  ok(fabs((double)omx_lfo_sweep(base, depth, 1.0f) - (double)(base + depth)) <= 1e-3,
     "the sweep's crest is base + depth", omx_lfo_sweep(base, depth, 1.0f), base + depth);
  double prev = -1.0;
  int monotone = 1, floored = 1;
  for (int i = 0; i <= 2000; i++) {
    const float s = (float)(-1.0 + 2.0 * (double)i / 2000.0);
    const double d = (double)omx_lfo_sweep(base, depth, s);
    if (d < prev) monotone = 0;
    if (d < (double)base - 1e-6) floored = 0;
    prev = d;
  }
  ok(monotone, "the sweep rises with the shape", 0.0, 0.0);
  ok(floored, "and never asks for less than the base", 0.0, 0.0);
  ok(omx_lfo_sweep(base, 0.0f, 0.3f) == base, "no depth is no sweep",
     omx_lfo_sweep(base, 0.0f, 0.3f), base);
  expect_clean();
}
