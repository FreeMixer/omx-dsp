// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_slew.h
 * @brief A gain that moves at a declared rate, so a fade lasts the same TIME at every rate and quantum.
 *
 * omx_ramp.h, omx_route_fill_gain() and omx_route_fold_fill() cross exactly one block, so their
 * length is the quantum's. Here the caller gives a time: omx_slew_step() turns it into a step per
 * sample at the live rate, and the cursor walks to its target by that step across as many blocks
 * as it takes: 5 ms is four blocks at 48 kHz and n = 64, and lands inside one at 44.1 kHz and
 * n = 1024. The block size does not change a single sample: one run cut into any blocks gives the
 * same output, bit for bit.
 *
 * Each sample moves the cursor first and applies it after, so the first sample of a move is
 * already one step along, and the cursor lands on its target exactly, never past it.
 *
 * omx_slew_gain() is the plain slew. omx_slew_hold() is the fade a rewire needs: a position from 0
 * (open) to 1 (silent), gain `1 − position`, whose way back to open moves only on a sample the
 * input carries. A link made but not yet running hands the port silence, and a fade spent on
 * silence would let the first real sample through at full level. Silence times any gain is
 * silence, so waiting costs nothing audible; the way down moves every sample.
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_slew.h>.
 */
#ifndef OMX_SLEW_H
#define OMX_SLEW_H

#include <float.h>
#include <stdint.h>

#include <omxdsp/omx_contract.h>

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "slew/step"
/**
 * @brief The step per sample that crosses a distance of 1 in `ms` at rate `sr`: `1000 / (ms·sr)`.
 * @param ms The time, ms. Zero, a negative or a non-finite value is a jump, a step of 1.
 * @param sr The live rate, Hz, positive.
 * @return The step, in (0, 1]; a time shorter than one sample gives 1, a time too long for a float
 *         the smallest normal step.
 * @pre `rate-positive`.
 * @post `step-within-one-sample`.
 * @note One divide. Thread-safe: pure.
 * @thread audio
 */
static inline float omx_slew_step(float ms, float sr) {
  OMX_PRE(sr > 0.0f, "rate-positive");
  const float s = ms > 0.0f && ms - ms == 0.0f ? 1000.0f / (ms * sr) : 1.0f;
  const float step = s < 1.0f ? (s > FLT_MIN ? s : FLT_MIN) : 1.0f;
  OMX_POST(step > 0.0f && step <= 1.0f, "step-within-one-sample");
  return step;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The cursor one step toward its target, landing on it exactly.
 * @param cur The cursor.
 * @param tgt The target.
 * @param step The step, positive.
 * @return `tgt` when the step would reach or pass it, else `cur ± step`.
 * @note Two compares. Thread-safe: pure.
 * @thread audio
 */
static inline float omx_slew_move(float cur, float tgt, float step) {
  if (cur < tgt) {
    const float up = cur + step;
    return up < tgt ? up : tgt;
  }
  const float down = cur - step;
  return down > tgt ? down : tgt;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "slew/gain"
/**
 * @brief Copy `in` to `out` at a gain that walks from `*cur` to `tgt` by `step` per sample.
 * @param in The input, `n` samples.
 * @param out The output, `n` samples, may be `in`.
 * @param n The block length; 0 touches nothing but the heal of the ends.
 * @param tgt The target; a non-finite value reads as 0.
 * @param step The step per sample, from omx_slew_step().
 * @param cur The cursor, kept across blocks; a non-finite value reads as 0.
 * @pre `step-positive`.
 * @post `cursor-finite`.
 * @note One compare and one multiply a sample. Thread-safe on distinct `cur` and `out`.
 * @thread audio
 */
static inline void omx_slew_gain(const float *in, float *out, uint32_t n, float tgt, float step, float *cur) {
  OMX_PRE(step > 0.0f, "step-positive");
  const float t = tgt - tgt == 0.0f ? tgt : 0.0f;
  float g = *cur - *cur == 0.0f ? *cur : 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    g = omx_slew_move(g, t, step);
    out[i] = in[i] * g;
  }
  *cur = g;
  OMX_POST(g - g == 0.0f, "cursor-finite");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "slew/hold"
/**
 * @brief Fade one or two legs by a position that walks to `tgt`, gain `1 − position`; toward open it
 *        moves only on a sample the input carries.
 * @param in_l The left input, `n` samples.
 * @param in_r The right input, `n` samples; `in_l` for one leg, and then `out_r` is not written.
 * @param out_l The left output, `n` samples, may be `in_l`.
 * @param out_r The right output, `n` samples, may be `in_r`.
 * @param n The block length; 0 touches nothing but the heal of the ends.
 * @param tgt The target position, 0 (open) to 1 (silent); a non-finite value reads as 1.
 * @param step The step per sample, from omx_slew_step().
 * @param pos The position, kept across blocks; a non-finite value reads as 1.
 * @pre `step-positive`, `target-a-position`.
 * @post `position-a-position`.
 * @note Three compares and two multiplies a sample. Thread-safe on distinct `pos` and outputs.
 * @thread audio
 */
static inline void omx_slew_hold(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n,
                                 float tgt, float step, float *pos) {
  OMX_PRE(step > 0.0f, "step-positive");
  OMX_PRE(!(tgt < 0.0f) && !(tgt > 1.0f), "target-a-position");
  const int two = in_r != in_l;
  const float t = tgt - tgt == 0.0f ? tgt : 1.0f;
  float h = *pos - *pos == 0.0f ? *pos : 1.0f;
  for (uint32_t i = 0; i < n; i++) {
    const float xl = in_l[i], xr = in_r[i];
    if (h < t || xl != 0.0f || xr != 0.0f) h = omx_slew_move(h, t, step);
    const float g = 1.0f - h;
    out_l[i] = xl * g;
    if (two) out_r[i] = xr * g;
  }
  *pos = h;
  OMX_POST(!(h < 0.0f) && !(h > 1.0f) && h - h == 0.0f, "position-a-position");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_SLEW_H */
