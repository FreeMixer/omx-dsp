// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_oversampler.h
 * @brief The one streaming rate changer: ×2 and ×4 polyphase half-band, up and down, with the
 *        history a block boundary needs; and the one half-band table and dot every consumer reads.
 *
 * Latency is declared, not small: up 24 and down 24 base samples at ×2 (48 round trip), 36 and
 * 36 at ×4 (72). A caller lines its dry path up by exactly the round trip. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 18 and Appendix A;
 * docs/design/notes/2026-09-14-oversampling-vs-192k.md (the measurement).
 */
#ifndef OMX_OVERSAMPLER_H
#define OMX_OVERSAMPLER_H

#include <stddef.h>
#include <stdint.h>

#include "omx_contract.h"

/**
 * @brief The half-band low-pass's centre tap: a Blackman-windowed ideal half-band sinc, M = 47,
 *        unity DC gain, 0.02 dB passband ripple to 0.9·Fs/4, at least 52.8 dB stopband from
 *        1.1·Fs/4. Every even tap but the centre is exactly zero.
 */
#define OMX_HALFBAND_CENTER 0.500000057f
/** @brief Nonzero odd-offset taps on ONE side, k = 1, 3, 5, …, 47; the other side mirrors them. */
#define OMX_HALFBAND_ODD_TAPS 24
/** @brief The odd-offset taps, k = 1, 3, …, 2·OMX_HALFBAND_ODD_TAPS − 1. */
static const float OMX_HALFBAND_ODD_COEF[OMX_HALFBAND_ODD_TAPS] = {
  0.3177273f,      -0.104366698f,   0.0608047389f,   -0.0415491669f,
  0.0304507085f,   -0.0231160632f,  0.0178622693f,   -0.0139073084f,
  0.0108378378f,   -0.00841314009f, 0.00648126218f,  -0.00493914778f,
  0.00371219675f,  -0.00274322342f, 0.00198629722f,  -0.00140325938f,
  0.000961759745f, -0.000634169659f, 0.000396992001f, -0.000230535968f,
  0.000118709811f, -4.88386527e-05f, 1.14505977e-05f,  9.39880332e-20f,
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "halfband/dot"
/**
 * @brief The half-band FIR at one interior centre: the centre tap, then the odd taps in
 *        ascending order — the accumulation order is part of the contract, every oracle's bits
 *        stand on it.
 * @param c Points at the centre sample; the caller owes `c[-(2T−1) … c[2T−1]]`, 95 samples.
 * @return The filtered sample.
 * @pre `finite-window`.
 * @post `finite-out`.
 * @note RT-safe: 25 multiply-adds, no call. Thread-safe: pure over the caller's window.
 */
static inline float omx_halfband_dot(const float *c) {
  OMX_PRE(omx_block_finite(c - (2u * OMX_HALFBAND_ODD_TAPS - 1u), 4u * OMX_HALFBAND_ODD_TAPS - 1u),
          "finite-window");
  float acc = OMX_HALFBAND_CENTER * c[0];
  for (uint32_t i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) {
    const int32_t k = (int32_t)(2u * i + 1u);
    acc += OMX_HALFBAND_ODD_COEF[i] * (c[-k] + c[k]);
  }
  OMX_POST(acc - acc == 0.0f, "finite-out");
  return acc;
}
#undef OMX_CONTRACT_STAGE

/** @brief The largest factor this element implements; 1 is a copy with no filtering and no delay. */
#define OMX_OVS_MAX_FACTOR 4u
/** @brief Stages of ×2 at the maximum factor: ×4 is two half-bands, never one ÷4 FIR. */
#define OMX_OVS_MAX_STAGES 2u

/** @brief The interpolator's delay, in samples of its own input. */
#define OMX_OVS_UP_DELAY (OMX_HALFBAND_ODD_TAPS)
/** @brief The decimator's delay, in samples of its own input. */
#define OMX_OVS_DOWN_DELAY (2u * OMX_HALFBAND_ODD_TAPS)
/** @brief History one interpolating stage keeps, samples of its input. */
#define OMX_OVS_UP_HIST (2u * OMX_HALFBAND_ODD_TAPS)
/** @brief History one decimating stage keeps, samples of its input. */
#define OMX_OVS_DOWN_HIST (4u * OMX_HALFBAND_ODD_TAPS)

/**
 * @brief The round-trip latency of ×4 in base samples, as a plain number a compensation delay
 *        line can be sized by at compile time; held equal to the stages' sum below.
 */
#define OMX_OVS_LATENCY_4X 72

OMXDSP_STATIC_ASSERT(OMX_OVS_LATENCY_4X == OMX_OVS_UP_DELAY + OMX_OVS_UP_DELAY / 2u
                                         + OMX_OVS_DOWN_DELAY / 4u + OMX_OVS_DOWN_DELAY / 2u,
               "the declared 4x latency must be the one the half-band stages actually cost");

/** @brief The element's state: plain inline floats an owner embeds; nothing to allocate. */
struct omx_oversampler {
  uint32_t factor;                                  /**< 1, 2 or 4. */
  uint32_t stages;                                  /**< 0, 1 or 2. */
  float up_hist[OMX_OVS_MAX_STAGES][OMX_OVS_UP_HIST];     /**< Each interpolating stage's history. */
  float down_hist[OMX_OVS_MAX_STAGES][OMX_OVS_DOWN_HIST]; /**< Each decimating stage's history. */
};

/**
 * @brief The state's size, for a host that lays the state out itself.
 * @return `sizeof(struct omx_oversampler)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_oversampler_state_size(void) { return sizeof(struct omx_oversampler); }

/**
 * @brief The state's alignment, for a host that lays the state out itself.
 * @return `OMXDSP_ALIGNOF(struct omx_oversampler)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_oversampler_state_align(void) { return OMXDSP_ALIGNOF(struct omx_oversampler); }

/**
 * @brief Set the factor and clear the history; the factor is rounded down to 1, 2 or 4.
 * @param o The element.
 * @param factor The requested factor.
 * @note RT-safe: one memset. Thread-safe on distinct state. The caller owns any crossfade that
 *       hides the discontinuity of re-arming a running element.
 */
void omx_oversampler_init(struct omx_oversampler *o, uint32_t factor);

/**
 * @brief `n` base-rate frames in, `n · factor` frames out; a copy at factor 1.
 * @param o The element.
 * @param in `n` frames.
 * @param n Base-rate frames.
 * @param out `n · factor` frames; may not alias `in`.
 * @pre `finite-in`.
 * @post `finite-out`.
 * @note RT-safe: bounded stack scratch per chunk, no allocation. Thread-safe on distinct state.
 */
void omx_oversampler_up(struct omx_oversampler *o, const float *in, uint32_t n, float *out);

/**
 * @brief `n · factor` frames in, `n` base-rate frames out; a copy at factor 1.
 * @param o The element.
 * @param in `n · factor` frames.
 * @param n Base-rate frames out.
 * @param out `n` frames; may not alias `in`.
 * @pre `finite-in`.
 * @post `finite-out`.
 * @note RT-safe: bounded stack scratch per chunk, no allocation. Thread-safe on distinct state.
 */
void omx_oversampler_down(struct omx_oversampler *o, const float *in, uint32_t n, float *out);

/**
 * @brief The round-trip group delay of up then down, in base-rate samples: 0, 48 or 72.
 * @param o The element.
 * @return Base-rate samples.
 * @note RT-safe and thread-safe: a table lookup on the factor.
 */
uint32_t omx_oversampler_latency(const struct omx_oversampler *o);

/**
 * @brief The same figure for a factor not yet instantiated.
 * @param factor 1, 2 or 4 (anything else rounds as omx_oversampler_init() does).
 * @return Base-rate samples.
 * @note RT-safe and thread-safe: arithmetic.
 */
uint32_t omx_oversampler_latency_for(uint32_t factor);

#endif /* OMX_OVERSAMPLER_H */
