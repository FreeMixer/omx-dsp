// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_gaincomp.h
 * @brief The dynamics gain computer: the soft-knee static characteristic, above or below a threshold.
 *
 * ABOVE (comp / limiter): unity below the threshold, slope `1/ratio − 1` above it. BELOW (gate /
 * expander): unity above, slope `ratio − 1` below, floored at `range_db`. A quadratic knee of
 * width `knee_db` centred on the threshold joins the two segments C¹; in BELOW mode the knee may
 * also sit off centre, spanning a gate's start and end points (omx_gaincomp_db_shifted). Stateless. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 9 and Appendix A.
 */
#ifndef OMX_GAINCOMP_H
#define OMX_GAINCOMP_H

#include "omx_contract.h"
#include <omxcontract/omx_contract_limits.h>
#include "omx_units.h"

/** @brief Gain-computer mode: act on the signal ABOVE the threshold (comp / limiter). */
#define OMX_DYN_ABOVE 0
/** @brief Gain-computer mode: act on the signal BELOW the threshold (gate / expander). */
#define OMX_DYN_BELOW 1

/** @brief The gain computer's parameters, resolved; a dynamics atom embeds one. */
struct omx_gaincomp_params {
  int mode;         /**< OMX_DYN_ABOVE or OMX_DYN_BELOW. */
  float thresh_db;  /**< The threshold, dB. */
  float ratio;      /**< The ratio, at least the mode's declared minimum. */
  float knee_db;    /**< The knee width, dB, not negative; 0 is a hard knee. */
  float range_db;   /**< BELOW: the attenuation floor, dB, at most 0. Ignored in ABOVE. */
  float makeup_lin; /**< The make-up, linear amplitude, positive; 1 is none. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "gaincomp/db"
/**
 * @brief The static gain, dB (at most 0 before make-up), at detector level `level_db`.
 * @param p The parameters.
 * @param level_db The detector level, dB; finite.
 * @return The gain, dB.
 * @pre `ratio-at-least-one` (`OMX_COMP_RATIO_MIN` in ABOVE, `OMX_GATE_RATIO_MIN` in BELOW),
 *      `knee-not-negative` (`OMX_COMP_KNEE_DB_MIN`), `range-is-an-attenuation`
 *      (`OMX_GATE_RANGE_DB_MAX`, BELOW only), `finite-in`.
 * @post `finite`.
 * @note RT-safe: arithmetic and selects, no call, no branch. Thread-safe: pure.
 */
static inline float omx_gaincomp_db(const struct omx_gaincomp_params *p, float level_db) {
  OMX_PRE(p->ratio >= (p->mode == OMX_DYN_BELOW ? OMX_GATE_RATIO_MIN : OMX_COMP_RATIO_MIN), "ratio-at-least-one");
  OMX_PRE(p->knee_db >= OMX_COMP_KNEE_DB_MIN, "knee-not-negative");
  OMX_PRE(p->mode != OMX_DYN_BELOW || p->range_db <= OMX_GATE_RANGE_DB_MAX, "range-is-an-attenuation");
  OMX_PRE(level_db - level_db == 0.0f, "finite-in");
  const float x = level_db - p->thresh_db;
  const float half = 0.5f * p->knee_db;
  const int soft = p->knee_db > 0.0f;
  const float eb = x - half;
  const float kb = (1.0f - p->ratio) * eb * eb / (2.0f * p->knee_db);
  float gb = ((soft & (x > -half)) != 0) ? kb : (p->ratio - 1.0f) * x;
  gb = x >= half ? 0.0f : gb;
  gb = gb < p->range_db ? p->range_db : gb;
  const float slope = (1.0f / p->ratio) - 1.0f;
  const float ea = x + half;
  const float ka = slope * ea * ea / (2.0f * p->knee_db);
  float ga = ((soft & (x < half)) != 0) ? ka : slope * x;
  ga = x <= -half ? 0.0f : ga;
  const float g = p->mode == OMX_DYN_BELOW ? gb : ga;
  OMX_POST(g - g == 0.0f, "finite");
  return g;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "gaincomp/gain"
/**
 * @brief The linear gain, make-up included, at detector level `level` (linear, the detector's domain).
 * @param p The parameters.
 * @param level The detector level, linear, not negative; finite.
 * @return The gain, in `[0, makeup_lin]`.
 * @pre `makeup-positive`.
 * @post `no-gain-added`: the gain lies in `[0, makeup_lin]` unless `range_db` is positive.
 * @note RT-safe: `log10f` and `powf` through omx_lin_to_db() and omx_db_to_lin(), both on the
 *       RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_gaincomp_gain(const struct omx_gaincomp_params *p, float level) {
  OMX_PRE(p->makeup_lin > 0.0f, "makeup-positive");
  const float g = omx_db_to_lin(omx_gaincomp_db(p, omx_lin_to_db(level))) * p->makeup_lin;
  OMX_POST(g >= 0.0f && (g <= p->makeup_lin || p->range_db > 0.0f), "no-gain-added");
  return g;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "gaincomp/db-shifted"
/**
 * @brief The BELOW static gain, dB, with the knee's centre moved `shift_db` off the threshold.
 *
 * The gate's knee is a RANGE, not only a width: it spans `[thresh + shift - knee/2, thresh + shift
 * + knee/2]`, with `|shift| <= knee/2` so the threshold stays inside it. Below the range the gain is
 * the expander's line `(ratio - 1) * (level - thresh)`, above it 0 dB, and across it the quadratic
 * Bezier whose control point is the corner of those two lines, `(thresh, 0 dB)`. It meets each line
 * at the range's edge with the line's own slope, so the curve is C1 at both edges. Centred
 * (`shift_db == 0`) that Bezier IS omx_gaincomp_db's quadratic knee, and this function returns
 * omx_gaincomp_db itself, bit for bit. Closed form, with `x` the level from the threshold and `xl`,
 * `xh` the edges: the Bezier at parameter `u` is `x = (1-u)^2 xl + u^2 xh`, `g = (1-u)^2 (ratio-1)
 * xl`, so its midpoint (`u = 1/2`) is `x = (xl + xh) / 4`, `g = (ratio - 1) xl / 4`. The result is
 * floored at `range_db` like every BELOW gain.
 * @param p The parameters; BELOW mode.
 * @param shift_db The knee centre's offset from the threshold, dB; 0 is the centred knee.
 * @param level_db The detector level, dB; finite.
 * @return The gain, dB, in `[range_db, 0]`.
 * @pre `shift-is-below-mode` and `shift-inside-the-knee` when `shift_db` is not 0, and then
 *      `ratio-at-least-one`, `range-is-an-attenuation`, `finite-in`.
 * @post `finite`, `no-gain-added`.
 * @note RT-safe: arithmetic and one sqrtf. Thread-safe: pure.
 */
static inline float omx_gaincomp_db_shifted(const struct omx_gaincomp_params *p, float shift_db, float level_db) {
  if (shift_db == 0.0f) return omx_gaincomp_db(p, level_db);
  const float half = 0.5f * p->knee_db;
  OMX_PRE(p->mode == OMX_DYN_BELOW, "shift-is-below-mode");
  OMX_PRE(shift_db >= -half && shift_db <= half, "shift-inside-the-knee");
  OMX_PRE(p->ratio >= OMX_GATE_RATIO_MIN, "ratio-at-least-one");
  OMX_PRE(p->range_db <= OMX_GATE_RANGE_DB_MAX, "range-is-an-attenuation");
  OMX_PRE(level_db - level_db == 0.0f, "finite-in");
  const float x = level_db - p->thresh_db;
  const float xl = shift_db - half, xh = shift_db + half;
  float g;
  if (x <= xl) g = (p->ratio - 1.0f) * x;
  else if (x >= xh) g = 0.0f;
  else {
    /* The root in [0, 1] of (xl + xh) u^2 - 2 xl u + (xl - x) = 0, in the form with no
     * cancellation and no division by a vanishing leading coefficient (the centred knee's is 0).
     * The discriminant is (xh - xl)^2 at x = xh and moves linearly to (2 xl)^2 at x = xl, so it
     * is not negative on the knee; the clamp only absorbs rounding. */
    const float a = xl + xh, b = -2.0f * xl, d = x - xl;
    float disc = b * b + 4.0f * a * d;
    disc = disc > 0.0f ? disc : 0.0f;
    const float u = 2.0f * d / (b + sqrtf(disc));
    const float w = 1.0f - u;
    g = w * w * (p->ratio - 1.0f) * xl;
  }
  g = g < p->range_db ? p->range_db : g;
  OMX_POST(g - g == 0.0f, "finite");
  OMX_POST(g <= 0.0f, "no-gain-added");
  return g;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "gaincomp/gain-shifted"
/**
 * @brief omx_gaincomp_gain over omx_gaincomp_db_shifted's knee range; `shift_db == 0` is
 *        omx_gaincomp_gain itself, bit for bit.
 * @param p The parameters.
 * @param shift_db The knee centre's offset from the threshold, dB.
 * @param level The detector level, linear, not negative; finite.
 * @return The gain, in `[0, makeup_lin]`.
 * @pre `makeup-positive`, and omx_gaincomp_db_shifted's.
 * @note RT-safe. Thread-safe: pure.
 */
static inline float omx_gaincomp_gain_shifted(const struct omx_gaincomp_params *p, float shift_db, float level) {
  if (shift_db == 0.0f) return omx_gaincomp_gain(p, level);
  OMX_PRE(p->makeup_lin > 0.0f, "makeup-positive");
  return omx_db_to_lin(omx_gaincomp_db_shifted(p, shift_db, omx_lin_to_db(level))) * p->makeup_lin;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_GAINCOMP_H */
