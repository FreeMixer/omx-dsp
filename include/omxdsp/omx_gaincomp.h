// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_gaincomp.h
 * @brief The dynamics gain computer: the soft-knee static characteristic, above or below a threshold.
 *
 * ABOVE (comp / limiter): unity below the threshold, slope `1/ratio − 1` above it. BELOW (gate /
 * expander): unity above, slope `ratio − 1` below, floored at `range_db`. UPWARD (upward
 * compressor): unity above, and below the gain RISES by `(T − L)(1 − 1/ratio)`, capped at
 * `range_db`, the largest boost; it is ABOVE's curve mirrored about the threshold, the same knee
 * read at `−x`, never a second knee. A quadratic knee of
 * width `knee_db` centred on the threshold joins the two segments C¹; in BELOW mode the knee may
 * also sit off centre, spanning a gate's start and end points (omx_gaincomp_db_shifted). Stateless. Design:
 */
#ifndef OMX_GAINCOMP_H
#define OMX_GAINCOMP_H

#include "omx_contract.h"
#include <omxdsp/omxdsp_definitions.h>
#include "omx_fastmath.h"
#include "omx_units.h"

/** @brief Gain-computer mode: act on the signal ABOVE the threshold (comp / limiter). */
#define OMX_DYN_ABOVE 0
/** @brief Gain-computer mode: act on the signal BELOW the threshold (gate / expander). */
#define OMX_DYN_BELOW 1
/** @brief Gain-computer mode: raise the signal BELOW the threshold toward it (upward compressor). */
#define OMX_DYN_UPWARD 2

/** @brief The gain computer's parameters, resolved; a dynamics atom embeds one. */
struct omx_gaincomp_params {
  int mode;         /**< OMX_DYN_ABOVE, OMX_DYN_BELOW or OMX_DYN_UPWARD. */
  float thresh_db;  /**< The threshold, dB. */
  float ratio;      /**< The ratio, at least the mode's declared minimum. */
  float knee_db;    /**< The knee width, dB, not negative; 0 is a hard knee. */
  float range_db;   /**< BELOW: the attenuation floor, dB, at most 0. UPWARD: the largest boost, dB,
                     *   at least 0. Ignored in ABOVE: an ABOVE floor is the caller's, through
                     *   omx_gaincomp_floor(), so no compressor is clamped by a word it never set. */
  float makeup_lin; /**< The make-up, linear amplitude, positive; 1 is none. */
};

/**
 * @brief THE FLOOR of an ABOVE gain: `max(floor, g)`, the one word every floored reduction goes through.
 *
 * The comp's duck (omx_dynamics_keyed, `omx_dyn.floor_db`), the de-esser and the band-dyn band
 * (omx_band_dyn_offset_db, `range_db`) and the wet ducker (omx_duck_gain_db, `depth_db`) all hold their
 * reduction here, so a reduction cannot be floored two ways. It is not inside omx_gaincomp_db(): `range_db`
 * is ignored in ABOVE mode, and flooring every compressor at a word it never set would clamp it.
 *
 * Either domain, both arguments in the same one: dB, or linear amplitude (the floor's own dB through
 * omx_db_to_lin(), make-up included as the gain carries it). The map is monotone, so the floor is the
 * same either way. A floor of -INFINITY in dB, or 0 in linear, is no floor: `g` itself, bit for bit.
 * @param g The gain.
 * @param floor The floor, in `g`'s domain.
 * @return `floor` where `g` is below it, else `g` (a NaN `g` stays NaN).
 * @note One compare and one select, no call, so a loop of it vectorises. Thread-safe: pure.
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_gaincomp_floor(float g, float floor) { return g < floor ? floor : g; }

#undef OMX_CONTRACT_STAGE
/**
 * @brief `c ? a : b` inside omx_gaincomp_db_sel(): the `?:` itself when `lanes` is 0, omx_fm_select() when it is 1.
 *
 * Under the trapping-math default the compiler may not speculate float arithmetic, so a `?:` whose arm is used
 * nowhere else gets that arm moved into a branch, and a loop over the function then does not vectorise. The
 * select of integer bits uses both arms in the open. It is the same float either way; a per-sample caller keeps
 * the `?:`, which skips the knee's work outside the knee.
 */
#define OMX_GC_SEL(lanes, c, a, b) ((lanes) ? omx_fm_select((c), (a), (b)) : ((c) ? (a) : (b)))

