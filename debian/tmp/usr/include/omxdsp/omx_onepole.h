// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_onepole.h
 * @brief The one-pole smoother, one convention: the parameter is the POLE `p` in [0, 1).
 *
 * `y = x·(1−p) + y·p`; DC gain 1 for every `p < 1`; corner `−ln(p)·sr/2π`; time constant
 * `τ = −1/(sr·ln p)`. Pole 0 is a wire, pole 1 a frozen filter. Neither constructor clamps its
 * argument: the caller's physics owns the range. Each keeps its pole below 1, though: a time or a
 * corner so far from the rate that `exp` rounds to 1 in float gives OMX_POLE_MAX, the pole nearest 1.
 * §1 row 15 and Appendix A.
 */
#ifndef OMX_ONEPOLE_H
#define OMX_ONEPOLE_H

#include <math.h>

#include "omx_contract.h"
#include "omx_denormal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** @brief The largest pole below 1 a float holds, 1 − 2⁻²⁴: what the constructors return when `exp` rounds to 1. */
#define OMX_POLE_MAX 0x1.fffffep-1f

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "onepole/convex"
/**
 * @brief One step of the one-pole in convex form, `y = x·(1−p) + y·p`.
 * @param state The filter's one word, updated in place.
 * @param x The input sample.
 * @param pole The pole, in [0, 1).
 * @return The new state.
 * @pre `finite-in`, `finite-state-in`, `pole-in-declared-range`.
 * @post `finite-out`; `no-overshoot-convex-combination`: the result lies between `x` and the
 *       previous state.
 * @note RT-safe: two multiplies, no call. Thread-safe on distinct state.
 */
