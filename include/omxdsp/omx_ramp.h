// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_ramp.h
 * @brief The per-block linear ramp: affine in the sample index, sanitised ends, exact endpoint.
 *
 * `g(i) = cur + step·i`, `step = (tgt − cur)/n`; a non-finite end reads as 0, and closing the
 * ramp stores the target itself, never the accumulated sum. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 19.
 */
#ifndef OMX_RAMP_H
#define OMX_RAMP_H

#include <math.h>
#include <stdint.h>

#include "omx_contract.h"

/**
 * @brief One block's ramp, held by value: the start, the per-sample step and the target.
 */
struct omx_ramp {
  float cur;  /**< The value at sample 0. */
  float step; /**< The increment per sample. */
  float tgt;  /**< The value the block ends on. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "ramp/begin"
/**
 * @brief Open a ramp from `*cur` toward `tgt` over `n` samples.
 * @param cur The value applied at the end of the previous block; a non-finite value reads as 0.
 * @param tgt The target; a non-finite value reads as 0.
 * @param n The block length in samples, at least 1.
 * @return The ramp.
 * @post `finite`: the start, the step and the target are finite whatever the caller held.
 * @note RT-safe: one divide, no call beyond `isfinite`. Thread-safe: no state.
 */
static inline struct omx_ramp omx_ramp_begin(const float *cur, float tgt, uint32_t n) {
  struct omx_ramp r;
  r.tgt = isfinite(tgt) ? tgt : 0.0f;
  r.cur = isfinite(*cur) ? *cur : 0.0f;
  r.step = (r.tgt - r.cur) / (float)n;
  OMX_POST(r.tgt - r.tgt == 0.0f && r.cur - r.cur == 0.0f && r.step - r.step == 0.0f, "finite");
  return r;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The ramp's value at sample `i`, independent per sample so the loop vectorises.
 * @param r The ramp.
 * @param i The sample index inside the block.
 * @return `r.cur + r.step·i`.
 * @note RT-safe: one multiply-add. Thread-safe: no state.
 */
static inline float omx_ramp_at(struct omx_ramp r, uint32_t i) {
  return r.cur + r.step * (float)i;
}

/**
 * @brief Close the ramp: the applied value lands exactly on the target.
 * @param r The ramp.
 * @param cur Receives `r.tgt`.
 * @note RT-safe: one store. Thread-safe on distinct `cur`.
 */
static inline void omx_ramp_end(struct omx_ramp r, float *cur) { *cur = r.tgt; }

#endif /* OMX_RAMP_H */
