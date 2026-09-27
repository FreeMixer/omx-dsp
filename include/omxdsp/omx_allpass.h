// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_allpass.h
 * @brief The all-pass sections: the first-order Gray–Markel lattice and the second-order design.
 *
 * First order: `A(z) = (a + z⁻¹)/(1 + a·z⁻¹)`, `a = (K − 1)/(K + 1)`, `K = tan(π·fc/sr)`, phase
 * `−2·atan(tan(ω/2)/K)`, −90° at `fc`. It is lossless with the stored energy `P·s²`,
 * `P = (1 − a)/(1 + a) = 1/K`: over any block `Σy² + P·s_end² = Σx² + P·s_start²`. Second order:
 * the cookbook section at `Q` whose numerator is its reversed denominator, −180° at `fc`. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 rows 4–5 and Appendix A.
 */
#ifndef OMX_ALLPASS_H
#define OMX_ALLPASS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "omx_contract.h"
#include "omx_contract_limits.h"
#include "omx_denormal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief The bilinear prewarp of a corner, `K = tan(π·fc/sr)` — the one frequency map of every
 *        all-pass and crossover design.
 * @param fc Corner, Hz; inside (0, sr/2).
 * @param sr Sample rate, Hz; positive.
 * @return `tan(π·fc/sr)`, positive for a corner inside the band.
 * @note RT-safe: one `tan`, on the RT-safe allowlist. Thread-safe: pure.
 */
static inline double omx_allpass_prewarp(double fc, double sr) {
  return tan(M_PI * fc / sr);
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "allpass1/coeff"
/**
 * @brief The first-order section's coefficient for a −90° corner at `fc`: `a = (K − 1)/(K + 1)`.
 * @param fc Corner, Hz; inside (0, sr/2).
 * @param sr Sample rate, Hz; a declared rate.
 * @return The coefficient, in (−1, 1); a refused corner answers 0.
 * @pre `corner-inside-the-band`, `rate-is-declared`.
 * @post `coefficient-inside-unity`.
 * @note RT-safe: omx_allpass_prewarp() and one divide. Thread-safe: pure.
 */
static inline double omx_allpass1_coeff(double fc, double sr) {
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

/** @brief The first-order section's state: the lattice's one delayed word. */
struct omx_allpass1_state {
  float s; /**< `x[n−1] + t[n−1]`, flushed below OMX_FLUSH_THRESHOLD. */
};

/**
 * @brief The state's size, for a host that lays the state out itself.
 * @return `sizeof(struct omx_allpass1_state)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_allpass1_state_size(void) { return sizeof(struct omx_allpass1_state); }

/**
 * @brief The state's alignment, for a host that lays the state out itself.
 * @return `_Alignof(struct omx_allpass1_state)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_allpass1_state_align(void) { return _Alignof(struct omx_allpass1_state); }

#define OMX_CONTRACT_STAGE "allpass1"
/**
 * @brief One sample of the first-order section, the one-multiply lattice:
 *        `t = a·(x − s); y = s + t; s ← flush(x + t)`.
 * @param x The input sample; finite.
 * @param a The coefficient (omx_allpass1_coeff()), inside (−1, 1).
 * @param st The section's state, updated in place.
 * @return The output sample.
 * @pre `coefficient-inside-unity`, `finite-in`.
 * @post `finite-out`.
 * @invariant `state-finite-and-flushed`: the state word is finite and zero or at least
 *            OMX_FLUSH_THRESHOLD in magnitude, before and after.
 * @note RT-safe: one multiply, no call but omx_flush(). Thread-safe on distinct state.
 */
static inline float omx_allpass1(float x, float a, struct omx_allpass1_state *st) {
  OMX_PRE(fabsf(a) < 1.0f, "coefficient-inside-unity");
  OMX_PRE(x - x == 0.0f, "finite-in");
  OMX_INVARIANT(st->s - st->s == 0.0f && (st->s == 0.0f || fabsf(st->s) >= OMX_FLUSH_THRESHOLD),
                "state-finite-and-flushed");
  const float t = a * (x - st->s);
  const float y = st->s + t;
  st->s = omx_flush(x + t);
  OMX_POST(y - y == 0.0f, "finite-out");
  OMX_INVARIANT(st->s - st->s == 0.0f && (st->s == 0.0f || fabsf(st->s) >= OMX_FLUSH_THRESHOLD),
                "state-finite-and-flushed");
  return y;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "allpass1/process"
/**
 * @brief The first-order section over a block in place: omx_allpass1() per sample.
 * @param buf The block, filtered in place; finite.
 * @param n Samples in the block.
 * @param a The coefficient (omx_allpass1_coeff()), inside (−1, 1).
 * @param st The section's state, updated in place.
 * @pre `coefficient-inside-unity`, `finite-in`.
 * @post `finite-out`; `unity-magnitude`: `Σy² + P·s_end²` equals `Σx² + P·s_start²`,
 *       `P = (1 − a)/(1 + a)`, to OMX_ALLPASS_UNITY_TOL of the larger side, plus what the flush
 *       may discard (`n·P·OMX_FLUSH_THRESHOLD²`).
 * @invariant `state-finite-and-flushed`, on entry and on return.
 * @note RT-safe: one multiply per sample, no call but the inlined omx_allpass1(). Thread-safe on
 *       distinct state.
 */
static inline void omx_allpass1_process(float *buf, uint32_t n, float a, struct omx_allpass1_state *st) {
  OMX_PRE(fabsf(a) < 1.0f, "coefficient-inside-unity");
  OMX_PRE(omx_block_finite(buf, n), "finite-in");
  OMX_INVARIANT(st->s - st->s == 0.0f && (st->s == 0.0f || fabsf(st->s) >= OMX_FLUSH_THRESHOLD),
                "state-finite-and-flushed");
#ifdef OMX_CONTRACTS
  const double p = (1.0 - (double)a) / (1.0 + (double)a);
  double e_in = p * (double)st->s * st->s, e_out = 0.0;
  for (uint32_t i = 0; i < n; i++) e_in += (double)buf[i] * buf[i];
#endif
  for (uint32_t i = 0; i < n; i++) buf[i] = omx_allpass1(buf[i], a, st);
#ifdef OMX_CONTRACTS
  e_out = p * (double)st->s * st->s;
  for (uint32_t i = 0; i < n; i++) e_out += (double)buf[i] * buf[i];
  const double flushed = (double)n * p * (double)OMX_FLUSH_THRESHOLD * (double)OMX_FLUSH_THRESHOLD;
  OMX_POST(fabs(e_out - e_in) <= (double)OMX_ALLPASS_UNITY_TOL * fmax(e_in, e_out) + flushed, "unity-magnitude");
#endif
  OMX_POST(omx_block_finite(buf, n), "finite-out");
  OMX_INVARIANT(st->s - st->s == 0.0f && (st->s == 0.0f || fabsf(st->s) >= OMX_FLUSH_THRESHOLD),
                "state-finite-and-flushed");
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
  OMX_POST(isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2]) && isfinite(c[3]) && isfinite(c[4]),
           "finite-coeffs");
  OMX_POST(fabs(c[3]) < 1.0 + c[4] && c[4] < 1.0, "poles-inside-the-unit-circle");
  OMX_POST(c[0] == c[4] && c[1] == c[3] && c[2] == 1.0, "allpass-unity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ALLPASS_H */
