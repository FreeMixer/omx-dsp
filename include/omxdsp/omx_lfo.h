// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_lfo.h
 * @brief The low-frequency oscillator: a phase accumulator in TURNS and one shape, the parabola
 *        `4t(1−|t|)`, `t = 2u−1`, which is −sin(2πu) to within 0.0561.
 *
 * One oscillator is read at N fixed offsets rather than advanced N times, so an ensemble's
 * voices cannot drift apart. Design: docs/design/specs/2026-09-22-chorus-and-flanger.md §1 and
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 12.
 */
#ifndef OMX_LFO_H
#define OMX_LFO_H

#include <stddef.h>

#include "omx_contract.h"

/** @brief One oscillator: a phase in turns, always in [0, 1), and its increment per sample. */
struct omx_lfo {
  float phase; /**< Turns, in [0, 1). */
  float inc;   /**< Turns per sample, in [0, 0.5); 0 is a legal, frozen oscillator. */
};

/**
 * @brief The state's size, for a host that lays the state out itself.
 * @return `sizeof(struct omx_lfo)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_lfo_state_size(void) { return sizeof(struct omx_lfo); }

/**
 * @brief The state's alignment, for a host that lays the state out itself.
 * @return `_Alignof(struct omx_lfo)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_lfo_state_align(void) { return _Alignof(struct omx_lfo); }

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "lfo/inc"
/**
 * @brief Turns per sample for a rate at a sample rate; a rate that cannot oscillate freezes.
 * @param rate_hz The rate, Hz, finite; non-positive, or at or above Nyquist, answers 0.
 * @param sr Sample rate, Hz; non-positive answers 0.
 * @return The increment, in [0, 0.5).
 * @pre `a-rate-is-a-finite-number-of-hz`.
 * @post `an-increment-is-inside-half-a-turn`.
 * @note RT-safe and thread-safe: one division.
 */
static inline float omx_lfo_inc(float rate_hz, float sr) {
  OMX_PRE(rate_hz - rate_hz == 0.0f, "a-rate-is-a-finite-number-of-hz");
  if (!(rate_hz > 0.0f) || !(sr > 0.0f)) return 0.0f;
  if (rate_hz * 2.0f >= sr) return 0.0f;
  const float inc = rate_hz / sr;
  OMX_POST(inc >= 0.0f && inc < 0.5f, "an-increment-is-inside-half-a-turn");
  return inc;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The shape at a phase: `4t(1−|t|)`, `t = 2·turns − 1`, in [−1, 1].
 * @param turns The phase, in [0, 1); the caller wraps.
 * @return The shape value.
 * @note RT-safe and thread-safe: three multiplies, no call.
 */
static inline float omx_lfo_shape(float turns) {
  const float t = 2.0f * turns - 1.0f;
  return 4.0f * t * (1.0f - (t < 0.0f ? -t : t));
}

/**
 * @brief Wrap a phase into [0, 1) by one subtraction; the input lies in [0, 2).
 * @param turns A phase plus an offset, each below 1.
 * @return The phase inside one turn.
 * @note RT-safe and thread-safe: two compares.
 */
static inline float omx_lfo_wrap(float turns) {
  float u = turns;
  if (u >= 1.0f) u -= 1.0f;
  if (u < 0.0f) u += 1.0f;
  return u;
}

#define OMX_CONTRACT_STAGE "lfo/at"
/**
 * @brief Read the oscillator `offset` turns ahead of its phase without moving it.
 * @param l The oscillator.
 * @param offset A fixed place in the ensemble, turns, in [0, 1); 0 reads the oscillator itself.
 * @return The shape, in [−1, 1].
 * @pre `a-phase-is-inside-one-turn`, `an-offset-is-inside-one-turn`.
 * @post `a-shape-is-inside-unity`.
 * @note RT-safe and thread-safe: a read of the state, no write.
 */
static inline float omx_lfo_at(const struct omx_lfo *l, float offset) {
  OMX_PRE(l->phase >= 0.0f && l->phase < 1.0f, "a-phase-is-inside-one-turn");
  OMX_PRE(offset >= 0.0f && offset < 1.0f, "an-offset-is-inside-one-turn");
  const float s = omx_lfo_shape(omx_lfo_wrap(l->phase + offset));
  OMX_POST(s >= -1.0f && s <= 1.0f, "a-shape-is-inside-unity");
  return s;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "lfo/advance"
/**
 * @brief Advance the oscillator one sample.
 * @param l The oscillator, its phase updated in place.
 * @pre `an-increment-is-inside-one-turn`.
 * @post `an-advanced-phase-is-still-inside-one-turn`.
 * @note RT-safe: one add and one wrap. Thread-safe on distinct state.
 */
static inline void omx_lfo_advance(struct omx_lfo *l) {
  OMX_PRE(l->inc >= 0.0f && l->inc < 1.0f, "an-increment-is-inside-one-turn");
  l->phase = omx_lfo_wrap(l->phase + l->inc);
  OMX_POST(l->phase >= 0.0f && l->phase < 1.0f, "an-advanced-phase-is-still-inside-one-turn");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "lfo/sweep"
/**
 * @brief The unipolar sweep `base + depth·(1 + s)/2`: upward from `base`, which is the shortest
 *        value the sweep reaches.
 * @param base The sweep's floor, in the caller's unit (samples, octaves).
 * @param depth The sweep's travel above the floor, same unit, non-negative.
 * @param s A shape value, in [−1, 1].
 * @return A value in [base, base + depth].
 * @pre `a-shape-is-inside-unity`, `a-depth-is-not-negative`.
 * @post `a-sweep-stays-inside-its-own-travel`.
 * @note RT-safe and thread-safe: two multiplies.
 */
static inline float omx_lfo_sweep(float base, float depth, float s) {
  OMX_PRE(s >= -1.0f && s <= 1.0f, "a-shape-is-inside-unity");
  OMX_PRE(depth >= 0.0f, "a-depth-is-not-negative");
  const float d = base + depth * 0.5f * (1.0f + s);
  OMX_POST(d >= base - 1e-6f && d <= base + depth + 1e-6f, "a-sweep-stays-inside-its-own-travel");
  return d;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_LFO_H */
