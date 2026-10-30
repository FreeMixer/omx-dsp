// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_bus_sum.h
 * @brief One source's contribution to a summing bus, as entries of the mix matrix G: the level
 *        split across the legs by the source's image, a mono bus's fold of both legs into one,
 *        and the silent-and-settled skip.
 *
 * A bus is `bus[n] = Σ coef(s) · tap(s)[n]`. Each contribution here is one or two entries of G
 * (`struct omx_mixmatrix_entry`, omx_mixmatrix.h): the coefficient at the start of the block
 * (the leg's ramp cursor) and at its end (the target), every non-finite end taken as 0. The
 * entries are ACCUMULATED into a bus the caller has zeroed (`bus[i] += g(i) · x[i]`), with the
 * ramp `g(i) = g_prev + step · i` that omx_ramp.h and omx_mixmatrix_sparse() both use, so a caller
 * that collects the entries of a whole bus may run them through omx_mixmatrix_sparse() instead.
 *
 * The engine keeps the summing structure: which sources feed which bus, the zeroing of the
 * accumulator once per cycle, the ramp cursors, and the walk over a bus's source table. Moved
 * from the engine's `omx_bus_add_ramp` (OpenMixer's mix_bus.h) and the stereo summing loop it
 * calls, `omx_mix_strip` (its mix_dsp.h), bit for bit.
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_bus_sum.h>.
 */
#ifndef OMX_BUS_SUM_H
#define OMX_BUS_SUM_H

#include <math.h>
#include <stdint.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_mixmatrix.h>

/** @brief How a contribution's width resolves into the bus it feeds. */
enum omx_bus_sum_width {
  OMX_BUS_SUM_FOLLOW = 0, /**< A stereo bus: the source keeps its image, one entry per leg. */
  OMX_BUS_SUM_FOLD = 1,   /**< A mono bus: both legs sum down its one leg at the fold coefficient. */
};

/**
 * @brief One entry of G for a block: the ramp from a leg's cursor to its target.
 * @param strip The source's row of X.
 * @param out The bus leg's row of Y.
 * @param cur The coefficient applied at the end of the previous block; non-finite is taken as 0.
 * @param tgt The coefficient at the end of this block; non-finite is taken as 0.
 * @return The entry: `g_prev` the finite cursor, `g_cur` the finite target.
 * @note RT-safe, pure. A non-finite coefficient mutes the contribution instead of turning the
 *       whole accumulator to NaN, and the next block starts from a finite cursor.
 */
