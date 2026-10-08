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

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "omx_contract.h"
#include <omxcontract/omx_contract_limits.h>

/**
 * @brief The most sections one cascade carries: the joint budget of the strip EQ (9), the
 *        feedback corrector (6) and HRP at full polyphony (8), plus one spare. OMX_EQ_MAX_BANDS
 *        comes from the generated omx_contract_limits.h (declared once, in the declarations package).
 */

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

/**
 * @brief Whether a normalised section's poles lie inside the unit circle: `|a1| < 1 + a2` and
 *        `a2 < 1`, the stability triangle.
 * @param c The coefficients `{b0, b1, b2, a1, a2}`.
 * @return 1 when both poles are inside, else 0.
 * @note RT-safe and thread-safe: two compares, pure.
 */
static inline int omx_biquad_stable(const double c[5]) {
  return fabs(c[3]) < 1.0 + c[4] && c[4] < 1.0;
}

/**
 * @brief One biquad section in double, transposed direct form II: `y = b0·x + s₁`,
 *        `s₁ ← b1·x − a1·y + s₂`, `s₂ ← b2·x − a2·y`.
 *
 * The section a crossover runs: its identity `lo + hi = AP` holds to the double's rounding at a
 * corner whose poles sit within `2π·fc/sr` of the unit circle, where the float section loses it
 * (dsp-primitives spec Appendix A). Named distinctly from omx_biquad_d() (direct form I, double
 * coefficients, `s[4]`): both forms are needed — DF1 for the float cascade's bit-identical
 * per-section history, TDF2 double for the all-pass/crossover's coefficient sensitivity at a
 * corner near the unit circle (ruling 2026-09-28: the primitive collision between
 * lane/cat-allpass-band and graphic-eq/dsp-primitives row 2a; dsp-primitives spec §1 row 2b).
 * @param x The input sample.
 * @param c The normalised coefficients `{b0, b1, b2, a1, a2}`.
 * @param s The section's state `{s₁, s₂}`, updated in place.
 * @return The output sample.
 * @note RT-safe: five multiplies, no call, no branch; the state's denormals are covered by the
 *       thread's FTZ mode. Thread-safe on distinct state.
 */