#define OMX_CONTRACT_STAGE "gaincomp/db"
/**
 * @brief omx_gaincomp_db() and omx_gaincomp_db_lane(): one body, its selects spelt by `lanes`.
 * @param p The parameters.
 * @param level_db The detector level, dB; finite.
 * @param lanes 0 for the per-sample function, 1 for the branch-free lane; a constant at every call.
 * @return The gain, dB: the same float for either `lanes`.
 * @pre omx_gaincomp_db()'s.
 * @post omx_gaincomp_db()'s.
 * @note Arithmetic and selects, no call. Thread-safe: pure.
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_gaincomp_db_sel(const struct omx_gaincomp_params *p, float level_db,
                                                      const int lanes) {
  OMX_PRE(p->ratio >= (p->mode == OMX_DYN_BELOW ? OMX_GATE_RATIO_MIN : OMX_COMP_RATIO_MIN), "ratio-at-least-one");
  OMX_PRE(p->knee_db >= OMX_COMP_KNEE_DB_MIN, "knee-not-negative");
  OMX_PRE(p->mode != OMX_DYN_BELOW || p->range_db <= OMX_GATE_RANGE_DB_MAX, "range-is-an-attenuation");
  OMX_PRE(p->mode != OMX_DYN_UPWARD || p->range_db >= 0.0f, "range-is-a-boost");
  OMX_PRE(level_db - level_db == 0.0f, "finite-in");
  const float x = level_db - p->thresh_db;
  const float half = 0.5f * p->knee_db;
  const int soft = p->knee_db > 0.0f;
  /* The knee's denominator, 1 at a hard knee: every knee arm is selected away there, and a division by 0 would
   * still raise FE_DIVBYZERO (and FE_INVALID at x == +-half) where both arms are evaluated. */
  const float k2 = omx_fm_select(soft, 2.0f * p->knee_db, 1.0f);
  const float eb = x - half;
  const float kb = (1.0f - p->ratio) * eb * eb / k2;
  float gb = OMX_GC_SEL(lanes, (soft & (x > -half)) != 0, kb, (p->ratio - 1.0f) * x);
  gb = OMX_GC_SEL(lanes, x >= half, 0.0f, gb);
  gb = OMX_GC_SEL(lanes, gb < p->range_db, p->range_db, gb);
  const float slope = (1.0f / p->ratio) - 1.0f;
  const float ea = x + half;
  const float ka = slope * ea * ea / k2;
  float ga = OMX_GC_SEL(lanes, (soft & (x < half)) != 0, ka, slope * x);
  ga = OMX_GC_SEL(lanes, x <= -half, 0.0f, ga);
  /* UPWARD: ABOVE's curve at the mirrored level `-x`, negated, then capped at the boost. */
  const float eu = half - x;
  const float ku = slope * eu * eu / k2;
  float gu = OMX_GC_SEL(lanes, (soft & (-x < half)) != 0, ku, slope * -x);
  gu = OMX_GC_SEL(lanes, -x <= -half, 0.0f, -gu);
  gu = OMX_GC_SEL(lanes, gu > p->range_db, p->range_db, gu);
  const float g = OMX_GC_SEL(lanes, p->mode == OMX_DYN_BELOW, gb, OMX_GC_SEL(lanes, p->mode == OMX_DYN_UPWARD, gu, ga));
  OMX_POST(g - g == 0.0f, "finite");
  return g;
}

