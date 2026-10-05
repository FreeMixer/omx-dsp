// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_rotor.h
 * @brief The ROTOR: one oscillator whose phase drives a Doppler read of a fractional delay line
 *        and an amplitude swing a quarter-turn apart, its rate a one-pole state moving toward a
 *        target — the word the rotary-speaker stage runs twice.
 *
 * Spec: docs/design/specs/2026-09-26-rotary-speaker.md §1-§2. Composed of `omx_fdelay.h` at order
 * 3, `omx_lfo.h` and `omx_onepole.h`; the constants are `OMX_ROTARY_*` from the generated
 * `omx_contract_limits.h`. No allocation: the lines are the caller's.
 */
#ifndef OMX_MIX_ROTOR_H
#define OMX_MIX_ROTOR_H

#include <stdint.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_fdelay.h>
#include <omxdsp/omx_lfo.h>
#include <omxdsp/omx_onepole.h>

/** @brief The Lagrange order a rotor reads at: the modulated-read order of the fdelay ruling. */
#define OMX_ROTOR_ORDER 3
/** @brief Where the amplitude read sits in the turn relative to the Doppler read, turns. */
#define OMX_ROTOR_AM_OFFSET 0.25f

/** @brief One rotor's resolved atom, built once per block (omx_rotor_resolve()). */
struct omx_rotor {
  float target_hz; /**< The rate the rotor moves toward, Hz, non-negative. */
  float pole;      /**< The per-sample pole of that move, in [0, 1). */
  float base;      /**< The shortest Doppler read, samples. */
  float depth;     /**< The read's travel above `base`, samples: 2·D. */
  float am;        /**< The amplitude swing m, in [0, 1]: the gain spans [1 − m, 1]. */
  float sr;        /**< The live rate, Hz. */
};

/** @brief One rotor's state: the phase, and the present rate as a target plus a distance from
 *         it — the one-pole steps the distance, which keeps its float precision to the end. */
struct omx_rotor_state {
  struct omx_lfo lfo; /**< The rotation, turns. */
  float anchor_hz;    /**< The target the distance is measured from, Hz, non-negative. */
  float dev_hz;       /**< The present rate minus `anchor_hz`, Hz. */
};

/**
 * @brief The rotor's present rate, Hz — the readback a surface draws the spin-up from.
 * @param s The state.
 * @return `anchor_hz + dev_hz`.
 * @note RT-safe and thread-safe: one add.
 */
static inline float omx_rotor_rate(const struct omx_rotor_state *s) { return s->anchor_hz + s->dev_hz; }

/**
 * @brief Settle a rotor at a rate and a phase, the distance zero.
 * @param s The state, written.
 * @param rate_hz The rate, Hz, non-negative.
 * @param phase The phase, turns, in [0, 1).
 * @note RT-safe: three stores. Thread-safe on distinct state.
 */
