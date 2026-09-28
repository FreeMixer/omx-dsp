// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_truepeak.h
 * @brief The true-peak detector: the largest `|sample|` of each frame's four ×4 images, through the
 *        shared half-band interpolator (ITU-R BS.1770-4's 4× true peak).
 *
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 row 11. The reading of frame `n`
 * covers the input interval `[n − U, n − U + 1)`, `U = omx_truepeak_delay()`; its phase-0 image is
 * the input sample `n − U` itself.
 */
#ifndef OMX_TRUEPEAK_H
#define OMX_TRUEPEAK_H

#include <math.h>
#include <stdint.h>

#include "omxdsp.h"
#include "omx_contract.h"
#include "omx_oversampler.h"

/** @brief The detector's oversampling factor: BS.1770-4's four. */
#define OMX_TRUEPEAK_FACTOR 4u
OMXDSP_STATIC_ASSERT(OMX_TRUEPEAK_FACTOR <= OMX_OVS_MAX_FACTOR, "the detector's factor is one the shared element runs");

/** @brief Base-rate frames interpolated per inner step (bounded stack scratch). */
#define OMX_TRUEPEAK_CHUNK 64u

/** @brief The detector's state: one ×4 interpolator. */
struct omx_truepeak {
  struct omx_oversampler ovs; /**< The shared half-band element, armed at ×4. */
};

OMXDSP_STATE_LAYOUT(truepeak, struct omx_truepeak)

/**
 * @brief Arm the detector: the interpolator at ×4, its history cleared.
 * @param t The detector.
 * @note CONTROL thread. Thread-safe on distinct state.
 */
static inline void omx_truepeak_init(struct omx_truepeak *t) { omx_oversampler_init(&t->ovs, OMX_TRUEPEAK_FACTOR); }

/**
 * @brief The reading's lag behind its input, in base-rate samples: the ×4 interpolator's group delay.
 * @return `OMX_OVS_UP_DELAY + OMX_OVS_UP_DELAY / 2` (36).
 * @note RT-safe and thread-safe: a constant.
 */
static inline uint32_t omx_truepeak_delay(void) { return OMX_OVS_UP_DELAY + OMX_OVS_UP_DELAY / 2u; }

#define OMX_CONTRACT_STAGE "truepeak/block"
/**
 * @brief Read `n` frames: `peak[i]` = the largest `|image|` of frame `i`'s four ×4 images.
 * @param t The detector.
 * @param in `n` base-rate frames; finite.
 * @param n Frames.
 * @param peak `n` readings out; may not alias `in`.
 * @return The block's largest reading (0 for `n = 0`).
 * @pre `finite-in`.
 * @post `true-peak-at-least-sample-peak`: each reading is at least its own phase-0 image;
 *       `finite`.
 * @note RT-safe: `4·OMX_TRUEPEAK_CHUNK` floats of stack, no allocation. Thread-safe on distinct state.
 */
static inline float omx_truepeak_block(struct omx_truepeak *t, const float *in, uint32_t n, float *peak) {
  OMX_PRE(omx_block_finite(in, n), "finite-in");
  float up[OMX_TRUEPEAK_FACTOR * OMX_TRUEPEAK_CHUNK];
  float most = 0.0f;
#ifdef OMX_CONTRACTS
  int above = 1;
#endif
  for (uint32_t done = 0; done < n;) {
    const uint32_t k = n - done < OMX_TRUEPEAK_CHUNK ? n - done : OMX_TRUEPEAK_CHUNK;
    omx_oversampler_up(&t->ovs, in + done, k, up);
    for (uint32_t i = 0; i < k; i++) {
      const float *f = up + OMX_TRUEPEAK_FACTOR * i;
      const float p = fmaxf(fmaxf(fabsf(f[0]), fabsf(f[1])), fmaxf(fabsf(f[2]), fabsf(f[3])));
#ifdef OMX_CONTRACTS
      above &= p >= fabsf(f[0]);
#endif
      peak[done + i] = p;
      most = fmaxf(most, p);
    }
    done += k;
  }
  OMX_POST(above, "true-peak-at-least-sample-peak");
  OMX_POST(most - most == 0.0f, "finite");
  return most;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_TRUEPEAK_H */
