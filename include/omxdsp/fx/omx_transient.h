// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_transient.h
 * @brief The native transient designer: a gain driven by two envelope contrasts, with no threshold.
 *
 * `g_dB = attackDb·min(1, ΔA/REF) + sustainDb·min(1, ΔS/REF) + outputDb`, `y = 10^(g_dB/20)·x` on
 * both legs; ΔA and ΔS are omx_envdiff_step() over the legs' 0.5/0.5 peak fold, sharing one fast
 * cascade. Zero latency. Design: docs/design/specs/2026-09-26-transient-designer.md §2, §4, §6.
 */
#ifndef OMX_MIX_TRANSIENT_H
#define OMX_MIX_TRANSIENT_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_contract.h>
#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_envelope.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_units.h>

/** @brief The resolved atom: the three gains and the poles of both contrasts at one rate. */
struct omx_transient {
  int enabled;                  /**< 0 is bypass: the block is returned untouched. */
  float attack_db;              /**< The onset gain at full contrast, dB, inside its declared travel. */
  float sustain_db;             /**< The decay gain at full contrast, dB, inside its declared travel. */
  float output_db;              /**< The output gain, dB, inside its declared travel. */
  struct omx_env_params fast;   /**< The fast follower's single poles, peak detection. */
  struct omx_envdiff_poles on;  /**< The per-stage poles of ΔA: the slow cascade has the slow attack. */
  struct omx_envdiff_poles dec; /**< The per-stage poles of ΔS: the slow cascade has the slow release. */
};

/** @brief The stage's state: three cascades and the last applied gain. A zeroed state is silence. */
struct omx_transient_state {
  struct omx_env fast;         /**< The fast follower. */
  struct omx_env slow_attack;  /**< The slow-attack follower (ΔA). */
  struct omx_env slow_release; /**< The slow-release follower (ΔS). */
  float gain_db;               /**< The gain applied to the last sample, dB: the `gainDb` meter. */
};

/**
 * @brief Zeroes the state: silence, no gain applied.
 * @param st The state.
 * @note RT-safe: one memset. Thread-safe on distinct state.
 */
static inline void omx_transient_state_init(struct omx_transient_state *st) { memset(st, 0, sizeof(*st)); }

/**
 * @brief The stage's latency, samples: zero at every rate (L1).
 * @return 0.
 * @note RT-safe, pure.
 */
static inline int omx_transient_latency(void) { return 0; }

#define OMX_CONTRACT_STAGE "transient/resolve"
/**
 * @brief Resolves the five controls into the atom at `rate`, on the control thread.
 * @param t The atom, written whole.
 * @param bypass Non-zero leaves the stage bypassed.
 * @param attack_db The onset gain, dB, within OMX_TRANSIENT_ATTACK_DB_MIN..MAX.
 * @param sustain_db The decay gain, dB, within OMX_TRANSIENT_SUSTAIN_DB_MIN..MAX.
 * @param attack_time_ms The slow attack, ms, within OMX_TRANSIENT_ATTACK_TIME_MS_MIN..MAX.
 * @param sustain_time_ms The slow release, ms, within OMX_TRANSIENT_SUSTAIN_TIME_MS_MIN..MAX.
 * @param output_db The output gain, dB, within OMX_TRANSIENT_OUTPUT_DB_MIN..MAX.
 * @param rate The sample rate, Hz, a declared rate.
 * @pre `control-inside-travel` (a value outside is recorded, then clamped to the travel),
 *      `rate-is-declared`, `slow-follower-is-slower`.
 * @post `poles-in-declared-range`.
 * @note Not for the RT thread: six `expf` and two `powf`. Thread-safe on distinct atoms.
 */