/**
 * @brief The static gain, dB (at most 0 before make-up, but in UPWARD mode, where it is at most
 *        `range_db`), at detector level `level_db`.
 * @param p The parameters.
 * @param level_db The detector level, dB; finite.
 * @return The gain, dB.
 * @pre `ratio-at-least-one` (`OMX_COMP_RATIO_MIN` in ABOVE, `OMX_GATE_RATIO_MIN` in BELOW),
 *      `knee-not-negative` (`OMX_COMP_KNEE_DB_MIN`), `range-is-an-attenuation`
 *      (`OMX_GATE_RANGE_DB_MAX`, BELOW only), `range-is-a-boost` (UPWARD only), `finite-in`.
 * @post `finite`.
 * @note Arithmetic and selects, no call. Thread-safe: pure.
 * @twin gaincompDb
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_gaincomp_db(const struct omx_gaincomp_params *p, float level_db) {
  return omx_gaincomp_db_sel(p, level_db, 0);
}

/**
 * @brief omx_gaincomp_db() for a vectorised loop: the same float, every select one of bits (omx_fastmath.h).
 * @param p The parameters.
 * @param level_db The detector level, dB; finite.
 * @return The gain, dB.
 * @pre omx_gaincomp_db()'s.
 * @post omx_gaincomp_db()'s.
 * @note Arithmetic and selects, no call, no branch. Thread-safe: pure.
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_gaincomp_db_lane(const struct omx_gaincomp_params *p, float level_db) {
  return omx_gaincomp_db_sel(p, level_db, 1);
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
 * @note `log10f` and `powf` through omx_lin_to_db() and omx_db_to_lin(), both on the
 *       RT-safe allowlist. Thread-safe: pure.
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_gaincomp_gain(const struct omx_gaincomp_params *p, float level) {
  OMX_PRE(p->makeup_lin > 0.0f, "makeup-positive");
  const float g = omx_db_to_lin(omx_gaincomp_db(p, omx_lin_to_db(level))) * p->makeup_lin;
  OMX_POST(g >= 0.0f && (g <= p->makeup_lin || p->range_db > 0.0f), "no-gain-added");
  return g;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief omx_gaincomp_gain() for a vectorised loop: the same float, through omx_gaincomp_db_lane().
 *
 * With libmvec the log10f and powf are the loop's vector calls, so the value differs from
 * omx_gaincomp_gain()'s by their ULP bounds only. It checks no contract (a contract note in a lane would be a
 * call per sample): the loops take it only where contracts are compiled out (OMX_DYN_LANES).
 * @param p The parameters.
 * @param level The detector level, linear, not negative; finite.
 * @return The gain, in `[0, makeup_lin]`.
 * @note No allocation, no state. Thread-safe: pure.
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_gaincomp_gain_lane(const struct omx_gaincomp_params *p, float level) {
  return omx_db_to_lin(omx_gaincomp_db_lane(p, omx_lin_to_db_lane(level))) * p->makeup_lin;
}

/**
 * @brief The gate's off-centre knee: the quadratic Bezier between the expander's line and 0 dB, at `x`.
 *
 * The root in [0, 1] of `(xl + xh) u^2 - 2 xl u + (xl - x) = 0`, in the form with no cancellation and
 * no division by a vanishing leading coefficient (the centred knee's is 0). The discriminant is
 * `(xh - xl)^2` at `x = xh` and moves linearly to `(2 xl)^2` at `x = xl`, so it is not negative on the
 * knee; the clamp only absorbs rounding. Meaningful for `xl < x < xh`; outside it the value is
 * arbitrary (computed with no division by zero) and the caller selects the line or 0 dB instead.
 * @param p The parameters; BELOW mode.
 * @param xl The knee's lower edge, dB from the threshold.
 * @param xh The knee's upper edge, dB from the threshold.
 * @param x The level, dB from the threshold.
 * @return The gain, dB, before the range floor.
 * @note Arithmetic and one sqrtf. Thread-safe: pure.
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_gaincomp_knee_shifted(const struct omx_gaincomp_params *p, float xl, float xh, float x) {
  const float a = xl + xh, b = -2.0f * xl, d = x - xl;
  /* Both clamps are selects of bits (omx_fm_select), not `?:`: a compiler may vectorise `c ? v : 0` before a sqrtf
   * as the square root of the unclamped value and a select after it (GCC 13 does), and outside the knee, where
   * the branch-free lane still evaluates this, the discriminant is negative: FE_INVALID. */
  const float disc0 = b * b + 4.0f * a * d;
  const float disc = omx_fm_select(disc0 > 0.0f, disc0, 0.0f);
  /* Inside the knee the denominator is positive (b > 0, or b == 0 with d > 0). Outside it, it is 0 at a shift of
   * +knee/2 (xl == 0) for every x <= 0: 1 there raises no FE_DIVBYZERO and no FE_INVALID, and the value is
   * selected away as before. */
  const float den0 = b + sqrtf(disc);
  const float den = omx_fm_select(den0 > 0.0f, den0, 1.0f);
  const float u = 2.0f * d / den;
  const float w = 1.0f - u;
  return w * w * (p->ratio - 1.0f) * xl;
}

