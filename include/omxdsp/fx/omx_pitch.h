// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_pitch.h
 * @brief The native PITCH SHIFTER: one fractional delay line per leg read twice, half a ramp apart,
 *        crossfaded by the ramp's own phase.
 *
 * Spec: docs/design/specs/2026-09-22-native-pitch-shift.md (§1, §3, §8, §10, §11). Composed from
 * libomxdsp words only: `omx_fdelay.h` at order 3, `omx_lfo.h` (its parabola is the crossfade;
 * the ramp is a fixed-point turn counter, spec §11), `omx_eq_design.h`'s low-pass and `omx_biquad.h` (the pre-filter),
 * `omx_denormal.h`. Every declared number is read from `OMX_PITCH_*` (packages/core/src/pitch-kernel.ts).
 *
 *     d_A = d_min + W·p          (down)      d_A = d_min + W·(1 − p)          (up)
 *     d_B = d_min + W·wrap(p+½)  (down)      d_B = d_min + W·(1 − wrap(p+½))  (up)
 *     y   = (1 − mix)·x + mix·(w_A·line(d_A) + w_B·line(d_B)),   w_A + w_B = 1
 *
 * NO PipeWire, NO napi, NO allocation: the rings are the caller's, allocated on insert.
 */
#ifndef OMX_MIX_PITCH_H
#define OMX_MIX_PITCH_H

#include <math.h>
#include <stdint.h>

#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_fdelay.h>
#include <omxdsp/omx_lfo.h>
#include <omxdsp/omx_param.h>

/** The Lagrange order the shifter's lines are armed at (spec §1). */
#define OMX_PITCH_ORDER OMX_FDELAY_MOD_READ_ORDER

/**
 * @brief The resolved control atom, built once per block from the row's controls: every time in
 *        samples, every rate in turns per sample, the pre-filter designed.
 */
struct omx_pitch {
  int enabled;  /**< 0 → passthrough, before a sample or a state word is touched. */
  int dir;      /**< −1 the ratio is below 1, +1 above, 0 exactly 1 (the ramp freezes). */
  float window; /**< W, samples. */
  uint32_t step; /**< The ramp's increment, round(2^32·|1 − r|/W): fixed-point turns per sample. */
  float mix;    /**< 0 = dry (identity), 1 = wet only. */
  float lp[5];  /**< The pre-filter, {b0, b1, b2, a1, a2}. */
};

/** @brief Caller-owned rings and RT-owned state. */
struct omx_pitch_state {
  struct omx_fdelay line_l; /**< The left leg's line, order 3. */
  struct omx_fdelay line_r; /**< The right leg's line, same geometry. */
  uint32_t ramp;            /**< The ramp's phase p, fixed-point turns (2^32 = one turn). */
  int dir;                  /**< The direction p is expressed in, −1 or +1. */
  float lp_l[4];            /**< The left pre-filter's state. */
  float lp_r[4];            /**< The right pre-filter's state. */
};

/**
 * @brief Ring capacity per leg at `sr`: the longest tap, d_min + W, plus the kernel's reach.
 * @param sr Sample rate, Hz, positive.
 * @return Floats per leg.
 * @note Control thread: called on insert and on a rate change.
 */