static inline float omx_onepole(float *state, float x, float pole) {
  OMX_PRE(x - x == 0.0f, "finite-in");
  OMX_PRE(*state - *state == 0.0f, "finite-state-in");
  OMX_PRE(pole >= 0.0f && pole < 1.0f, "pole-in-declared-range");
#ifdef OMX_CONTRACTS
  const float lo = fminf(x, *state), hi = fmaxf(x, *state);
  /* the rounding of the two products and their sum is relative to the level: 1e-6 absolute was a fraction of
   * an ulp only below 16, and a sample of 1000 (a gain of +60 dB on a loud one) broke it by rounding alone */
  const float slack = 1e-6f * (1.0f + fmaxf(fabsf(lo), fabsf(hi)));
#endif
  *state = x * (1.0f - pole) + *state * pole;
  OMX_POST(*state - *state == 0.0f, "finite-out");
  OMX_POST(*state >= lo - slack && *state <= hi + slack, "no-overshoot-convex-combination");
  return *state;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "onepole/flush"
/**
 * @brief One step of omx_onepole() with the state flushed after it, for a filter inside a tail
 *        that decays for seconds.
 * @param state The filter's one word, updated in place and flushed.
 * @param x The input sample.
 * @param pole The pole, in [0, 1).
 * @return The new, flushed state.
 * @post `no-denormal-state`: the state is 0 or at least OMX_FLUSH_THRESHOLD in magnitude.
 * @note RT-safe: omx_onepole() and one compare. Thread-safe on distinct state.
 */
static inline float omx_onepole_flush(float *state, float x, float pole) {
  *state = omx_flush(omx_onepole(state, x, pole));
  OMX_POST(*state == 0.0f || fabsf(*state) >= OMX_FLUSH_THRESHOLD, "no-denormal-state");
  return *state;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "onepole/toward"
/**
 * @brief One step of the one-pole in increment form, `y += (1−p)·(target − y)`.
 *
 * The same filter as omx_onepole() and not bit-identical to it: at `target == *state` the
 * increment is exactly 0 and the state does not move, which a detector pair tracking equal
 * energies needs to hold a ratio of exactly 1.
 * @param state The filter's one word, updated in place.
 * @param target The value the state moves toward.
 * @param pole The pole, in [0, 1).
 * @return The new state.
 * @pre `finite-target`, `finite-state-in`, `pole-in-declared-range`.
 * @post `finite-out`; `never-overshoots-target`.
 * @note RT-safe: one multiply, no call. Thread-safe on distinct state.
 */
static inline float omx_onepole_toward(float *state, float target, float pole) {
  OMX_PRE(target - target == 0.0f, "finite-target");
  OMX_PRE(*state - *state == 0.0f, "finite-state-in");
  OMX_PRE(pole >= 0.0f && pole < 1.0f, "pole-in-declared-range");
#ifdef OMX_CONTRACTS
  const float lo = fminf(target, *state), hi = fmaxf(target, *state);
  const float slack = 1e-6f * (1.0f + fmaxf(fabsf(lo), fabsf(hi))); /* relative to the level, as in omx_onepole() */
#endif
  *state += (1.0f - pole) * (target - *state);
  OMX_POST(*state - *state == 0.0f, "finite-out");
  OMX_POST(*state >= lo - slack && *state <= hi + slack, "never-overshoots-target");
  return *state;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "onepole/from-time-ms"
/**
 * @brief The pole with time constant `ms` at `sr`: `exp(−1/(τ·sr))`; `ms <= 0` is a wire (pole 0).
 * @param ms Time constant, milliseconds; finite.
 * @param sr Sample rate, Hz; a declared rate.
 * @return The pole, in [0, 1): a time so long that `exp` rounds to 1 in float gives OMX_POLE_MAX.
 * @pre `finite-ms`, `rate-is-declared`.
 * @post `pole-in-declared-range`.
 * @note RT-safe: one `expf`, on the RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_pole_from_time_ms(float ms, float sr) {
  OMX_PRE(ms - ms == 0.0f, "finite-ms");
  OMX_PRE(sr <= 0.0f || OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  const float pole = ms <= 0.0f ? 0.0f : fminf(expf(-1.0f / (ms * 0.001f * sr)), OMX_POLE_MAX);
  OMX_POST(pole >= 0.0f && pole < 1.0f, "pole-in-declared-range");
  return pole;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "onepole/from-cutoff-hz"
/**
 * @brief The pole whose −3 dB corner is `hz` at `sr`: `exp(−2π·hz/sr)`; `hz <= 0` is a wire.
 * @param hz Corner frequency, Hz; finite.
 * @param sr Sample rate, Hz; a declared rate.
 * @return The pole, in [0, 1): a corner so low that `exp` rounds to 1 in float gives OMX_POLE_MAX.
 * @pre `finite-hz`, `rate-is-declared`.
 * @post `pole-in-declared-range`.
 * @note RT-safe: one `expf`, on the RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_pole_from_cutoff_hz(float hz, float sr) {
  OMX_PRE(hz - hz == 0.0f, "finite-hz");
  OMX_PRE(sr <= 0.0f || OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  const float pole = hz <= 0.0f ? 0.0f : fminf(expf(-2.0f * (float)M_PI * hz / sr), OMX_POLE_MAX);
  OMX_POST(pole >= 0.0f && pole < 1.0f, "pole-in-declared-range");
  return pole;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "onepole/rescale"
/**
 * @brief A pole quoted at a reference rate, re-expressed at the running rate: `p^(ref/sr)`, the
 *        same time constant in seconds (and the same corner in Hz) at every rate.
 * @param p The pole at `ref`, in [0, 1].
 * @param ref The rate the pole is quoted at, Hz; a rate that is not above 0 (0, negative, -inf,
 *        NaN: one the caller could not supply) leaves the pole as it is.
 * @param sr The running rate, Hz; a rate that is not above 0 leaves the pole as it is, as for `ref`.
 * @return The pole at `sr`. 0 and 1 are their own fixed points, so are `sr == ref` (no `powf` is
 *         taken, the reference rate is bit-for-bit untouched) and either rate not above 0. With
 *         both rates above 0 the exponent `ref/sr` is in [0, +inf], so the result is in [0, 1]
 *         (an infinite or overflowing exponent gives 0, a vanishing one gives 1).
 * @pre `pole-in-unit-range`: `0 <= p <= 1`.
 * @post `stable-pole`: the result stays in [0, 1], never NaN.
 * @note RT-safe: one `powf`, on the RT-safe allowlist; call it once per block, never per sample.
 *       Thread-safe: pure.
 */
static inline float omx_pole_rescale(float p, float ref, float sr) {
  OMX_PRE(p >= 0.0f && p <= 1.0f, "pole-in-unit-range");
  const float q = (p <= 0.0f || p >= 1.0f || !(ref > 0.0f) || !(sr > 0.0f) || sr == ref) ? p : powf(p, ref / sr);
  OMX_POST(q >= 0.0f && q <= 1.0f, "stable-pole");
  return q;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ONEPOLE_H */
