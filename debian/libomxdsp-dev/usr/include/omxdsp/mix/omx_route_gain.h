// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_route_gain.h
 * @brief The gain stages of an output route: a leg copied at a ramped gain, a stereo bus folded
 *        into one mono buffer at a ramped gain, and a stereo bus folded in place onto both legs.
 *
 * A route carries one bus to one destination at the route's own gain × mute, and a mono
 * destination takes the bus's two legs summed, `(L + R)·c`, where `c` already folds the per-leg
 * fold coefficient (0.5 by default, so a correlated pair lands at unity, not +6 dB). Every gain
 * moves by a click-free linear ramp from the caller's cursor to the target across the block. The
 * engine keeps the cursors, the route selection and the delay lines; these are the arithmetic.
 * Moved from the engine's `omx_fill_gain`, `omx_fold_gain` and `omx_fold_fill` (OpenMixer's
 * mix_dsp.h), bit for bit: the same operations in the same order, including omx_route_fold_fill()'s
 * own accumulated step where the other two use omx_ramp.h.
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_route_gain.h>.
 */
#ifndef OMX_ROUTE_GAIN_H
#define OMX_ROUTE_GAIN_H

#include <stdint.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_ramp.h>

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "route/fill-gain"
/**
 * @brief Copy `in` to `out` at a gain ramped linearly from `*cur` to `tgt` across the block:
 *        `out[i] = in[i]·(cur + i·(tgt − cur)/n)`.
 *
 * The first stage of a strip (the head gain before the dynamics) and a route's leg copy alike. A
 * negative target is a polarity invert, and a move between `+g` and `−g` ramps through 0.
 * @param in The source, `n` samples.
 * @param out The destination, `n` samples; `out == in` is allowed (in place).
 * @param n Samples; 0 is a no-op that leaves `*cur` alone.
 * @param tgt The gain at the end of the block; a non-finite target reads as 0 (omx_ramp_begin()).
 * @param cur In: the gain applied at the end of the previous block. Out: `tgt`.
 * @pre `finite-in`.
 * @post `finite-out`.
 * @note RT-safe: one pass, no allocation, no branch in the loop. Thread-safe on disjoint buffers.
 */
static inline void omx_route_fill_gain(const float *in, float *out, uint32_t n, float tgt, float *cur) {
  if (n == 0) return;
  OMX_PRE(omx_block_finite(in, n), "finite-in");
  const struct omx_ramp r = omx_ramp_begin(cur, tgt, n);
  for (uint32_t i = 0; i < n; i++) out[i] = in[i] * omx_ramp_at(r, i);
  omx_ramp_end(r, cur);
  OMX_POST(omx_block_finite(out, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "route/fold-gain"
/**
 * @brief Fold a stereo bus into ONE mono buffer at a ramped gain:
 *        `out[i] = (inL[i] + inR[i])·(cur + i·(tgt − cur)/n)`.
 *
 * `tgt` already folds the route's gain × mute × the per-leg fold coefficient. A correlated pair
 * folded at 0.5 lands exactly on its own value: `(x + x)·0.5` is exact in binary floating point.
 * @param inL The left leg, `n` samples.
 * @param inR The right leg, `n` samples.
 * @param out The mono destination, `n` samples, distinct from both legs.
 * @param n Samples; 0 is a no-op that leaves `*cur` alone.
 * @param tgt The fold gain at the end of the block; a non-finite target reads as 0.
 * @param cur In: the gain at the end of the previous block. Out: `tgt`.
 * @post `finite-out`.
 * @note RT-safe: one pass, no allocation, no branch in the loop. Thread-safe on disjoint buffers.
 */
static inline void omx_route_fold_gain(const float *inL, const float *inR, float *restrict out, uint32_t n,
                                       float tgt, float *cur) {
  if (n == 0) return;
  const struct omx_ramp r = omx_ramp_begin(cur, tgt, n);
  for (uint32_t i = 0; i < n; i++) out[i] = (inL[i] + inR[i]) * omx_ramp_at(r, i);
  omx_ramp_end(r, cur);
  OMX_POST(omx_block_finite(out, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "route/fold-fill"
/**
 * @brief Fold a stereo bus IN PLACE onto both of its legs at a ramped gain: each leg becomes
 *        `(a + b)·c`, `a`, `b` read before either is written, `c` stepped from that leg's cursor
 *        toward `tgt` by `(tgt − cur)/n` per sample.
 *
 * The in-place counterpart of omx_route_fold_gain(): writing both legs means whichever port is
 * linked to a mono sink carries the fold. Two cursors, so the second leg can be enabled
 * click-free. The step is ACCUMULATED (`c += step` after each sample), not omx_ramp_at()'s
 * `cur + step·i`, so the two differ in the last bits on a moving gain; neither heals a non-finite
 * cursor. On return both cursors are `tgt`.
 * @param busL The left leg, `n` samples, overwritten with the fold.
 * @param busR The right leg, `n` samples, overwritten with the fold.
 * @param n Samples; 0 is a no-op that leaves both cursors alone.
 * @param tgt The fold gain at the end of the block.
 * @param curL In: the left leg's gain at the end of the previous block. Out: `tgt`.
 * @param curR In: the right leg's gain at the end of the previous block. Out: `tgt`.
 * @pre `finite-in`.
 * @post `finite-out`.
 * @note RT-safe: one pass, no allocation. Thread-safe on disjoint buses.
 */
static inline void omx_route_fold_fill(float *busL, float *busR, uint32_t n, float tgt, float *curL,
                                       float *curR) {
  if (n == 0) return;
  OMX_PRE(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-in");
  float cl = *curL, cr = *curR;
  float stepL = (tgt - cl) / (float)n;
  float stepR = (tgt - cr) / (float)n;
  for (uint32_t i = 0; i < n; i++) {
    float a = busL[i];
    float b = busR[i];
    float folded = a + b;
    busL[i] = folded * cl;
    busR[i] = folded * cr;
    cl += stepL;
    cr += stepR;
  }
  *curL = tgt;
  *curR = tgt;
  OMX_POST(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ROUTE_GAIN_H */
