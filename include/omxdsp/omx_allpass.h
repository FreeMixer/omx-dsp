// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_allpass.h
 * @brief The first-order all-pass section `A(z) = (a + z⁻¹)/(1 + a·z⁻¹)` in the one-multiply
 *        Gray–Markel lattice: `t = a·(x − s); y = s + t; s ← flush(x + t)`.
 *
 * The coefficient is bilinear-prewarped at `fc`, `a = (tan(π·fc/sr) − 1)/(tan(π·fc/sr) + 1)`, so
 * the phase `−2·atan(tan(ω/2)/tan(π·fc/sr))` crosses −90° exactly at `fc` at every rate. The
 * lattice conserves `y² + c·s²`, `c = (1 − a)/(1 + a)`: a block's energy in plus its stored
 * energy equals its energy out plus the stored energy it leaves. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 4 and
 * docs/design/specs/2026-09-22-native-phaser.md §1.
 */
#ifndef OMX_ALLPASS_H
#define OMX_ALLPASS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "omx_contract.h"
#include "omx_denormal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** @brief The block POST's relative energy tolerance (dsp-primitives §1 row 4: "to 1e-4"). */
#define OMX_ALLPASS1_ENERGY_TOL 1e-4

/** @brief One section's state: the lattice's delayed word `s = x[n−1] + t[n−1]`. */
struct omx_allpass1 {
  float s; /**< The delayed word, finite and flushed. */
};

/**
 * @brief The state's size, for a host that lays the state out itself.
 * @return `sizeof(struct omx_allpass1)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_allpass1_state_size(void) { return sizeof(struct omx_allpass1); }

/**
 * @brief The state's alignment, for a host that lays the state out itself.
 * @return `_Alignof(struct omx_allpass1)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_allpass1_state_align(void) { return _Alignof(struct omx_allpass1); }

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "allpass1/coef"
/**
 * @brief The prewarped coefficient for a −90° crossing at `fc`.
 * @param fc_hz The crossing, Hz, inside (0, sr/2).
 * @param sr Sample rate, Hz, a declared rate.
 * @return `a = (t − 1)/(t + 1)`, `t = tan(π·fc/sr)`, inside (−1, 1).
 * @pre `finite-fc`, `rate-is-declared`, `fc-inside-nyquist`.
 * @post `coefficient-inside-unity`.
 * @note Control-rate: one `tan`. RT-safe (no allocation, no lock) and thread-safe: pure.
 */
static inline float omx_allpass1_coef(float fc_hz, float sr) {
  OMX_PRE(fc_hz - fc_hz == 0.0f, "finite-fc");
  OMX_PRE(OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  OMX_PRE(fc_hz > 0.0f && fc_hz < 0.5f * sr, "fc-inside-nyquist");
  const double t = tan(M_PI * (double)fc_hz / (double)sr);
  const float a = (float)((t - 1.0) / (t + 1.0));
  OMX_POST(a > -1.0f && a < 1.0f, "coefficient-inside-unity");
  return a;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "allpass1/tick"
/**
 * @brief One sample through one section.
 * @param st The section, its word updated in place.
 * @param x The input sample.
 * @param a The coefficient, inside (−1, 1).
 * @return The output sample.
 * @pre `coefficient-inside-unity`, `finite-in`.
 * @post `finite-out`.
 * @invariant `state-finite-and-flushed`.
 * @note RT-safe: one multiply, no call. Thread-safe on distinct state.
 */
static inline float omx_allpass1_tick(struct omx_allpass1 *st, float x, float a) {
  OMX_PRE(a > -1.0f && a < 1.0f, "coefficient-inside-unity");
  OMX_PRE(x - x == 0.0f, "finite-in");
  const float t = a * (x - st->s);
  const float y = st->s + t;
  st->s = omx_flush(x + t);
  OMX_POST(y - y == 0.0f, "finite-out");
  OMX_INVARIANT(st->s - st->s == 0.0f && (st->s == 0.0f || fabsf(st->s) >= 1e-20f),
                "state-finite-and-flushed");
  return y;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief One sample through `n` identical sections in series.
 * @param st The `n` sections, each word updated in place.
 * @param n How many sections, 0 is a wire.
 * @param x The input sample.
 * @param a The one coefficient every section shares, inside (−1, 1).
 * @return The chain's output sample.
 * @note RT-safe: `n` multiplies. Thread-safe on distinct state.
 */
static inline float omx_allpass1_cascade(struct omx_allpass1 *st, uint32_t n, float x, float a) {
  float v = x;
  for (uint32_t k = 0; k < n; k++) v = omx_allpass1_tick(&st[k], v, a);
  return v;
}

#define OMX_CONTRACT_STAGE "allpass1/block"
/**
 * @brief A block through one section, in place, at one coefficient.
 * @param st The section, its word updated in place.
 * @param buf `n` samples, replaced by the section's output.
 * @param n The block length.
 * @param a The coefficient, inside (−1, 1).
 * @pre `coefficient-inside-unity`.
 * @post `unity-magnitude`: `Σy² + c·s_after² = Σx² + c·s_before²`, `c = (1 − a)/(1 + a)`, to
 *       `OMX_ALLPASS1_ENERGY_TOL` of the larger side.
 * @note RT-safe: one multiply per sample. Thread-safe on distinct state.
 */
static inline void omx_allpass1_block(struct omx_allpass1 *st, float *buf, uint32_t n, float a) {
  OMX_PRE(a > -1.0f && a < 1.0f, "coefficient-inside-unity");
#ifdef OMX_CONTRACTS
  const double c = (1.0 - (double)a) / (1.0 + (double)a);
  double e_in = c * (double)st->s * (double)st->s, e_out = 0.0;
#endif
  for (uint32_t i = 0; i < n; i++) {
#ifdef OMX_CONTRACTS
    e_in += (double)buf[i] * (double)buf[i];
#endif
    buf[i] = omx_allpass1_tick(st, buf[i], a);
#ifdef OMX_CONTRACTS
    e_out += (double)buf[i] * (double)buf[i];
#endif
  }
#ifdef OMX_CONTRACTS
  e_out += c * (double)st->s * (double)st->s;
#endif
  OMX_POST(fabs(e_out - e_in) <= OMX_ALLPASS1_ENERGY_TOL * (e_in > e_out ? e_in : e_out) + 1e-30,
           "unity-magnitude");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ALLPASS_H */
