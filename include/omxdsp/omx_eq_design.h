// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_eq_design.h
 * @brief The EQ section design in C, the twin of core's `rbjSection` (eq.ts): each kind's
 *        design, matched-Z or cookbook, is the one its `omx_eq_kind` member names.
 *
 * Doubles throughout, narrowed to float at the very end, so a section designed here and one
 * designed in TypeScript run the same bits; held equal by
 * `packages/audio-engine/src/eq-lv2-design-twins.test.ts`. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 3 and Appendix A.
 */
#ifndef OMX_EQ_DESIGN_H
#define OMX_EQ_DESIGN_H

#include <math.h>
#include <stdint.h>

#include "omx_contract.h"
#include "omx_matched_pair.h"

/** @brief The biquad kinds `rbjSection` designs, in core's `BiquadKind` order. */
enum omx_eq_kind {
  OMX_EQ_PEAKING = 0,   /**< A bell, matched pole and zero pairs. */
  OMX_EQ_LOWSHELF = 1,  /**< A low shelf, DC gain `A²`. */
  OMX_EQ_HIGHSHELF = 2, /**< A high shelf, DC gain unity. */
  OMX_EQ_HIGHPASS = 3,  /**< The cookbook high-pass. */
  OMX_EQ_LOWPASS = 4,   /**< Matched poles, fitted numerator. */
  OMX_EQ_NOTCH = 5,     /**< Matched poles under the cookbook numerator, a total null. */
  OMX_EQ_BANDPASS = 6,  /**< The cookbook constant-0 dB-peak bandpass, cookbook poles: `½(1 − AP₂)`. */
  OMX_EQ_ALLPASS1 = 7,  /**< A first-order allpass `(k + z⁻¹)/(1 + k z⁻¹)`, −90° at the corner. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "eq-design/matched-pair"
/**
 * @brief One conjugate pair placed by the matched-Z map, as `{p1, p2, dc}`.
 * @param wn Angular frequency, radians per sample, positive; held at or below π.
 * @param zeta Damping ratio, positive.
 * @param out `{p1, p2, dc}`: the z⁻¹ and z⁻² coefficients and the factored value at `z = 1`.
 * @pre `centre-and-damping-positive`.
 * @post `pair-inside-the-unit-circle`, `dc-positive`.
 * @note RT-safe: `exp`, `cos`, `sin`, `sqrt` on the RT-safe allowlist. Thread-safe: pure.
 */
static inline void omx_eq_matched_pair(double wn, double zeta, double out[3]) {
  OMX_PRE(wn > 0.0 && zeta > 0.0, "centre-and-damping-positive");
  const double w = wn > M_PI ? M_PI : wn;
  if (zeta < 1.0) {
    const double e = exp(-zeta * w), th = sqrt(1.0 - zeta * zeta) * w;
    const double ec = e * cos(th), es = e * sin(th);
    out[0] = -2.0 * ec;
    out[1] = e * e;
    out[2] = (1 - ec) * (1 - ec) + es * es;
  } else {
    const double sq = sqrt(zeta * zeta - 1.0);
    const double z1 = exp(-w / (zeta + sq)), z2 = exp(-w * (zeta + sq));
    out[0] = -(z1 + z2);
    out[1] = z1 * z2;
    out[2] = (1 - z1) * (1 - z2);
  }
  OMX_POST(out[1] >= 0.0 && out[1] < 1.0, "pair-inside-the-unit-circle");
  OMX_POST(out[2] > 0.0 && isfinite(out[2]), "dc-positive");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "eq-design/section"
/**
 * @brief Design one section as the normalised `{b0, b1, b2, a1, a2}` omx_biquad() runs, term
 *        for term core's `rbjCoeffs`.
 * @param kind The section kind.
 * @param freq_hz Centre or corner, Hz; clamped to `[1, 0.999·Nyquist]`.
 * @param q Quality factor; a non-positive value reads as 1e-3.
 * @param gain_db Gain, dB; read by the peaking and shelf kinds only.
 * @param sample_rate Sample rate, Hz, positive.
 * @param c The five coefficients, in double.
 * @pre `finite-params-and-positive-rate`.
 * @post `finite-coeffs`; `poles-inside-the-unit-circle` (`|a1| < 1 + a2` and `a2 < 1`).
 * @note RT-safe: `exp`, `cos`, `sin`, `sqrt` on the RT-safe allowlist (no `pow`). Thread-safe: pure.
 */
static inline void omx_eq_design(enum omx_eq_kind kind, double freq_hz, double q, double gain_db,
                                 double sample_rate, double c[5]) {
  OMX_PRE(sample_rate > 0.0 && isfinite(freq_hz) && isfinite(q) && isfinite(gain_db),
          "finite-params-and-positive-rate");
  const double nyquist = sample_rate * 0.5;
  const double lo = 1.0, hi = nyquist * 0.999;
  const double f0 = freq_hz < lo ? lo : (freq_hz > hi ? hi : freq_hz);
  const double qq = q <= 0.0 ? 1e-3 : q;
  const double w0 = (2.0 * M_PI * f0) / sample_rate;
  const double A = exp(gain_db * (M_LN10 / 40.0));
  double p[3], z[3];
  switch (kind) {
    case OMX_EQ_PEAKING: {
      omx_eq_matched_pair(w0, 1.0 / (2.0 * A * qq), p);
      omx_eq_matched_pair(w0, A / (2.0 * qq), z);
      const double k = p[2] / z[2];
      c[0] = k; c[1] = k * z[0]; c[2] = k * z[1]; c[3] = p[0]; c[4] = p[1];
      break;
    }
    case OMX_EQ_LOWSHELF: {
      const double sa = sqrt(A);
      omx_eq_matched_pair(w0 / sa, 1.0 / (2.0 * qq), p);
      omx_eq_matched_pair(w0 * sa, 1.0 / (2.0 * qq), z);
      const double k = (A * A * p[2]) / z[2];
      c[0] = k; c[1] = k * z[0]; c[2] = k * z[1]; c[3] = p[0]; c[4] = p[1];
      break;
    }
    case OMX_EQ_HIGHSHELF: {
      const double sa = sqrt(A);
      omx_eq_matched_pair(w0 * sa, 1.0 / (2.0 * qq), p);
      omx_eq_matched_pair(w0 / sa, 1.0 / (2.0 * qq), z);
      const double k = p[2] / z[2];
      c[0] = k; c[1] = k * z[0]; c[2] = k * z[1]; c[3] = p[0]; c[4] = p[1];
      break;
    }
    case OMX_EQ_HIGHPASS: {
      const double cw = cos(w0);
      const double alpha = sin(w0) / (2.0 * qq);
      const double a0 = 1.0 + alpha;
      c[0] = (1.0 + cw) / 2.0 / a0;
      c[1] = -(1.0 + cw) / a0;
      c[2] = (1.0 + cw) / 2.0 / a0;
      c[3] = -2.0 * cw / a0;
      c[4] = (1.0 - alpha) / a0;
      break;
    }
    case OMX_EQ_LOWPASS: {
      omx_eq_matched_pair(w0, 1.0 / (2.0 * qq), p);
      const double xn = nyquist / f0;
      const double un = 1.0 - xn * xn;
      const double tPi = 1.0 / (un * un + (xn / qq) * (xn / qq));
      const double dPi = 1.0 - p[0] + p[1];
      const double cw = cos(w0);
      const double dr = 1.0 + p[0] * cw + p[1] * cos(2.0 * w0);
      const double di = -(p[0] * sin(w0) + p[1] * sin(2.0 * w0));
      const double s0 = sin(w0 / 2.0) * sin(w0 / 2.0);
      const double U = p[2] * p[2];
      const OmxMatchedNumerator n =
          omx_matched_fit_numerator(U, tPi * dPi * dPi, qq * qq * (dr * dr + di * di), s0);
      c[0] = n.b0; c[1] = n.b1; c[2] = n.b2; c[3] = p[0]; c[4] = p[1];
      break;
    }
    case OMX_EQ_BANDPASS: {
      const double alpha = sin(w0) / (2.0 * qq);
      const double a0 = 1.0 + alpha;
      c[0] = alpha / a0; c[1] = 0.0; c[2] = -alpha / a0; c[3] = -2.0 * cos(w0) / a0; c[4] = (1.0 - alpha) / a0;
      break;
    }
    case OMX_EQ_ALLPASS1: {
      const double t = sin(w0) / (1.0 + cos(w0));
      const double k = (t - 1.0) / (t + 1.0);
      c[0] = k; c[1] = 1.0; c[2] = 0.0; c[3] = k; c[4] = 0.0;
      break;
    }
    case OMX_EQ_NOTCH:
    default: {
      const double b1 = -2.0 * cos(w0);
      omx_eq_matched_pair(w0, 1.0 / (2.0 * qq), p);
      const double sw2 = sin(w0 / 2.0);
      const double k = p[2] / (4.0 * sw2 * sw2);
      c[0] = k; c[1] = k * b1; c[2] = k; c[3] = p[0]; c[4] = p[1];
      break;
    }
  }
  OMX_POST(isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2]) && isfinite(c[3]) && isfinite(c[4]),
           "finite-coeffs");
  OMX_POST(fabs(c[3]) < 1.0 + c[4] && c[4] < 1.0, "poles-inside-the-unit-circle");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "eq-design/section-float"
/**
 * @brief omx_eq_design() narrowed to the float tuple the cascade runs.
 * @param kind The section kind.
 * @param freq_hz Centre or corner, Hz.
 * @param q Quality factor.
 * @param gain_db Gain, dB.
 * @param sample_rate Sample rate, Hz, positive.
 * @param out The five float coefficients.
 * @post `finite-float-coeffs`.
 * @note RT-safe and thread-safe: omx_eq_design() and five narrowing stores.
 */
static inline void omx_eq_design_f(enum omx_eq_kind kind, double freq_hz, double q,
                                   double gain_db, double sample_rate, float out[5]) {
  double c[5];
  omx_eq_design(kind, freq_hz, q, gain_db, sample_rate, c);
  for (int i = 0; i < 5; i++) out[i] = (float)c[i];
  OMX_POST(isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]) && isfinite(out[3]) &&
               isfinite(out[4]),
           "finite-float-coeffs");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "eq-design/butterworth"
/**
 * @brief The per-section Q values of a Butterworth pass filter: 12 dB/oct is one section at
 *        `1/√2`, 24 dB/oct two whose 4th-order product is maximally flat.
 * @param slope_24 Non-zero for 24 dB/oct, zero for 12 dB/oct.
 * @param q_out The Q of each section, in order.
 * @return The section count, 1 or 2.
 * @post `one-or-two-sections`.
 * @note RT-safe and thread-safe: constants.
 */
static inline uint32_t omx_eq_butterworth_qs(int slope_24, double q_out[2]) {
  uint32_t n;
  if (slope_24) {
    q_out[0] = 0.5411961;
    q_out[1] = 1.3065630;
    n = 2u;
  } else {
    q_out[0] = M_SQRT1_2;
    n = 1u;
  }
  OMX_POST(n == 1u || n == 2u, "one-or-two-sections");
  return n;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_EQ_DESIGN_H */