static inline uint32_t omx_pitch_cap_for(float sr) {
  const float w = OMX_PITCH_WINDOW_MS * 1e-3f * sr;
  return omx_fdelay_cap_for(omx_fdelay_min_delay(OMX_PITCH_ORDER) + w, OMX_PITCH_ORDER);
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "pitch/init"
/**
 * @brief Arm both legs over caller-owned rings; the ramp starts at phase 0, going down.
 * @param s The state.
 * @param ring_l The left ring, `cap` floats.
 * @param ring_r The right ring, `cap` floats.
 * @param cap Floats per ring, from omx_pitch_cap_for().
 * @return The line's own code.
 * @post `an-armed-shifter-reads-at-order-three`.
 * @note Control thread.
 */
static inline enum omx_fdelay_code omx_pitch_state_init(struct omx_pitch_state *s, float *ring_l,
                                                        float *ring_r, uint32_t cap) {
  s->ramp = 0u;
  s->dir = -1;
  for (int k = 0; k < 4; k++) s->lp_l[k] = s->lp_r[k] = 0.0f;
  enum omx_fdelay_code c = omx_fdelay_init(&s->line_l, ring_l, cap, OMX_PITCH_ORDER);
  if (c != OMX_FDELAY_OK) return c;
  c = omx_fdelay_init(&s->line_r, ring_r, cap, OMX_PITCH_ORDER);
  OMX_POST(c != OMX_FDELAY_OK || s->line_l.order == OMX_PITCH_ORDER,
           "an-armed-shifter-reads-at-order-three");
  return c;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The shift ratio of a control pair, each clamped to its declared travel.
 * @param semitones Coarse shift, semitones.
 * @param cents Fine shift, cents.
 * @return r = 2^((semitones + cents/100)/12).
 * @note Resolve-time: one exp2f.
 */
static inline float omx_pitch_ratio(float semitones, float cents) {
  const float st = fminf(fmaxf(semitones, OMX_PITCH_SEMITONES_MIN), OMX_PITCH_SEMITONES_MAX);
  const float ct = fminf(fmaxf(cents, OMX_PITCH_CENTS_MIN), OMX_PITCH_CENTS_MAX);
  return exp2f((st + ct / 100.0f) / 12.0f);
}

#define OMX_CONTRACT_STAGE "pitch/resolve"
/**
 * @brief Resolve a ratio into the atom at `sr`: the window, the ramp's increment and direction,
 *        and the pre-filter at min(ceiling, sr/(2r)).
 * @param p The atom.
 * @param enabled The stage's switch.
 * @param r The shift ratio, positive.
 * @param mix The wet share, 0..1.
 * @param sr Sample rate, Hz, positive.
 * @pre `a-ratio-is-positive`.
 * @post `an-increment-is-inside-half-a-turn`.
 * @note The increment is fixed-point (spec §11), so the ramp adds and wraps exactly.
 *       Resolve-time, once per block: omx_eq_design_f's libm calls, no allocation.
 */
static inline void omx_pitch_resolve_ratio(struct omx_pitch *p, int enabled, float r, float mix,
                                           float sr) {
  OMX_PRE(r > 0.0f && sr > 0.0f, "a-ratio-is-positive");
  p->enabled = enabled;
  p->window = OMX_PITCH_WINDOW_MS * 1e-3f * sr;
  p->dir = r < 1.0f ? -1 : (r > 1.0f ? 1 : 0);
  const double inc = fabs(1.0 - (double)r) / ((double)OMX_PITCH_WINDOW_MS * 1e-3 * (double)sr);
  p->step = inc < 0.5 ? (uint32_t)llround(ldexp(inc, 32)) : 0u;
  p->mix = omx_unit(mix);
  const double fc = fmin((double)OMX_PITCH_PREFILTER_CEILING_HZ, (double)sr / (2.0 * (double)r));
  omx_eq_design_f(OMX_EQ_LOWPASS, fc, (double)OMX_PITCH_PREFILTER_Q, 0.0, (double)sr, p->lp);
  OMX_POST(p->step < 0x80000000u, "an-increment-is-inside-half-a-turn");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Resolve the row's controls: semitones and cents on their travel, mix in percent.
 * @param p The atom.
 * @param enabled The stage's switch.
 * @param semitones Coarse shift.
 * @param cents Fine shift.
 * @param mix_pct Wet share, percent.
 * @param sr Sample rate, Hz.
 * @note Resolve-time.
 */
static inline void omx_pitch_resolve(struct omx_pitch *p, int enabled, float semitones, float cents,
                                     float mix_pct, float sr) {
  const float m = omx_clampf(mix_pct, OMX_PITCH_MIX_MIN, OMX_PITCH_MIX_MAX) / OMX_PITCH_MIX_MAX;
  omx_pitch_resolve_ratio(p, enabled, omx_pitch_ratio(semitones, cents), m, sr);
}

#define OMX_CONTRACT_STAGE "pitch/tap"
/**
 * @brief A fixed-point phase as a float turn in [0, 1), exact: its top 24 bits.
 * @param x The phase, 2^32 = one turn.
 * @return The turn.
 * @note RT-safe and thread-safe: a shift and a multiply.
 */
static inline float omx_pitch_turns(uint32_t x) { return (float)(x >> 8) * 0x1p-24f; }

/**
 * @brief The delay a tap reads at ramp phase `phase`, in direction `dir`.
 * @param window W, samples.
 * @param phase The tap's own phase (the ramp's, plus ½ turn for tap B), fixed-point turns.
 * @param dir −1 or +1.
 * @return The delay, samples, in [d_min, d_min + W].
 * @post `a-tap-is-inside-the-window`.
 * @note RT-safe and thread-safe: pure.
 */
static inline float omx_pitch_tap_delay(float window, uint32_t phase, int dir) {
  const float v = omx_pitch_turns(phase);
  const float dmin = omx_fdelay_min_delay(OMX_PITCH_ORDER);
  const float d = dmin + window * (dir > 0 ? 1.0f - v : v);
  OMX_POST(d >= dmin && d <= dmin + window, "a-tap-is-inside-the-window");
  return d;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "pitch/frame"
/**
 * @brief One stereo frame in place: filter, write, two shared-split taps crossfaded, the ramp
 *        advanced one step.
 * @param p The resolved atom.
 * @param s The state, its direction already the atom's.
 * @param yl The left sample, replaced by the output.
 * @param yr The right sample, replaced by the output.
 * @note RT-safe: two biquads, two writes, two pair reads, two parabola reads, one integer add.
 *       Thread-safe on distinct state.
 */
static inline void omx_pitch_frame(const struct omx_pitch *p, struct omx_pitch_state *s, float *yl,
                                   float *yr) {
  const float in_l = *yl, in_r = *yr;
  s->lp_l[2] = omx_flush(omx_biquad(in_l, p->lp, s->lp_l));
  s->lp_r[2] = omx_flush(omx_biquad(in_r, p->lp, s->lp_r));
  omx_fdelay_write(&s->line_l, s->lp_l[2]);
  omx_fdelay_write(&s->line_r, s->lp_r[2]);
  float wet_l = 0.0f, wet_r = 0.0f;
  for (int tap = 0; tap < 2; tap++) {
    const uint32_t ph = s->ramp + (tap ? 0x80000000u : 0u);
    const float w = omx_lfo_sweep(0.0f, 1.0f, omx_lfo_shape(omx_pitch_turns(ph + 0x40000000u)));
    float tap_l = 0.0f, tap_r = 0.0f;
    omx_fdelay_read_pair(&s->line_l, &s->line_r, omx_pitch_tap_delay(p->window, ph, s->dir), &tap_l,
                         &tap_r);
    wet_l += w * tap_l;
    wet_r += w * tap_r;
  }
  *yl = (1.0f - p->mix) * in_l + p->mix * wet_l;
  *yr = (1.0f - p->mix) * in_r + p->mix * wet_r;
  s->ramp += p->step;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "pitch"
/**
 * @brief Process one block in place on both legs; both legs run ONE ramp phase.
 * @param l The left leg.
 * @param r The right leg, a distinct buffer.
 * @param n Frames.
 * @param p The resolved atom.
 * @param s The state.
 * @pre `finite-in`, `mix-in-unit-range`, `an-increment-is-inside-half-a-turn`,
 *      `the-longest-tap-is-inside-the-ring`.
 * @post `finite-out`.
 * @note RT-safe: omx_pitch_frame per frame; no allocation, no libm call. Thread-safe on distinct
 *       state.
 */
static inline void omx_pitch_process(float *l, float *r, uint32_t n, const struct omx_pitch *p,
                                     struct omx_pitch_state *s) {
  OMX_PRE(omx_block_pair_finite(l, r, n), "finite-in");
  OMX_PRE(p->mix >= 0.0f && p->mix <= 1.0f, "mix-in-unit-range");
  OMX_PRE(p->step < 0x80000000u, "an-increment-is-inside-half-a-turn");
  OMX_PRE(omx_fdelay_fits_ring(&s->line_l, p->enabled,
                               omx_fdelay_min_delay(OMX_PITCH_ORDER) + p->window),
          "the-longest-tap-is-inside-the-ring");
  if (!omx_fdelay_pair_live(&s->line_l, &s->line_r, p->enabled, n, p->mix)) return;
  if (p->dir != 0 && p->dir != s->dir) {
    s->ramp = 0u - s->ramp;
    s->dir = p->dir;
  }
  for (uint32_t i = 0; i < n; i++) omx_pitch_frame(p, s, &l[i], &r[i]);
  OMX_POST(omx_block_pair_finite(l, r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_PITCH_H */
