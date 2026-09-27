// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_envelope.h
 * @brief The envelope detector: a cascade of one-poles with an attack and a release pole.
 *
 * `OMX_DYN_ENV_STAGES` one-poles in series, each stage taking the attack pole while its input
 * rises past it and the release pole while it falls. The input arrives rectified into the
 * detector's domain (`|x|` for peak, `x²` for RMS); the level is the last stage, its square root
 * in RMS. Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 row 7 and Appendix A.
 */
#ifndef OMX_ENVELOPE_H
#define OMX_ENVELOPE_H

#include <math.h>
#include <stdint.h>

#include "omxdsp.h"
#include "omx_contract.h"
#include "omx_onepole.h"

/** @brief Detector domain: the peak, `|x|`. */
#define OMX_DETECT_PEAK 0
/** @brief Detector domain: the RMS, the square root of the smoothed square. */
#define OMX_DETECT_RMS 1

/** @brief The number of one-poles in the cascade. */
#define OMX_DYN_ENV_STAGES 4

/** @brief The detector's state: the cascade, each stage a level in the detector's domain. A zeroed state is silence. */
struct omx_env {
  float stage[OMX_DYN_ENV_STAGES]; /**< Stage values, in the detector's domain; the last is the level. */
};

/** @brief The state layout, exported: omx_env_state_size() and omx_env_state_align(). */
OMXDSP_STATE_LAYOUT(env, struct omx_env)

/** @brief The detector's parameters. */
struct omx_env_params {
  float attack_pole;  /**< The single-pole attack pole at the base rate, in [0, 1); 0 is instant. */
  float release_pole; /**< The single-pole release pole at the base rate, in [0, 1). */
  int detect;         /**< OMX_DETECT_PEAK or OMX_DETECT_RMS. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "env/stage-poles"
/**
 * @brief The per-stage attack and release poles for a cascade run at `rate_mul` times the base rate.
 *
 * `pole^(N/rate_mul)`: the pole of time constant `τ/N` at that rate, `N` = OMX_DYN_ENV_STAGES.
 * @param e The parameters.
 * @param rate_mul The rate multiplier, at least 1 (1 is the base rate).
 * @param ac Out: the per-stage attack pole, in [0, 1).
 * @param rc Out: the per-stage release pole, in [0, 1).
 * @pre `pole-in-declared-range`, `rate-multiplier-positive`.
 * @post `pole-in-declared-range`.
 * @note RT-safe: products at the base rate, two `powf` otherwise, on the RT-safe allowlist.
 *       Thread-safe: pure.
 */
static inline void omx_env_stage_poles(const struct omx_env_params *e, uint32_t rate_mul, float *ac,
                                       float *rc) {
  OMX_PRE(e->attack_pole >= 0.0f && e->attack_pole < 1.0f && e->release_pole >= 0.0f &&
              e->release_pole < 1.0f,
          "pole-in-declared-range");
  OMX_PRE(rate_mul >= 1u, "rate-multiplier-positive");
  if (rate_mul == 1u) {
    float a = e->attack_pole, r_ = e->release_pole;
    for (int s = 1; s < OMX_DYN_ENV_STAGES; s++) { a *= e->attack_pole; r_ *= e->release_pole; }
    *ac = a;
    *rc = r_;
  } else {
    const float x = (float)OMX_DYN_ENV_STAGES / (float)rate_mul;
    *ac = powf(e->attack_pole, x);
    *rc = powf(e->release_pole, x);
  }
  OMX_POST(*ac >= 0.0f && *ac < 1.0f && *rc >= 0.0f && *rc < 1.0f, "pole-in-declared-range");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "env/level"
/**
 * @brief The detector level: the last stage, its square root in RMS.
 * @param env The state.
 * @param e The parameters (`detect` is read).
 * @return The level, linear, in the signal's amplitude.
 * @post `finite`.
 * @note RT-safe: at most one `sqrtf`. Thread-safe: pure.
 */
static inline float omx_env_level(const struct omx_env *env, const struct omx_env_params *e) {
  const float s = env->stage[OMX_DYN_ENV_STAGES - 1];
  const float y = (e->detect == OMX_DETECT_RMS) ? sqrtf(s < 0.0f ? 0.0f : s) : s;
  OMX_POST(y - y == 0.0f, "finite");
  return y;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "env/step"
/**
 * @brief One rectified sample through the cascade; returns the level.
 * @param env The state, updated in place.
 * @param e The parameters (`detect` is read).
 * @param d The input, rectified into the detector's domain; finite.
 * @param ac The per-stage attack pole, from omx_env_stage_poles().
 * @param rc The per-stage release pole, from omx_env_stage_poles().
 * @return The level, as omx_env_level() reads it.
 * @pre `finite-in`, `pole-in-declared-range`.
 * @post `finite`.
 * @invariant `envelope-finite`; `no-overshoot`: every stage lies between the input and the
 *            previous stages, each being a convex combination.
 * @note RT-safe: `OMX_DYN_ENV_STAGES` one-pole steps and at most one `sqrtf`. Thread-safe on
 *       distinct state.
 */
static inline float omx_env_step(struct omx_env *env, const struct omx_env_params *e, float d, float ac,
                                 float rc) {
  OMX_PRE(d - d == 0.0f, "finite-in");
  OMX_PRE(ac >= 0.0f && ac < 1.0f && rc >= 0.0f && rc < 1.0f, "pole-in-declared-range");
#ifdef OMX_CONTRACTS
  float lo = d, hi = d;
  for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) {
    lo = fminf(lo, env->stage[s]);
    hi = fmaxf(hi, env->stage[s]);
  }
#endif
  for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) d = omx_onepole(&env->stage[s], d, d > env->stage[s] ? ac : rc);
  const float y = (e->detect == OMX_DETECT_RMS) ? sqrtf(d < 0.0f ? 0.0f : d) : d;
  OMX_POST(y - y == 0.0f, "finite");
  OMX_INVARIANT(omx_block_finite(env->stage, (uint32_t)OMX_DYN_ENV_STAGES), "envelope-finite");
#ifdef OMX_CONTRACTS
  int inside = 1;
  for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) inside &= env->stage[s] >= lo - 1e-6f && env->stage[s] <= hi + 1e-6f;
#endif
  OMX_INVARIANT(inside, "no-overshoot");
  return y;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ENVELOPE_H */