static inline void omx_transient_resolve(struct omx_transient *t, int bypass, float attack_db, float sustain_db,
                                         float attack_time_ms, float sustain_time_ms, float output_db, float rate) {
  OMX_PRE(attack_db >= OMX_TRANSIENT_ATTACK_DB_MIN && attack_db <= OMX_TRANSIENT_ATTACK_DB_MAX &&
              sustain_db >= OMX_TRANSIENT_SUSTAIN_DB_MIN && sustain_db <= OMX_TRANSIENT_SUSTAIN_DB_MAX &&
              attack_time_ms >= OMX_TRANSIENT_ATTACK_TIME_MS_MIN && attack_time_ms <= OMX_TRANSIENT_ATTACK_TIME_MS_MAX &&
              sustain_time_ms >= OMX_TRANSIENT_SUSTAIN_TIME_MS_MIN &&
              sustain_time_ms <= OMX_TRANSIENT_SUSTAIN_TIME_MS_MAX && output_db >= OMX_TRANSIENT_OUTPUT_DB_MIN &&
              output_db <= OMX_TRANSIENT_OUTPUT_DB_MAX,
          "control-inside-travel");
  OMX_PRE(OMX_RATE_IS_DECLARED(rate), "rate-is-declared");
  OMX_PRE(OMX_TRANSIENT_ATTACK_TIME_MS_MIN > OMX_TRANSIENT_FAST_ATTACK_MS &&
              OMX_TRANSIENT_SUSTAIN_TIME_MS_MIN > OMX_TRANSIENT_FAST_RELEASE_MS,
          "slow-follower-is-slower");
  t->enabled = !bypass;
  t->attack_db = fminf(fmaxf(attack_db, OMX_TRANSIENT_ATTACK_DB_MIN), OMX_TRANSIENT_ATTACK_DB_MAX);
  t->sustain_db = fminf(fmaxf(sustain_db, OMX_TRANSIENT_SUSTAIN_DB_MIN), OMX_TRANSIENT_SUSTAIN_DB_MAX);
  t->output_db = fminf(fmaxf(output_db, OMX_TRANSIENT_OUTPUT_DB_MIN), OMX_TRANSIENT_OUTPUT_DB_MAX);
  const float at = fminf(fmaxf(attack_time_ms, OMX_TRANSIENT_ATTACK_TIME_MS_MIN), OMX_TRANSIENT_ATTACK_TIME_MS_MAX);
  const float st = fminf(fmaxf(sustain_time_ms, OMX_TRANSIENT_SUSTAIN_TIME_MS_MIN), OMX_TRANSIENT_SUSTAIN_TIME_MS_MAX);
  t->fast = (struct omx_env_params){omx_pole_from_time_ms(OMX_TRANSIENT_FAST_ATTACK_MS, rate),
                                    omx_pole_from_time_ms(OMX_TRANSIENT_FAST_RELEASE_MS, rate), OMX_DETECT_PEAK};
  const struct omx_env_params sa = {omx_pole_from_time_ms(at, rate), t->fast.release_pole, OMX_DETECT_PEAK};
  const struct omx_env_params sr = {t->fast.attack_pole, omx_pole_from_time_ms(st, rate), OMX_DETECT_PEAK};
  omx_env_stage_poles(&t->fast, 1u, &t->on.fast_attack, &t->on.fast_release);
  omx_env_stage_poles(&sa, 1u, &t->on.slow_attack, &t->on.slow_release);
  t->dec.fast_attack = t->on.fast_attack;
  t->dec.fast_release = t->on.fast_release;
  omx_env_stage_poles(&sr, 1u, &t->dec.slow_attack, &t->dec.slow_release);
  OMX_POST(t->on.slow_attack > t->on.fast_attack && t->dec.slow_release > t->dec.fast_release &&
               t->on.slow_attack < 1.0f && t->dec.slow_release < 1.0f,
           "poles-in-declared-range");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "transient"
/**
 * @brief Runs one block through the stage, in place, both legs by one gain.
 * @param l The left leg, `n` samples, overwritten.
 * @param r The right leg, `n` samples, overwritten.
 * @param n The block length, samples.
 * @param t The resolved atom.
 * @param st The state, updated in place.
 * @pre `finite-in`.
 * @post `gain-inside-the-declared-bound` (L4), `finite-out`.
 * @invariant `no-denormal-state` (L8).
 * @note RT-safe: per sample three cascades, one division and one omx_lin_to_db_poly() per
 *       contrast, one omx_db_to_lin_poly(); no libm call, allocation, lock or syscall. Thread-safe
 *       on distinct state.
 */
static inline void omx_transient_process(float *l, float *r, uint32_t n, const struct omx_transient *t,
                                         struct omx_transient_state *st) {
  if (!t->enabled) return;
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
#ifdef OMX_CONTRACTS
  const float hi = fmaxf(0.0f, t->attack_db) + fmaxf(0.0f, t->sustain_db) + t->output_db;
  const float lo = fminf(0.0f, t->attack_db) + fminf(0.0f, t->sustain_db) + t->output_db;
  int inside = 1;
#endif
  const float inv_ref = 1.0f / OMX_TRANSIENT_REF_DB;
  for (uint32_t i = 0; i < n; i++) {
    const float d = 0.5f * (fabsf(l[i]) + fabsf(r[i]));
    const float fl = omx_env_step(&st->fast, &t->fast, d, t->on.fast_attack, t->on.fast_release);
    const float da = omx_envdiff_step(&st->slow_attack, &t->fast, d, fl, &t->on, OMX_TRANSIENT_FLOOR_LIN);
    const float ds = omx_envdiff_step(&st->slow_release, &t->fast, d, fl, &t->dec, OMX_TRANSIENT_FLOOR_LIN);
    const float g_db = t->attack_db * fminf(1.0f, da * inv_ref) + t->sustain_db * fminf(1.0f, ds * inv_ref) + t->output_db;
#ifdef OMX_CONTRACTS
    inside &= g_db <= hi && g_db >= lo;
#endif
    for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) {
      st->fast.stage[s] = omx_flush(st->fast.stage[s]);
      st->slow_attack.stage[s] = omx_flush(st->slow_attack.stage[s]);
      st->slow_release.stage[s] = omx_flush(st->slow_release.stage[s]);
    }
    st->gain_db = g_db;
    const float g = omx_db_to_lin_poly(g_db);
    if (g != 1.0f) {
      l[i] *= g;
      r[i] *= g;
    }
  }
  OMX_POST(inside, "gain-inside-the-declared-bound");
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
#ifdef OMX_CONTRACTS
  int normal = 1;
  for (int s = 0; s < OMX_DYN_ENV_STAGES; s++)
    normal &= fpclassify(st->fast.stage[s]) != FP_SUBNORMAL && fpclassify(st->slow_attack.stage[s]) != FP_SUBNORMAL &&
              fpclassify(st->slow_release.stage[s]) != FP_SUBNORMAL;
#endif
  OMX_INVARIANT(normal, "no-denormal-state");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_TRANSIENT_H */
