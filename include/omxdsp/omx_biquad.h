// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_biquad.h
 * @brief The biquad section (direct form I) and the cascade over a block.
 *
 * Direct form I keeps the raw input and output history as its state, so a coefficient set
 * swapped live re-reads real samples with no rescaling. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 rows 1–2 and Appendix A.
 */
#ifndef OMX_BIQUAD_H
#define OMX_BIQUAD_H

#include <stdint.h>

#include "omx_contract.h"

/**
 * @brief The most sections one cascade carries: the joint budget of the strip EQ (9), the
 *        feedback corrector (6) and HRP at full polyphony (8), plus one spare. Its twin is
 *        `EQ_MAX_BANDS` in `@freemixer/declarations`.
 */
#define OMX_EQ_MAX_BANDS 24

/**
 * @brief One biquad section, direct form I: `y = b0·x + b1·x₁ + b2·x₂ − a1·y₁ − a2·y₂`.
 * @param x The input sample.
 * @param c The normalised coefficients `{b0, b1, b2, a1, a2}` (a0 folded to 1, the a-terms
 *          subtracted here).
 * @param s The section's state `{x₁, x₂, y₁, y₂}`, updated in place.
 * @return The output sample.
 * @note RT-safe: five multiplies, no call, no branch. Thread-safe on distinct state.
 */
static inline float omx_biquad(float x, const float c[5], float s[4]) {
  float y = c[0] * x + c[1] * s[0] + c[2] * s[1] - c[3] * s[2] - c[4] * s[3];
  s[1] = s[0];
  s[0] = x;
  s[3] = s[2];
  s[2] = y;
  return y;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "eq/biquad-cascade"
/**
 * @brief Run up to `nbands` sections over a block in place, section-outer and sample-inner.
 *
 * A parked section (`enabled[b] == 0`) is skipped and its state does not advance, so state slot
 * `b` always belongs to coefficient slot `b`.
 * @param buf The block, filtered in place.
 * @param n Samples in the block.
 * @param nbands Sections to run, at most OMX_EQ_MAX_BANDS.
 * @param coeffs One `{b0, b1, b2, a1, a2}` set per section.
 * @param enabled One byte per section, or NULL for all on.
 * @param state One `{x₁, x₂, y₁, y₂}` per section, updated in place.
 * @pre `finite-in`, `bands-within-cap`, `finite-coeffs` per section run.
 * @post `finite-state` per section run, `finite-out`.
 * @note RT-safe: O(nbands·n), no allocation, no branch in the inner loop. Thread-safe on
 *       distinct state.
 */
static inline void omx_biquad_cascade(float *buf, uint32_t n, uint32_t nbands,
                                      const float coeffs[][5], const uint8_t *enabled,
                                      float state[][4]) {
  OMX_PRE(omx_block_finite(buf, n), "finite-in");
  OMX_PRE(nbands <= OMX_EQ_MAX_BANDS, "bands-within-cap");
  if (nbands > OMX_EQ_MAX_BANDS) nbands = OMX_EQ_MAX_BANDS;
  for (uint32_t b = 0; b < nbands; b++) {
    if (enabled && !enabled[b]) continue;
    const float *c = coeffs[b];
    float *s = state[b];
    OMX_PRE(omx_block_finite(c, 5u), "finite-coeffs");
    for (uint32_t i = 0; i < n; i++) buf[i] = omx_biquad(buf[i], c, s);
    OMX_POST(omx_block_finite(s, 4u), "finite-state");
  }
  OMX_POST(omx_block_finite(buf, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_BIQUAD_H */
