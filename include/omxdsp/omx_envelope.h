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
 * The rate-of-change detector omx_envdiff_step() is row 8: two cascades over one input differing
 * in one pole, their ratio in dB (docs/design/specs/2026-09-26-transient-designer.md §2).
 */
#ifndef OMX_ENVELOPE_H
#define OMX_ENVELOPE_H

#include <math.h>
#include <stdint.h>

#include "omxdsp.h"
#include "omx_contract.h"
#include "omx_onepole.h"
#include "omx_units.h"

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

/** @brief The per-stage poles of a differential detector's two cascades, from omx_env_stage_poles(). */
struct omx_envdiff_poles {
  float fast_attack;  /**< The fast cascade's per-stage attack pole, in [0, 1). */
  float fast_release; /**< The fast cascade's per-stage release pole, in [0, 1). */
  float slow_attack;  /**< The slow cascade's per-stage attack pole, in [fast_attack, 1). */
  float slow_release; /**< The slow cascade's per-stage release pole, in [fast_release, 1). */
};

#define OMX_CONTRACT_STAGE "envdiff/step"
/**
 * @brief One rectified sample through the slow cascade; returns its contrast against the fast level, dB.
 *
 * The slow cascade differs from the fast one in exactly one pole. With a slower attack the fast
 * level is the upper one, `Δ = 20·log10(max(fast, F) / max(slow, F))` (the onset contrast ΔA);
 * with a slower release the slow level is the upper one and the operands swap (the decay
 * contrast ΔS). Both envelopes scale with the input, so `Δ` does not depend on its level while
 * both stay above `F`. The ordering holds in float32 to rounding: a held input settles both
 * cascades onto `d`, each stage stalling within `2⁻²⁴/(1 − q)` of it, so the ratio is floored at 1.
 * @param slow The slow cascade's state, updated in place.
 * @param e The parameters (`detect` is read).
 * @param d The input, rectified into the detector's domain; finite.
 * @param fast_level The fast cascade's level for the same sample, from omx_env_step(); finite.
 * @param p The two cascades' per-stage poles.
 * @param floor The level both envelopes are floored at before the ratio, linear, positive.
 * @return The contrast, dB, at least 0.
 * @pre `finite-in`, `floor-positive`, `pole-in-declared-range`, `differ-in-exactly-one-pole`,
 *      `slow-is-slower`.
 * @post `contrast-non-negative`, `finite`.
 * @invariant `both-cascades-finite`; `ordering-within-rounding`: the unfloored ratio is at least
 *            `1 − N·2⁻²⁴/(1 − q)`, `q` the fast cascade's slower per-stage pole.
 * @note RT-safe: one omx_env_step(), one division, one omx_lin_to_db_poly(), no libm call.
 *       Thread-safe on distinct state.
 */
static inline float omx_envdiff_step(struct omx_env *slow, const struct omx_env_params *e, float d, float fast_level,
                                     const struct omx_envdiff_poles *p, float floor) {
  OMX_PRE(d - d == 0.0f && fast_level - fast_level == 0.0f, "finite-in");
  OMX_PRE(floor > 0.0f && floor - floor == 0.0f, "floor-positive");
  OMX_PRE(p->fast_attack >= 0.0f && p->fast_release >= 0.0f && p->slow_attack < 1.0f && p->slow_release < 1.0f,
          "pole-in-declared-range");
  OMX_PRE((p->slow_attack != p->fast_attack) != (p->slow_release != p->fast_release), "differ-in-exactly-one-pole");
  OMX_PRE(p->slow_attack >= p->fast_attack && p->slow_release >= p->fast_release, "slow-is-slower");
  const float s = omx_env_step(slow, e, d, p->slow_attack, p->slow_release);
  const int slow_above = p->slow_release != p->fast_release;
  const float hi = fmaxf(slow_above ? s : fast_level, floor);
  const float lo = fmaxf(slow_above ? fast_level : s, floor);
  const float ratio = hi / lo;
  const float delta = omx_lin_to_db_poly(fmaxf(ratio, 1.0f));
  OMX_POST(delta >= 0.0f, "contrast-non-negative");
  OMX_POST(delta - delta == 0.0f, "finite");
  OMX_INVARIANT(omx_block_finite(slow->stage, (uint32_t)OMX_DYN_ENV_STAGES) && fast_level - fast_level == 0.0f,
                "both-cascades-finite");
  OMX_INVARIANT(ratio >= 1.0f - (float)OMX_DYN_ENV_STAGES * 0x1p-24f / (1.0f - fmaxf(p->fast_attack, p->fast_release)),
                "ordering-within-rounding");
  return delta;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ENVELOPE_H */
