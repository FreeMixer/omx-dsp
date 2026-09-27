// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_divider.h
 * @brief The zero-crossing divider: a sign word `q` armed below `−h`, toggled at the next upward
 *        zero crossing, and the product `q·v`.
 *
 * For a steady `A·sin` the product has lines only at the half-integer multiples `(2j+1)·f/2`:
 * `8A/(3π)` at `f/2`, `8A/(π(2j+3)(2j−1))` above it, and exactly zero at `f`. The toggle lands on
 * the first sample at or after the continuous crossing, so the sampled product is the sampled
 * ideal product and its largest step never exceeds the input's own. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 14 and
 * docs/design/specs/2026-09-26-sub-octaver.md §1–§2, L2–L4.
 */
#ifndef OMX_DIVIDER_H
#define OMX_DIVIDER_H

#include <math.h>
#include <stdint.h>

#include "omxdsp.h"
#include "omx_contract.h"

/**
 * @brief The divider's state: the previous input sample, the arming word and the sign word.
 * @invariant `sign-word-is-unit`: `q` is exactly `−1` or `+1`; `armed` is 0 or 1.
 */
struct omx_divider {
  float prev;       /**< The previous input sample. */
  float q;          /**< The sign word, `−1` or `+1`. */
  uint32_t armed;   /**< 1 once the input has been below `−h` since the last toggle. */
  uint32_t toggles; /**< Toggles since init, modulo 2^32: a reader's period counter. */
};
OMXDSP_STATE_LAYOUT(divider, struct omx_divider)

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "divider/init"
/**
 * @brief Reset the divider: unarmed, `q = +1`, no previous sample, no toggle counted.
 * @param d The caller-owned state.
 * @post `sign-word-is-unit`.
 * @note RT-safe: four stores. Thread-safe on distinct state.
 */
static inline void omx_divider_init(struct omx_divider *d) {
  d->prev = 0.0f;
  d->q = 1.0f;
  d->armed = 0u;
  d->toggles = 0u;
  OMX_POST(d->q == 1.0f, "sign-word-is-unit");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "divider"
/**
 * @brief One sample: arm below `−h`, toggle `q` at the first upward crossing after arming,
 *        return `q·v`.
 * @param d The caller-owned state, updated in place.
 * @param v The input sample; finite.
 * @param h The hysteresis, `h ≥ 0`, in the input's own units.
 * @return `q·v`, whose magnitude is exactly `|v|`.
 * @pre `finite-in`, `hysteresis-non-negative`.
 * @post `output-magnitude-le-input`, `finite`.
 * @invariant `sign-word-is-unit`.
 * @note RT-safe: two comparisons and one multiply, no call. Thread-safe on distinct state.
 */
static inline float omx_divider_step(struct omx_divider *d, float v, float h) {
  OMX_PRE(v - v == 0.0f, "finite-in");
  OMX_PRE(h >= 0.0f && h - h == 0.0f, "hysteresis-non-negative");
  OMX_INVARIANT((d->q == 1.0f || d->q == -1.0f) && d->armed <= 1u, "sign-word-is-unit");
  if (v < -h) d->armed = 1u;
  if (d->armed && d->prev < 0.0f && v >= 0.0f) {
    d->q = -d->q;
    d->armed = 0u;
    d->toggles++;
  }
  d->prev = v;
  const float y = d->q * v;
  OMX_POST(fabsf(y) <= fabsf(v), "output-magnitude-le-input");
  OMX_POST(y - y == 0.0f, "finite");
  return y;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "divider/block"
/**
 * @brief A block of omx_divider_step() at one hysteresis; `in` and `out` may alias.
 * @param d The caller-owned state, updated in place.
 * @param in The input block, `n` finite samples.
 * @param out The output block, `n` samples.
 * @param n The block length; 0 is a no-op.
 * @param h The hysteresis for the whole block, `h ≥ 0`.
 * @pre `hysteresis-non-negative`.
 * @note RT-safe: `n` steps, no call outside this header. Thread-safe on distinct state.
 */
static inline void omx_divider_block(struct omx_divider *d, const float *in, float *out, uint32_t n,
                                     float h) {
  OMX_PRE(h >= 0.0f, "hysteresis-non-negative");
  for (uint32_t i = 0; i < n; i++) out[i] = omx_divider_step(d, in[i], h);
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_DIVIDER_H */