static inline struct omx_mixmatrix_entry omx_bus_sum_entry(uint32_t strip, uint32_t out, float cur,
                                                           float tgt) {
  struct omx_mixmatrix_entry e;
  e.strip = strip;
  e.out = out;
  e.g_prev = isfinite(cur) ? cur : 0.0f;
  e.g_cur = isfinite(tgt) ? tgt : 0.0f;
  return e;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "bus-sum/accumulate"
/**
 * @brief Accumulate one entry into one bus leg: `y[i] += x[i] · (g_prev + step · i)`,
 *        `step = (g_cur − g_prev) / n`.
 * @param y The bus leg, `n` samples, added to.
 * @param x The source leg, `n` samples.
 * @param n Samples; at least 1.
 * @param e The entry (omx_bus_sum_entry()); a non-finite `g_prev` or `g_cur` is taken as 0.
 * @pre `finite-tap`, `finite-accumulator-in`.
 * @post `finite-accumulator-out`.
 * @note RT-safe: one pass, no branch in the loop, the ramp affine in `i` so the loop vectorises.
 *       A NaN or infinite coefficient mutes the contribution; it never turns the accumulator to NaN.
 */
static inline void omx_bus_sum_accumulate(float *restrict y, const float *x, uint32_t n,
                                          const struct omx_mixmatrix_entry *e) {
  OMX_PRE(omx_block_finite(x, n), "finite-tap");
  OMX_PRE(omx_block_finite(y, n), "finite-accumulator-in");
  const float g0 = isfinite(e->g_prev) ? e->g_prev : 0.0f;
  const float g1 = isfinite(e->g_cur) ? e->g_cur : 0.0f;
  const float step = (g1 - g0) / (float)n;
  for (uint32_t i = 0; i < n; i++) y[i] += x[i] * (g0 + step * (float)i);
  OMX_POST(omx_block_finite(y, n), "finite-accumulator-out");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "bus-sum/accumulate-fold"
/**
 * @brief Accumulate one entry over the sum of two source legs into one bus leg:
 *        `y[i] += (a[i] + b[i]) · (g_prev + step · i)`.
 * @param y The mono bus leg, `n` samples, added to.
 * @param a The source's left leg.
 * @param b The source's right leg; a mono source passes `a` again.
 * @param n Samples; at least 1.
 * @param e The entry: the fold's coefficient, already level × fold coefficient; a non-finite
 *        `g_prev` or `g_cur` is taken as 0.
 * @post `finite-accumulator-out`.
 * @note RT-safe: one pass, no branch in the loop.
 */
static inline void omx_bus_sum_accumulate_fold(float *restrict y, const float *a, const float *b,
                                               uint32_t n, const struct omx_mixmatrix_entry *e) {
  const float g0 = isfinite(e->g_prev) ? e->g_prev : 0.0f;
  const float g1 = isfinite(e->g_cur) ? e->g_cur : 0.0f;
  const float step = (g1 - g0) / (float)n;
  for (uint32_t i = 0; i < n; i++) y[i] += (a[i] + b[i]) * (g0 + step * (float)i);
  OMX_POST(omx_block_finite(y, n), "finite-accumulator-out");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "bus-sum/add"
/**
 * @brief Fold one source into a bus, honouring the bus's width, click-free.
 *
 * A contribution whose level is 0 and whose cursors have both settled at 0 is skipped: it costs
 * one compare and writes nothing. One still ramping towards 0 is summed, because cutting a ramp
 * short is a click.
 *
 * Into a stereo bus the source keeps its image: one entry per leg, `level × imgL` and
 * `level × imgR`. Into a mono bus an image means nothing: one entry at `level × fold_coef` over
 * the sum of both legs, so a correlated source with a fold coefficient of 0.5 lands at unity,
 * and both cursors take its target.
 * @param inL The source's left leg, `n` samples.
 * @param inR The source's right leg; a mono source passes `inL` again.
 * @param busL The bus's left (or only) leg, added to.
 * @param busR The bus's right leg, added to; untouched by a mono bus, which may pass NULL.
 * @param n Samples per leg; 0 is a no-op.
 * @param level The contribution's level, linear.
 * @param imgL The image's left gain, [-1, 1]; 1 for a plain send.
 * @param imgR The image's right gain, [-1, 1]; 1 for a plain send.
 * @param cur_l In: the left cursor. Out: the left target (the fold's target on a mono bus).
 * @param cur_r In: the right cursor. Out: the right target (the fold's target on a mono bus).
 * @param w The bus's width.
 * @param fold_coef The fold's per-leg gain; read only when `w` is #OMX_BUS_SUM_FOLD.
 * @return 1 when samples were written, 0 when the contribution was skipped.
 * @pre `finite-image`, `image-is-a-gain`.
 * @note RT-safe: one or two passes over the block, no allocation. Thread-safe on disjoint buses.
 */
static inline int omx_bus_sum_add(const float *inL, const float *inR, float *restrict busL,
                                  float *restrict busR, uint32_t n, float level, float imgL,
                                  float imgR, float *cur_l, float *cur_r, enum omx_bus_sum_width w,
                                  float fold_coef) {
  OMX_PRE(imgL - imgL == 0.0f && imgR - imgR == 0.0f, "finite-image");
  OMX_PRE(imgL >= -1.0f && imgL <= 1.0f && imgR >= -1.0f && imgR <= 1.0f, "image-is-a-gain");
  if (n == 0) return 0;
  if (level == 0.0f && *cur_l == 0.0f && *cur_r == 0.0f) return 0;
  if (w == OMX_BUS_SUM_FOLD) {
    const struct omx_mixmatrix_entry e = omx_bus_sum_entry(0u, 0u, *cur_l, level * fold_coef);
    omx_bus_sum_accumulate_fold(busL, inL, inR, n, &e);
    *cur_l = e.g_cur;
    *cur_r = e.g_cur;
    return 1;
  }
  const struct omx_mixmatrix_entry el = omx_bus_sum_entry(0u, 0u, *cur_l, level * imgL);
  const struct omx_mixmatrix_entry er = omx_bus_sum_entry(0u, 1u, *cur_r, level * imgR);
  omx_bus_sum_accumulate(busL, inL, n, &el);
  omx_bus_sum_accumulate(busR, inR, n, &er);
  *cur_l = el.g_cur;
  *cur_r = er.g_cur;
  return 1;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_BUS_SUM_H */