#define OMX_CONTRACT_STAGE "gaincomp/db-shifted"
/**
 * @brief The BELOW static gain, dB, with the knee's centre moved `shift_db` off the threshold.
 *
 * The gate's knee is a RANGE, not only a width: from `thresh + shift - knee/2` to
 * `thresh + shift + knee/2`, with `|shift| <= knee/2` so the threshold stays inside it. Below the
 * range the gain is the expander's line `(ratio - 1) * (level - thresh)`, above it 0 dB, and
 * across it the quadratic Bezier whose control point is the corner of those two lines,
 * `(thresh, 0 dB)`. It meets each line at the range's edge with the line's own slope, so the curve
 * is C1 at both edges. Centred (`shift_db == 0`) that Bezier IS omx_gaincomp_db's quadratic knee,
 * and this function returns omx_gaincomp_db itself, bit for bit.
 *
 * Closed form, with `x` the level from the threshold and `xl`, `xh` the edges: the Bezier at
 * parameter `u` is `x = (1-u)^2 xl + u^2 xh` and `g = (1-u)^2 (ratio-1) xl`, so its midpoint
 * (`u = 1/2`) is `x = (xl + xh) / 4` and `g = (ratio - 1) xl / 4`. The result is floored at
 * `range_db` like every BELOW gain.
 * @param p The parameters; BELOW mode.
 * @param shift_db The knee centre's offset from the threshold, dB; 0 is the centred knee.
 * @param level_db The detector level, dB; finite.
 * @return The gain, dB, in `[range_db, 0]`.
 * @pre `shift-is-below-mode` and `shift-inside-the-knee` when `shift_db` is not 0, and then
 *      `ratio-at-least-one`, `range-is-an-attenuation`, `finite-in`.
 * @post `finite`, `no-gain-added`.
 * @note Arithmetic and one sqrtf. Thread-safe: pure.
 * @twin gaincompDbShifted
 * @thread audio
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
  else g = omx_gaincomp_knee_shifted(p, xl, xh, x);
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
 * @note Thread-safe: pure.
 * @thread audio
 */
