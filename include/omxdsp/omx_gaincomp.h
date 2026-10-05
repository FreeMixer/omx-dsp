// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_gaincomp.h
 * @brief The dynamics gain computer: the soft-knee static characteristic, above or below a threshold.
 *
 * ABOVE (comp / limiter): unity below the threshold, slope `1/ratio − 1` above it. BELOW (gate /
 * expander): unity above, slope `ratio − 1` below, floored at `range_db`. A quadratic knee of
 * width `knee_db` centred on the threshold joins the two segments C¹. Stateless. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 9 and Appendix A.
 */
#ifndef OMX_GAINCOMP_H
#define OMX_GAINCOMP_H

#include "omx_contract.h"
#include "omx_contract_limits.h"
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
 * @note RT-safe: omx_log10f() and `powf` through omx_lin_to_db() and omx_db_to_lin(), both on the
 *       RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_gaincomp_gain(const struct omx_gaincomp_params *p, float level) {
  OMX_PRE(p->makeup_lin > 0.0f, "makeup-positive");
  const float g = omx_db_to_lin(omx_gaincomp_db(p, omx_lin_to_db(level))) * p->makeup_lin;
  OMX_POST(g >= 0.0f && (g <= p->makeup_lin || p->range_db > 0.0f), "no-gain-added");
  return g;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_GAINCOMP_H */
