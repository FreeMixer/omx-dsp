// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_rotary.h
 * @brief The ROTARY SPEAKER stage: a drum rotor on the low band and a horn rotor on the high band
 *        of one complementary split, weighed by the balance law, mixed with the dry signal.
 *
 * Spec: docs/design/specs/2026-09-26-rotary-speaker.md §2-§4. Composed of mix_rotor.h (twice),
 * `omx_biquad` over `omx_eq_design_f`'s low-pass for the split (`hi = x − lo`, exact), and
 * `omx_balance_law`. The rings are inline: nothing is allocated. Latency is zero (L1).
 */
#ifndef OMX_MIX_ROTARY_H
#define OMX_MIX_ROTARY_H

#include <stdint.h>

#include <omxdsp/omx_balance_law.h>
#include "omx_rotor.h"
#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_wetdry.h>

/** @brief The `speed` member set, by index: the row's enum order. */
enum omx_rotary_speed { OMX_ROTARY_STOP = 0, OMX_ROTARY_SLOW = 1, OMX_ROTARY_FAST = 2 };

/** @brief The resolved atom, built once per block by omx_rotary_resolve(). */
struct omx_rotary {
  int enabled;            /**< 0 → passthrough before a sample or a state word is touched. */
  struct omx_rotor drum;  /**< The low band's rotor. */
  struct omx_rotor horn;  /**< The high band's rotor. */
  float xo[5];            /**< The split's low-pass section. */
  float g_lo;             /**< The drum's balance gain, in [0, 1]. */
  float g_hi;             /**< The horn's balance gain, in [0, 1]. */
  float mix;              /**< Wet share, in [0, 1]; 0 is bit-identical dry. */
  float dry;              /**< 1 − mix, resolved once per block. */
};