static inline float omx_gaincomp_gain_shifted(const struct omx_gaincomp_params *p, float shift_db, float level) {
  if (shift_db == 0.0f) return omx_gaincomp_gain(p, level);
  OMX_PRE(p->makeup_lin > 0.0f, "makeup-positive");
  return omx_db_to_lin(omx_gaincomp_db_shifted(p, shift_db, omx_lin_to_db(level))) * p->makeup_lin;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief omx_gaincomp_gain_shifted() for a vectorised loop: the same float, with no branch.
 *
 * The line, 0 dB and the knee are all evaluated and one is selected, so a loop of calls has no control
 * flow and the compiler can run it over several samples at once (omx_fastmath.h). Only for
 * `shift_db != 0` (the caller takes omx_gaincomp_gain() for the centred knee) and in BELOW mode with the
 * preconditions of omx_gaincomp_db_shifted(), which it does not check. The libm calls are the loop's,
 * so with libmvec the value differs from omx_gaincomp_gain_shifted()'s by their ULP bounds only.
 * @param p The parameters; BELOW mode.
 * @param shift_db The knee centre's offset from the threshold, dB; not 0.
 * @param level The detector level, linear, not negative; finite.
 * @return The gain, in `[0, makeup_lin]`.
 * @note No allocation, no state. Thread-safe: pure.
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_gaincomp_gain_shifted_lane(const struct omx_gaincomp_params *p, float shift_db, float level) {
  const float level_db = omx_lin_to_db_lane(level);
  const float half = 0.5f * p->knee_db;
  const float x = level_db - p->thresh_db;
  const float xl = shift_db - half, xh = shift_db + half;
  const float line = (p->ratio - 1.0f) * x, knee = omx_gaincomp_knee_shifted(p, xl, xh, x);
  float g = omx_fm_select(x <= xl, line, omx_fm_select(x >= xh, 0.0f, knee));
  g = omx_fm_select(g < p->range_db, p->range_db, g);
  return omx_db_to_lin(g) * p->makeup_lin;
}

#define OMX_CONTRACT_STAGE "gaincomp/db-fb"
/**
 * @brief The feedback detector's OUTPUT-domain characteristic, dB (at most 0, before make-up).
 *
 * Above the threshold the slope is `−(R − 1)` against the level the detector HEARD, which is the
 * compressor's own output; closing the loop `y = x + g(y)` gives `y − T = (x − T)/R`, the
 * feed-forward curve at the same ratio (bus compressor design, §4 L2). The knee is the library's C¹ parabola, `−(R − 1)(x + W/2)²/(2W)` across `±W/2`: it is
 * omx_gaincomp_db()'s BELOW branch read at the mirrored level `−x` against the mirrored threshold
 * `−T`, with no floor — the same word, never a second knee.
 * @param p The slot's gain-computer parameters (`thresh_db`, `ratio`, `knee_db` are read).
 * @param level_db The detector level, dB; finite.
 * @return The gain, dB.
 * @pre `ratio-at-least-one` (`OMX_COMP_RATIO_MIN`), `finite-in`.
 * @post `no-gain-added`: at most 0 dB.
 * @note One omx_gaincomp_db(). Thread-safe: pure.
 * @thread audio
 */
static inline float omx_gaincomp_db_fb(const struct omx_gaincomp_params *p, float level_db) {
  OMX_PRE(p->ratio >= OMX_COMP_RATIO_MIN, "ratio-at-least-one");
  OMX_PRE(level_db - level_db == 0.0f, "finite-in");
  const struct omx_gaincomp_params mirror = {OMX_DYN_BELOW, -p->thresh_db, p->ratio, p->knee_db, -INFINITY, 1.0f};
  const float g = omx_gaincomp_db(&mirror, -level_db);
  OMX_POST(g <= 0.0f, "no-gain-added");
  return g;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "gaincomp/fb-settled"
/**
 * @brief Where the feedback loop settles for a HELD input level: the output level, dB, solving
 *        `y = x + omx_gaincomp_db_fb(y)` in closed form (bus compressor design §4 L2).
 *
 * Below the knee's reach `y = x`; past it (`x − T ≥ R·W/2`) `y − T = (x − T)/R`, the feed-forward
 * curve at the same ratio; inside it `v = y − T + W/2` solves `a·v² + v − (x − T + W/2) = 0` with
 * `a = (R − 1)/(2W)`, taken as `v = 2q/(1 + √(1 + 4aq))` so the knee's foot (q → 0) loses nothing to
 * cancellation. Continuous at both edges. The loop's attack is bounded by it (omx_dynamics_keyed):
 * a step can bring the envelope TO the level the held input settles at, never past it.
 * @param p The slot's gain-computer parameters (`thresh_db`, `ratio`, `knee_db` are read).
 * @param x_db The input level, dB, in the detector's measure; finite.
 * @return The settled output level, dB: at most `x_db`.
 * @pre `ratio-at-least-one`, `finite-in`.
 * @post `no-gain-added`.
 * @note At most one sqrtf. Thread-safe: pure.
 * @thread audio
 */
static inline float omx_gaincomp_fb_settled_db(const struct omx_gaincomp_params *p, float x_db) {
  OMX_PRE(p->ratio >= OMX_COMP_RATIO_MIN, "ratio-at-least-one");
  OMX_PRE(x_db - x_db == 0.0f, "finite-in");
  const float r = p->ratio, w = p->knee_db, s = x_db - p->thresh_db, hw = 0.5f * w;
  float y;
  if (s <= -hw) y = x_db;
  else if (s >= hw * r) y = p->thresh_db + s / r;
  else {
    const float a = (r - 1.0f) / (2.0f * w), q = s + hw;
    y = p->thresh_db + 2.0f * q / (1.0f + sqrtf(1.0f + 4.0f * a * q)) - hw;
  }
  y = y < x_db ? y : x_db;
  OMX_POST(y <= x_db, "no-gain-added");
  return y;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "gaincomp/fb-level-of"
/**
 * @brief The inverse of omx_gaincomp_db_fb(): the detector level, dB, at which the feedback characteristic
 *        answers `gr_db`.
 *
 * Past the knee's reach (`−gr ≥ (R − 1)·W/2`) the level is `T + (−gr)/(R − 1)`; inside it, from the C¹ parabola,
 * `T − W/2 + √(2W·(−gr)/(R − 1))`. A reduction of 0 (or a ratio of 1) answers the knee's foot, `T − W/2`, the
 * highest level the characteristic leaves untouched. The loop keeps its envelope at least at the level the
 * APPLIED gain stands for while a program release holds that gain deeper (omx_dynamics_keyed).
 * @param p The slot's gain-computer parameters (`thresh_db`, `ratio`, `knee_db` are read).
 * @param gr_db The reduction, dB, at most 0; finite.
 * @return The detector level, dB.
 * @pre `ratio-at-least-one`, `finite-in`.
 * @note At most one sqrtf. Thread-safe: pure.
 * @thread audio
 */
static inline float omx_gaincomp_fb_level_of_db(const struct omx_gaincomp_params *p, float gr_db) {
  OMX_PRE(p->ratio >= OMX_COMP_RATIO_MIN, "ratio-at-least-one");
  OMX_PRE(gr_db - gr_db == 0.0f, "finite-in");
  const float r1 = p->ratio - 1.0f, w = p->knee_db, red = gr_db < 0.0f ? -gr_db : 0.0f, hw = 0.5f * w;
  if (r1 <= 0.0f || red <= 0.0f) return p->thresh_db - hw;
  if (red >= r1 * hw) return p->thresh_db + red / r1;
  return p->thresh_db - hw + sqrtf(2.0f * w * red / r1);
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_GAINCOMP_H */
