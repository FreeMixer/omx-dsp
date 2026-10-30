// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_duck.h
 * @brief The wet ducker: an effect's return pulled down while its dry key is loud, so repeats and
 *        tails sit in the gaps of the phrase.
 *
 * The key is the effect's own dry input. Its level is a one-pole peak follower, `a` the pole while
 * the level rises and `r` while it falls; the wet gain, in dB, is the infinite-ratio law floored at
 * the depth:
 *
 *     g = max(depth, min(0, T − L))
 *
 * so the wet is untouched while the key is at or below the threshold `T`, falls dB for dB as the key
 * rises past it, and never below `depth`. A depth of 0 dB is no ducking: the wet passes bit for bit
 * and no state word moves.
 *
 * Contract: no allocation, no lock, no libm call per sample (the level is read through
 * omx_lin_to_db_poly(), the gain through omx_db_to_lin_poly()). The state is one float.
 */
#ifndef OMX_DUCK_H
#define OMX_DUCK_H

#include <math.h>
#include <stdint.h>

#include "omx_contract.h"
#include "omx_denormal.h"
#include "omx_units.h"

/** @brief A ducker's resolved controls. */
struct omx_duck {
  float thresh_db; /**< The key level the ducking starts at, dBFS. */
  float depth_db;  /**< The deepest the wet goes, dB, at most 0; 0 is no ducking. */
  float attack;    /**< The follower's pole while the key rises, in [0, 1). */
  float release;   /**< The follower's pole while the key falls, in [0, 1). */
};

/** @brief A ducker's state: the key's level, linear. */
struct omx_duck_state {
  float level; /**< The peak follower, linear amplitude. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "duck/gain-db"
/**
 * @brief The wet gain at key level `level`: `max(depth, min(0, T − L))`, dB.
 * @param d The controls.
 * @param level The key level, linear, not negative.
 * @return The gain, dB, in `[depth, 0]`.
 * @post `gain-within-depth`.
 * @note RT-safe: no libm call. Thread-safe: pure.
 */
static inline float omx_duck_gain_db(const struct omx_duck *d, float level) {
  float g = d->thresh_db - omx_lin_to_db_poly(level);
  g = g > 0.0f ? 0.0f : g;
  g = g < d->depth_db ? d->depth_db : g;
  OMX_POST(g <= 0.0f && g >= d->depth_db, "gain-within-depth");
  return g;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "duck/step"
/**
 * @brief One key sample through the follower: the wet's linear gain for this sample.
 * @param d The controls; a depth below 0 dB.
 * @param st The state, the follower advanced.
 * @param key The key sample.
 * @return The linear gain, `10^(g/20)` with `g` from omx_duck_gain_db().
 * @note RT-safe: one follower step, no libm call. Thread-safe on distinct state.
 */
static inline float omx_duck_step(const struct omx_duck *d, struct omx_duck_state *st, float key) {
  const float k = fabsf(key);
  const float p = k > st->level ? d->attack : d->release;
  st->level = omx_flush(p * st->level + (1.0f - p) * k);
  return omx_db_to_lin_poly(omx_duck_gain_db(d, st->level));
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "duck/run"
/**
 * @brief Duck one block of wet signal by its dry key, in place.
 * @param d The controls.
 * @param st The state.
 * @param key The dry key, `n` samples (the louder leg, or a mono key).
 * @param wl The left wet leg, scaled in place.
 * @param wr The right wet leg, scaled in place, or NULL.
 * @param n Frames.
 * @pre `depth-not-positive`, `poles-in-unit-interval`, `finite-key`.
 * @post `finite-state`.
 * @note RT-safe: one follower step and one gain per sample, no libm call. A depth of 0 dB returns
 *       at once: no sample and no state word moves.
 */
static inline void omx_duck_run(const struct omx_duck *d, struct omx_duck_state *st, const float *key, float *wl,
                                float *wr, uint32_t n) {
  OMX_PRE(d->depth_db <= 0.0f, "depth-not-positive");
  OMX_PRE(d->attack >= 0.0f && d->attack < 1.0f && d->release >= 0.0f && d->release < 1.0f, "poles-in-unit-interval");
  OMX_PRE(omx_block_finite(key, n), "finite-key");
  if (!(d->depth_db < 0.0f)) return;
  for (uint32_t i = 0; i < n; i++) {
    const float g = omx_duck_step(d, st, key[i]);
    wl[i] *= g;
    if (wr) wr[i] *= g;
  }
  OMX_POST(st->level - st->level == 0.0f, "finite-state");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_DUCK_H */