static inline double omx_biquad_tdf2_d(double x, const double c[5], double s[2]) {
  const double y = c[0] * x + s[0];
  s[0] = c[1] * x - c[3] * y + s[1];
  s[1] = c[2] * x - c[4] * y;
  return y;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "eq/biquad-cascade"
/**
 * @brief Run up to `nbands` sections over one block of one or two legs in place — the cascade's
 *        ONE implementation.
 *
 * The result is, per leg, exactly the band-outer loop `for b: for i: x[i] = omx_biquad(x[i], c[b],
 * s[b])` over the live bands in slot order: every section is omx_biquad() over the same operands
 * in the same order, so the output and the state are bit-identical to it. The cost is not: the
 * parked bands are dropped ONCE per block, then the live ones run TWO sections per pass over the
 * samples and BOTH legs in that pass, each section's state copied into locals for the pass and
 * written back once. A section's recurrence is latency-bound; four independent recurrences per
 * pass let them overlap instead of waiting on each other (dsp-primitives spec §1 row 2).
 *
 * A parked section (`enabled[b] == 0`) is skipped and its state does not advance, so state slot
 * `b` always belongs to coefficient slot `b`.
 * @param l The first leg, filtered in place.
 * @param r The second leg, filtered in place, or NULL for one leg.
 * @param n Samples in the block.
 * @param nbands Sections to run, at most OMX_EQ_MAX_BANDS.
 * @param coeffs One `{b0, b1, b2, a1, a2}` set per section, shared by both legs.
 * @param enabled One byte per section, or NULL for all on.
 * @param state_l One `{x₁, x₂, y₁, y₂}` per section for `l`, updated in place.
 * @param state_r The same for `r`; unread when `r` is NULL.
 * @pre `finite-in`, `bands-within-cap`, `finite-coeffs` per section run.
 * @post `finite-state` per section run, `finite-out`.
 * @note RT-safe: O(nbands·n), no allocation, no call but omx_biquad(), no branch in the inner
 *       loops. Thread-safe on distinct state; reentrant — every intermediate is a local.
 */
static inline void omx_biquad_cascade_stereo(float *l, float *r, uint32_t n, uint32_t nbands,
                                             const float coeffs[][5], const uint8_t *enabled,
                                             float state_l[][4], float state_r[][4]) {
  OMX_PRE(omx_block_finite(l, n), "finite-in");
  OMX_PRE(!r || omx_block_finite(r, n), "finite-in");
  OMX_PRE(nbands <= OMX_EQ_MAX_BANDS, "bands-within-cap");
  if (nbands > OMX_EQ_MAX_BANDS) nbands = OMX_EQ_MAX_BANDS;
  uint8_t live[OMX_EQ_MAX_BANDS];
  uint32_t nl = 0;
  for (uint32_t b = 0; b < nbands; b++) {
    if (enabled && !enabled[b]) continue;
    OMX_PRE(omx_block_finite(coeffs[b], 5u), "finite-coeffs");
    live[nl++] = (uint8_t)b;
  }
  for (uint32_t k = 0; k < nl; k += 2) {
    const uint32_t b = live[k], two = k + 1 < nl;
    const uint32_t d = two ? live[k + 1] : b;
    float ca[5], cb[5], sa[4], sb[4], ta[4], tb[4];
    memcpy(ca, coeffs[b], sizeof ca);
    memcpy(cb, coeffs[d], sizeof cb);
    memcpy(sa, state_l[b], sizeof sa);
    memcpy(sb, state_l[d], sizeof sb);
    if (r) {
      memcpy(ta, state_r[b], sizeof ta);
      memcpy(tb, state_r[d], sizeof tb);
    }
    if (two && r) {
      for (uint32_t i = 0; i < n; i++) {
        l[i] = omx_biquad(omx_biquad(l[i], ca, sa), cb, sb);
        r[i] = omx_biquad(omx_biquad(r[i], ca, ta), cb, tb);
      }
    } else if (two) {
      for (uint32_t i = 0; i < n; i++) l[i] = omx_biquad(omx_biquad(l[i], ca, sa), cb, sb);
    } else if (r) {
      for (uint32_t i = 0; i < n; i++) {
        l[i] = omx_biquad(l[i], ca, sa);
        r[i] = omx_biquad(r[i], ca, ta);
      }
    } else {
      for (uint32_t i = 0; i < n; i++) l[i] = omx_biquad(l[i], ca, sa);
    }
    memcpy(state_l[b], sa, sizeof sa);
    OMX_POST(omx_block_finite(sa, 4u), "finite-state");
    if (two) {
      memcpy(state_l[d], sb, sizeof sb);
      OMX_POST(omx_block_finite(sb, 4u), "finite-state");
    }
    if (r) {
      memcpy(state_r[b], ta, sizeof ta);
      OMX_POST(omx_block_finite(ta, 4u), "finite-state");
      if (two) {
        memcpy(state_r[d], tb, sizeof tb);
        OMX_POST(omx_block_finite(tb, 4u), "finite-state");
      }
    }
  }
  OMX_POST(omx_block_finite(l, n), "finite-out");
  OMX_POST(!r || omx_block_finite(r, n), "finite-out");
}

/**
 * @brief Run up to `nbands` sections over a block in place, section-outer and sample-inner — the
 *        one-leg call of omx_biquad_cascade_stereo(), bit-identical to the band-outer loop.
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
  omx_biquad_cascade_stereo(buf, NULL, n, nbands, coeffs, enabled, state, NULL);
  /* The stateful kernel's own postcondition, at this entry too: every section run left its state
   * finite. A no-op loop, dropped by the compiler, unless OMX_CONTRACTS is defined. */
  for (uint32_t b = 0; b < nbands && b < OMX_EQ_MAX_BANDS; b++)
    if (!enabled || enabled[b]) OMX_POST(omx_block_finite(state[b], 4u), "finite-state");
  OMX_POST(omx_block_finite(buf, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief One biquad section, direct form I, in DOUBLE: omx_biquad()'s law with double coefficients
 *        and double history, a float sample in and out (dsp-primitives spec §1 row 2a).
 * A section whose poles sit within `ω/(2Q)` of the unit circle amplifies float32 round-off by
 * `1/|A(e^{jω})|`; in double the same section meets its closed form.
 * @param x The input sample.
 * @param c The normalised coefficients `{b0, b1, b2, a1, a2}`, in double.
 * @param s The section's state `{x₁, x₂, y₁, y₂}`, in double, updated in place.
 * @return The output sample, narrowed to float.
 * @note RT-safe: five multiplies, no call, no branch. Thread-safe on distinct state.
 */
static inline float omx_biquad_d(float x, const double c[5], double s[4]) {
  const double y = c[0] * (double)x + c[1] * s[0] + c[2] * s[1] - c[3] * s[2] - c[4] * s[3];
  s[1] = s[0];
  s[0] = (double)x;
  s[3] = s[2];
  s[2] = y;
  return (float)y;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "eq/biquad-cascade-double"
/**
 * @brief Run `nbands` double-precision sections over one block of one or two legs in place.
 * Band-outer, sample-inner: per leg exactly `for b: for i: x[i] = omx_biquad_d(x[i], c[b], s[b])`
 * over the enabled sections in slot order. No section cap: the caller sizes its own arrays. A
 * disabled section (`enabled[b] == 0`) is skipped and its state does not advance.
 * @param l The first leg, filtered in place.
 * @param r The second leg, filtered in place, or NULL for one leg.
 * @param n Samples in the block.
 * @param nbands Sections to run.
 * @param coeffs One double `{b0, b1, b2, a1, a2}` set per section, shared by both legs.
 * @param enabled One byte per section, or NULL for all on.
 * @param state_l One double `{x₁, x₂, y₁, y₂}` per section for `l`, updated in place.
 * @param state_r The same for `r`; unread when `r` is NULL.
 * @pre `finite-in`, `finite-coeffs` per section run.
 * @post `finite-state` per section run, `finite-out`.
 * @note RT-safe: O(nbands·n), no allocation, no call but omx_biquad_d(). Thread-safe on distinct
 *       state; reentrant — every intermediate is a local.
 */
static inline void omx_biquad_cascade_d_stereo(float *l, float *r, uint32_t n, uint32_t nbands,
                                               const double coeffs[][5], const uint8_t *enabled,
                                               double state_l[][4], double state_r[][4]) {
  OMX_PRE(omx_block_finite(l, n), "finite-in");
  OMX_PRE(!r || omx_block_finite(r, n), "finite-in");
  for (uint32_t b = 0; b < nbands; b++) {
    if (enabled && !enabled[b]) continue;
    OMX_PRE(isfinite(coeffs[b][0]) && isfinite(coeffs[b][1]) && isfinite(coeffs[b][2]) &&
                isfinite(coeffs[b][3]) && isfinite(coeffs[b][4]),
            "finite-coeffs");
    for (uint32_t i = 0; i < n; i++) l[i] = omx_biquad_d(l[i], coeffs[b], state_l[b]);
    if (r)
      for (uint32_t i = 0; i < n; i++) r[i] = omx_biquad_d(r[i], coeffs[b], state_r[b]);
    OMX_POST(isfinite(state_l[b][2]) && isfinite(state_l[b][3]), "finite-state");
  }
  OMX_POST(omx_block_finite(l, n) && (!r || omx_block_finite(r, n)), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_BIQUAD_H */