static inline void omx_rotor_settle(struct omx_rotor_state *s, float rate_hz, float phase) {
  s->lfo.phase = phase;
  s->lfo.inc = 0.0f;
  s->anchor_hz = rate_hz;
  s->dev_hz = 0.0f;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "rotor/resolve"
/**
 * @brief Resolve one rotor for the next block: the pole is the accel time when the target is
 *        above the present rate, the decel time otherwise, each scaled by `accel`.
 * @param p The atom, written.
 * @param s The rotor's state, read for its present rate.
 * @param target_hz The target rate, Hz, non-negative (0 is `stop`).
 * @param accel_ms The speed-up time constant, ms.
 * @param decel_ms The slow-down time constant, ms.
 * @param accel The scale on both times, positive.
 * @param doppler_ms The Doppler excursion D, ms.
 * @param am The amplitude swing, in [0, 1].
 * @param sr The live rate, a declared rate.
 * @pre `a-target-is-not-negative`, `an-accel-scale-is-positive`, `a-swing-is-inside-unity`.
 * @post `the-sweep-clears-the-kernels-reach`.
 * @note Not per sample: one `expf` (omx_pole_from_time_ms). Thread-safe on distinct atoms.
 */
static inline void omx_rotor_resolve(struct omx_rotor *p, const struct omx_rotor_state *s,
                                     float target_hz, float accel_ms, float decel_ms, float accel,
                                     float doppler_ms, float am, float sr) {
  OMX_PRE(target_hz >= 0.0f, "a-target-is-not-negative");
  OMX_PRE(accel > 0.0f, "an-accel-scale-is-positive");
  OMX_PRE(am >= 0.0f && am <= 1.0f, "a-swing-is-inside-unity");
  p->target_hz = target_hz;
  p->pole = omx_pole_from_time_ms(accel * (target_hz > omx_rotor_rate(s) ? accel_ms : decel_ms), sr);
  p->base = OMX_ROTARY_BASE_MS * 0.001f * sr;
  p->depth = 2.0f * doppler_ms * 0.001f * sr;
  p->am = am;
  p->sr = sr;
  OMX_POST(p->base >= omx_fdelay_min_delay(OMX_ROTOR_ORDER), "the-sweep-clears-the-kernels-reach");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "rotor/step"
/**
 * @brief Advance the rotor one sample — the rate's distance from its target first (re-based when
 *        the target moved), then the phase — and read the delay and the gain at the new phase.
 * @param p The atom.
 * @param s The state, updated in place.
 * @param d The Doppler read, samples: `sweep(base, depth, lfo_at(u, 0))`.
 * @param a The gain: `1 − m·(1 − lfo_at(u, 1/4))/2`.
 * @pre `a-target-is-not-negative`, `a-rate-is-not-negative`.
 * @post `a-stopped-rotor-holds-an-exact-zero`, `a-gain-is-inside-the-swing`.
 * @note RT-safe: one one-pole step, one division, two parabolas, no libm. Thread-safe on distinct
 *       state.
 */
static inline void omx_rotor_step(const struct omx_rotor *p, struct omx_rotor_state *s, float *d,
                                  float *a) {
  OMX_PRE(p->target_hz >= 0.0f, "a-target-is-not-negative");
  OMX_PRE(omx_rotor_rate(s) >= 0.0f, "a-rate-is-not-negative");
  if (p->target_hz != s->anchor_hz) {
    s->dev_hz = omx_rotor_rate(s) - p->target_hz;
    s->anchor_hz = p->target_hz;
  }
  omx_onepole_toward(&s->dev_hz, 0.0f, p->pole);
  if (p->target_hz == 0.0f && s->dev_hz < OMX_ROTARY_STOP_EPS) s->dev_hz = 0.0f;
  const float rate = omx_rotor_rate(s);
  s->lfo.inc = omx_lfo_inc(rate, p->sr);
  omx_lfo_advance(&s->lfo);
  *d = omx_lfo_sweep(p->base, p->depth, omx_lfo_at(&s->lfo, 0.0f));
  *a = 1.0f - p->am * 0.5f * (1.0f - omx_lfo_at(&s->lfo, OMX_ROTOR_AM_OFFSET));
  OMX_POST(p->target_hz != 0.0f || rate == 0.0f || rate >= OMX_ROTARY_STOP_EPS,
           "a-stopped-rotor-holds-an-exact-zero");
  OMX_POST(*a >= 1.0f - p->am - 1e-6f && *a <= 1.0f + 1e-6f, "a-gain-is-inside-the-swing");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "rotor/tick"
/**
 * @brief One sample of one rotor over two legs: step, write each leg, read both at the one delay
 *        through one kernel, scale by the one gain.
 * @param p The atom.
 * @param s The rotor's state, shared by both legs.
 * @param ll The left leg's line, armed at OMX_ROTOR_ORDER.
 * @param lr The right leg's line, same geometry.
 * @param xl The left input.
 * @param xr The right input.
 * @param yl The left output.
 * @param yr The right output.
 * @pre `both-legs-share-one-geometry`, `the-sweep-is-inside-the-ring`.
 * @note RT-safe: one step, two writes, one kernel, two four-tap reads. Thread-safe on distinct
 *       state and lines.
 */
static inline void omx_rotor_tick(const struct omx_rotor *p, struct omx_rotor_state *s,
                                  struct omx_fdelay *ll, struct omx_fdelay *lr, float xl, float xr,
                                  float *yl, float *yr) {
  OMX_PRE(ll->cap == lr->cap && ll->order == OMX_ROTOR_ORDER && lr->order == OMX_ROTOR_ORDER &&
              ll->wpos == lr->wpos,
          "both-legs-share-one-geometry");
  OMX_PRE(p->base + p->depth <= omx_fdelay_max_delay(ll->cap, OMX_ROTOR_ORDER),
          "the-sweep-is-inside-the-ring");
  float d = 0.0f, a = 0.0f;
  omx_rotor_step(p, s, &d, &a);
  omx_fdelay_write(ll, xl);
  omx_fdelay_write(lr, xr);
  uint32_t id = 0u;
  float kf = 0.0f;
  if (omx_fdelay_split(ll, d, &id, &kf)) {
    *yl = a * omx_fdelay_read_at(ll, id, 0);
    *yr = a * omx_fdelay_read_at(lr, id, 0);
  } else {
    float c[OMX_FDELAY_MAX_TAPS];
    omx_fdelay_lagrange(OMX_ROTOR_ORDER, kf, c);
    *yl = a * omx_fdelay_read_at(ll, id, c);
    *yr = a * omx_fdelay_read_at(lr, id, c);
  }
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_ROTOR_H */
