// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_onepole.h
 * @brief The one-pole smoother, one convention: the parameter is the POLE `p` in [0, 1).
 *
 * `y = x·(1−p) + y·p`; DC gain 1 for every `p < 1`; corner `−ln(p)·sr/2π`; time constant
 * `τ = −1/(sr·ln p)`. Pole 0 is a wire, pole 1 a frozen filter. Neither constructor clamps:
 * the caller's physics owns the range. Design: docs/design/specs/2026-09-26-dsp-primitives.md
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
#endif
  *state = x * (1.0f - pole) + *state * pole;
  OMX_POST(*state - *state == 0.0f, "finite-out");
  OMX_POST(*state >= lo - 1e-6f && *state <= hi + 1e-6f, "no-overshoot-convex-combination");
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
#endif
  *state += (1.0f - pole) * (target - *state);
  OMX_POST(*state - *state == 0.0f, "finite-out");
  OMX_POST(*state >= lo - 1e-6f && *state <= hi + 1e-6f, "never-overshoots-target");
  return *state;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "onepole/from-time-ms"
/**
 * @brief The pole with time constant `ms` at `sr`: `exp(−1/(τ·sr))`; `ms <= 0` is a wire (pole 0).
 * @param ms Time constant, milliseconds; finite.
 * @param sr Sample rate, Hz; a declared rate.
 * @return The pole, in [0, 1).
 * @pre `finite-ms`, `rate-is-declared`.
 * @post `pole-in-declared-range`.
 * @note RT-safe: one `expf`, on the RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_pole_from_time_ms(float ms, float sr) {
  OMX_PRE(ms - ms == 0.0f, "finite-ms");
  OMX_PRE(sr <= 0.0f || OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  const float pole = ms <= 0.0f ? 0.0f : expf(-1.0f / (ms * 0.001f * sr));
  OMX_POST(pole >= 0.0f && pole < 1.0f, "pole-in-declared-range");
  return pole;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "onepole/from-cutoff-hz"
/**
 * @brief The pole whose −3 dB corner is `hz` at `sr`: `exp(−2π·hz/sr)`; `hz <= 0` is a wire.
 * @param hz Corner frequency, Hz; finite.
 * @param sr Sample rate, Hz; a declared rate.
 * @return The pole, in [0, 1).
 * @pre `finite-hz`, `rate-is-declared`.
 * @post `pole-in-declared-range`.
 * @note RT-safe: one `expf`, on the RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_pole_from_cutoff_hz(float hz, float sr) {
  OMX_PRE(hz - hz == 0.0f, "finite-hz");
  OMX_PRE(sr <= 0.0f || OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  const float pole = hz <= 0.0f ? 0.0f : expf(-2.0f * (float)M_PI * hz / sr);
  OMX_POST(pole >= 0.0f && pole < 1.0f, "pole-in-declared-range");
  return pole;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ONEPOLE_H */