/** @brief The stage's state: the split's history per leg, two rotors, four inline rings. */
struct omx_rotary_state {
  float xs_l[4];                                /**< The left leg's split section history. */
  float xs_r[4];                                /**< The right leg's split section history. */
  struct omx_rotor_state drum;                  /**< The drum's phase and rate. */
  struct omx_rotor_state horn;                  /**< The horn's phase and rate. */
  struct omx_fdelay lo_l, lo_r, hi_l, hi_r;     /**< The four lines over the rings below. */
  float ring[4][OMX_ROTARY_RING_FLOATS];        /**< Inline rings: drum L/R, horn L/R. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "rotary/init"
/**
 * @brief Zero the state and arm the four lines over the inline rings.
 * @param s The state, written.
 * @return The line's own code; OMX_FDELAY_OK when armed.
 * @post `four-lines-armed-at-the-rotor-order`.
 * @note Not RT: a `memset` of the state, done on insert. Thread-safe on distinct state.
 */
static inline enum omx_fdelay_code omx_rotary_init(struct omx_rotary_state *s) {
  memset(s, 0, sizeof *s);
  struct omx_fdelay *lines[4] = {&s->lo_l, &s->lo_r, &s->hi_l, &s->hi_r};
  for (int k = 0; k < 4; k++) {
    const enum omx_fdelay_code c =
        omx_fdelay_init(lines[k], s->ring[k], OMX_ROTARY_RING_FLOATS, OMX_ROTOR_ORDER);
    if (c != OMX_FDELAY_OK) return c;
  }
  OMX_POST(s->hi_r.order == OMX_ROTOR_ORDER, "four-lines-armed-at-the-rotor-order");
  return OMX_FDELAY_OK;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "rotary/resolve"
/**
 * @brief Resolve the controls into the atom for the next block.
 * @param p The atom, written.
 * @param s The state, read for the rotors' present rates (the accel/decel choice).
 * @param enabled The row's `on`.
 * @param speed The `speed` member index.
 * @param horn_slow_hz The horn's slow rate, Hz.
 * @param horn_fast_hz The horn's fast rate, Hz.
 * @param drum_slow_hz The drum's slow rate, Hz.
 * @param drum_fast_hz The drum's fast rate, Hz.
 * @param accel The scale on every speed-change time, positive.
 * @param balance The balance, in [−1, 1]: −1 drum only, +1 horn only.
 * @param mix The wet share, in [0, 1].
 * @param sr The live rate, a declared rate.
 * @pre `a-speed-is-a-member`, `mix-in-unit-range`.
 * @post `balance-gains-never-boost`.
 * @note Per block: two `expf` and the split's design. Thread-safe on distinct atoms.
 */
static inline void omx_rotary_resolve(struct omx_rotary *p, const struct omx_rotary_state *s,
                                      int enabled, int speed, float horn_slow_hz,
                                      float horn_fast_hz, float drum_slow_hz, float drum_fast_hz,
                                      float accel, float balance, float mix, float sr) {
  OMX_PRE(speed >= OMX_ROTARY_STOP && speed <= OMX_ROTARY_FAST, "a-speed-is-a-member");
  OMX_PRE(mix >= 0.0f && mix <= 1.0f, "mix-in-unit-range");
  const float horn_t = speed == OMX_ROTARY_FAST ? horn_fast_hz : speed == OMX_ROTARY_SLOW ? horn_slow_hz : 0.0f;
  const float drum_t = speed == OMX_ROTARY_FAST ? drum_fast_hz : speed == OMX_ROTARY_SLOW ? drum_slow_hz : 0.0f;
  p->enabled = enabled;
  omx_rotor_resolve(&p->horn, &s->horn, horn_t, OMX_ROTARY_HORN_ACCEL_MS, OMX_ROTARY_HORN_DECEL_MS,
                    accel, OMX_ROTARY_HORN_DOPPLER_MS, OMX_ROTARY_HORN_AM, sr);
  omx_rotor_resolve(&p->drum, &s->drum, drum_t, OMX_ROTARY_DRUM_ACCEL_MS, OMX_ROTARY_DRUM_DECEL_MS,
                    accel, OMX_ROTARY_DRUM_DOPPLER_MS, OMX_ROTARY_DRUM_AM, sr);
  double q[2];
  omx_eq_butterworth_qs(0, q);
  omx_eq_design_f(OMX_EQ_LOWPASS, OMX_ROTARY_CROSSOVER_HZ, q[0], 0.0, sr, p->xo);
  omx_balance_law(balance, &p->g_lo, &p->g_hi);
  p->mix = mix;
  p->dry = 1.0f - mix;
  OMX_POST(p->g_lo >= 0.0f && p->g_lo <= 1.0f && p->g_hi >= 0.0f && p->g_hi <= 1.0f,
           "balance-gains-never-boost");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "rotary"
/**
 * @brief Process one block in place: split each leg, run the drum on the low band and the horn on
 *        the high band (one phase each, both legs), weigh, mix.
 * @param l The left leg.
 * @param r The right leg.
 * @param n Frames.
 * @param p The atom.
 * @param s The state.
 * @pre `finite-in` (the atom's `mix` range is the resolve's PRE).
 * @post `finite-out`, per sample, by omx_wetdry_mix().
 * @note RT-safe: per sample two sections, two rotor ticks (four four-tap reads), no libm, no
 *       allocation. `enabled == 0` returns before a word is touched; `mix == 0` returns the
 *       block bit-identical after advancing the rotors (a muted mic does not stop the cabinet).
 *       Thread-safe on distinct state.
 */
static inline void omx_rotary_process(float *l, float *r, uint32_t n, const struct omx_rotary *p,
                                      struct omx_rotary_state *s) {
  if (!p->enabled || n == 0u) return;
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
  if (p->mix <= 0.0f) {
    float d = 0.0f, a = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
      omx_rotor_step(&p->drum, &s->drum, &d, &a);
      omx_rotor_step(&p->horn, &s->horn, &d, &a);
    }
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    const float lo_l = omx_flush(omx_biquad(l[i], p->xo, s->xs_l));
    const float lo_r = omx_flush(omx_biquad(r[i], p->xo, s->xs_r));
    float dl = 0.0f, dr = 0.0f, hl = 0.0f, hr = 0.0f;
    omx_rotor_tick(&p->drum, &s->drum, &s->lo_l, &s->lo_r, lo_l, lo_r, &dl, &dr);
    omx_rotor_tick(&p->horn, &s->horn, &s->hi_l, &s->hi_r, l[i] - lo_l, r[i] - lo_r, &hl, &hr);
    const float wet_l = p->g_lo * dl + p->g_hi * hl, wet_r = p->g_lo * dr + p->g_hi * hr;
    omx_wetdry_mix(&l[i], &r[i], l[i], r[i], wet_l, wet_r, p->dry, p->mix);
  }
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The stage's latency, samples: zero at every rate (L1 — the Doppler delay is the effect).
 * @return 0.
 * @note RT-safe and thread-safe: a constant.
 */
static inline uint32_t omx_rotary_latency(void) { return 0u; }

#endif /* OMX_MIX_ROTARY_H */
