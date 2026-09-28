// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_allpass.h
 * @brief The all-pass sections: the first-order Gray–Markel lattice (RULED 2026-09-23,
 *        docs/design/specs/2026-09-22-native-phaser.md §7) and the second-order design.
 *
 * First order: `A(z) = (a + z⁻¹)/(1 + a·z⁻¹)` in the one-multiply lattice
 * `t = a·(x − s); y = s + t; s ← flush(x + t)`, coefficient bilinear-prewarped at `fc`,
 * `a = (K − 1)/(K + 1)`, `K = tan(π·fc/sr)`, phase `−2·atan(tan(ω/2)/K)`, −90° at `fc`. It is
 * lossless with the stored energy `P·s²`, `P = (1 − a)/(1 + a) = 1/K`: over any block
 * `Σy² + P·s_end² = Σx² + P·s_start²`. The coefficient is float (`omx_allpass1_coef()`); a caller
 * whose corner is designed once and run through a double-precision cascade (the crossover) needs
 * the `_d` sibling below instead — the same formula, kept from losing the corner's precision on
 * the float round-trip a per-sample-swept caller (the phaser) does not need. Second order: the
 * cookbook section at `Q` whose numerator is its reversed denominator, −180° at `fc`. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 rows 4–5 and Appendix A.
 */
#ifndef OMX_ALLPASS_H
#define OMX_ALLPASS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "omx_biquad.h"
#include "omx_contract.h"
#include "omx_contract_limits.h"
#include "omx_denormal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
  OMX_INVARIANT(st->s - st->s == 0.0f && (st->s == 0.0f || fabsf(st->s) >= OMX_FLUSH_THRESHOLD),
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
 *       `OMX_ALLPASS_UNITY_TOL` of the larger side.
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
  OMX_POST(fabs(e_out - e_in) <= OMX_ALLPASS_UNITY_TOL * (e_in > e_out ? e_in : e_out) + 1e-30,
           "unity-magnitude");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The bilinear prewarp of a corner, `K = tan(π·fc/sr)` — the one frequency map every
 *        double-precision all-pass and crossover design shares.
 * @param fc Corner, Hz; inside (0, sr/2).
 * @param sr Sample rate, Hz; positive.
 * @return `tan(π·fc/sr)`, positive for a corner inside the band.
 * @note RT-safe: one `tan`, on the RT-safe allowlist. Thread-safe: pure.
 */
static inline double omx_allpass_prewarp(double fc, double sr) {
  return tan(M_PI * fc / sr);
}

#define OMX_CONTRACT_STAGE "allpass1/coef_d"
/**
 * @brief The double-precision sibling of omx_allpass1_coef(): the same `a = (K − 1)/(K + 1)`
 *        formula, kept in double for a caller whose corner is designed once and run through a
 *        double-precision state (the crossover's `omx_biquad_tdf2_d()` cascade), rather than swept
 *        every sample the way the phaser sweeps `omx_allpass1_coef()`.
 *
 * Measured (docs/design/specs/2026-09-26-dsp-primitives.md §1, allpass1_coef_d row): against the
 * crossover's own `bands-partition-unity` tolerance (`OMX_XOVER_PARTITION_TOL = 1e-5` relative),
 * this double coefficient leaves the worst residual at ~1.2e-7 across the declared rates and five
 * representative corners — three orders of margin. Narrowing through `omx_allpass1_coef()`'s float
 * result and back leaves as little as ~9.5e-6 at the worst of those same corners (fc = 20 Hz, high
 * rates): 95% of the tolerance already spent on five convenient, exactly-float-representable
 * corners, before a real user's non-round corner erodes the margin further. The double form is not
 * decoration.
 * @param fc Corner, Hz; inside (0, sr/2).
 * @param sr Sample rate, Hz; a declared rate.
 * @return The coefficient, in (−1, 1); a refused corner answers 0.
 * @pre `corner-inside-the-band`, `rate-is-declared`.
 * @post `coefficient-inside-unity`.
 * @note RT-safe: omx_allpass_prewarp() and one divide. Thread-safe: pure.
 */
static inline double omx_allpass1_coef_d(double fc, double sr) {
  const int inside = sr > 0.0 && fc > 0.0 && fc < 0.5 * sr;
  OMX_PRE(inside, "corner-inside-the-band");
  OMX_PRE(OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  double a = 0.0;
  if (inside) {
    const double K = omx_allpass_prewarp(fc, sr);
    a = (K - 1.0) / (K + 1.0);
  }
  OMX_POST(fabs(a) < 1.0, "coefficient-inside-unity");
  return a;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "allpass2/design"
/**
 * @brief The second-order all-pass at `fc` and `q` as the normalised `{b0, b1, b2, a1, a2}`
 *        omx_biquad() and omx_biquad_d() run: the cookbook denominator
 *        `{1, 2(K² − 1)/a0, (1 − K/q + K²)/a0}`, `a0 = 1 + K/q + K²`, and the numerator its
 *        reverse, `{a2, a1, 1}`.
 * @param fc Corner, Hz; inside (0, sr/2).
 * @param q Quality factor; positive.
 * @param sr Sample rate, Hz; a declared rate.
 * @param c The five coefficients, in double; a refused design is the identity `{1, 0, 0, 0, 0}`.
 * @pre `corner-inside-the-band`, `rate-is-declared`, `q-positive`.
 * @post `finite-coeffs`; `poles-inside-the-unit-circle` (`|a1| < 1 + a2` and `a2 < 1`);
 *       `allpass-unity` (the numerator is the reversed denominator, bit for bit).
 * @note RT-safe: omx_allpass_prewarp() and one divide. Thread-safe: pure.
 */
static inline void omx_allpass2_design(double fc, double q, double sr, double c[5]) {
  const int inside = sr > 0.0 && fc > 0.0 && fc < 0.5 * sr;
  OMX_PRE(inside, "corner-inside-the-band");
  OMX_PRE(OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  OMX_PRE(q > 0.0, "q-positive");
  c[0] = 1.0; c[1] = 0.0; c[2] = 0.0; c[3] = 0.0; c[4] = 0.0;
  if (inside && q > 0.0) {
    const double K = omx_allpass_prewarp(fc, sr);
    const double norm = 1.0 / (1.0 + K / q + K * K);
    c[3] = 2.0 * (K * K - 1.0) * norm;
    c[4] = (1.0 - K / q + K * K) * norm;
    c[0] = c[4];
    c[1] = c[3];
    c[2] = 1.0;
  }
  OMX_POST(omx_block_finite_d(c, 5u), "finite-coeffs");
  OMX_POST(omx_biquad_stable(c), "poles-inside-the-unit-circle");
  OMX_POST(c[0] == c[4] && c[1] == c[3] && c[2] == 1.0, "allpass-unity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ALLPASS_H */
