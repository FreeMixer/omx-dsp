// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_eq_design.h
 * @brief The EQ section design in C, the twin of core's `rbjSection` (eq.ts): each kind's
 *        design, matched-Z or cookbook, is the one its `omx_eq_kind` member names.
 *
 * Doubles throughout, narrowed to float at the very end, so a section designed here and one
 * designed in TypeScript run the same bits; held equal by
 * the console's EQ design twin test.
 */
#ifndef OMX_EQ_DESIGN_H
#define OMX_EQ_DESIGN_H

#include <math.h>
#include <stdint.h>

#include "omx_allpass.h"
#include "omx_biquad.h"
#include "omx_contract.h"
#include "omx_matched_pair.h"

/** @brief The biquad kinds `rbjSection` designs, in core's `BiquadKind` order. */
enum omx_eq_kind {
  OMX_EQ_PEAKING = 0,   /**< A bell, matched pole and zero pairs. */
  OMX_EQ_LOWSHELF = 1,  /**< A low shelf, DC gain `A²`. */
  OMX_EQ_HIGHSHELF = 2, /**< A high shelf, DC gain unity. */
  OMX_EQ_HIGHPASS = 3,  /**< The cookbook high-pass. */
  OMX_EQ_LOWPASS = 4,   /**< Matched poles, fitted numerator; bilinear poles near Nyquist. */
  OMX_EQ_NOTCH = 5,     /**< Matched poles under the cookbook numerator, a total null. */
  OMX_EQ_BANDPASS = 6,  /**< The cookbook constant-0 dB-peak bandpass, cookbook poles: `½(1 − AP₂)`. */
  OMX_EQ_ALLPASS1 = 7,  /**< A first-order allpass `(k + z⁻¹)/(1 + k z⁻¹)`, −90° at the corner. */
  OMX_EQ_ALLPASS2 = 8,  /**< The second-order allpass, −180° at the corner: omx_allpass2_design(). */
};

/** @brief The low-pass hand-over (omx-dsp#105): the corner band, as fractions of Nyquist, across
 *         which the low-pass's poles move from the matched pair to the prewarped bilinear pair.
 *         It depends on Q, because the matched design lifts the passband sooner the higher its Q:
 *         at Q = 1/√2 (the 12 dB Butterworth) it starts at 0.6 and ends at 0.85; at Q = 0.5412
 *         (the low-Q half of the 24 dB Butterworth) at 0.8 and 0.9; between them it moves
 *         linearly, and below Q = 0.5412 it holds that band. Above 1/√2 the prototype peaks and
 *         the band's end moves down to 0.8 by Q = 0.9 (omx-dsp#116), held above it. Below the band
 *         the low-pass is the matched design unchanged. */
#define OMX_EQ_LOWPASS_HANDOVER_Q_HI 0.70710678118654752
/** @brief The low Q end of the hand-over table: Q = 0.5412, the low-Q half of the 24 dB Butterworth. */
#define OMX_EQ_LOWPASS_HANDOVER_Q_LO 0.54119610014619698
/** @brief Where the hand-over starts at the high Q end, as a fraction of Nyquist. */
#define OMX_EQ_LOWPASS_HANDOVER_LO_AT_Q_HI 0.6
/** @brief Where the hand-over ends at the high Q end, as a fraction of Nyquist. */
#define OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_HI 0.85
/** @brief Where the hand-over starts at the low Q end, as a fraction of Nyquist. */
#define OMX_EQ_LOWPASS_HANDOVER_LO_AT_Q_LO 0.8
/** @brief Where the hand-over ends at the low Q end, as a fraction of Nyquist. */
#define OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_LO 0.9
/** @brief The peaked end of the hand-over table (omx-dsp#116): from 1/√2 to this Q the band's end
 *         moves from 0.85 to OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_PEAK and the fitted Nyquist target is
 *         handed over to the cookbook's Nyquist zero (omx_eq_lowpass_nyquist_handover). */
#define OMX_EQ_LOWPASS_HANDOVER_Q_PEAK 0.9
/** @brief Where the hand-over ends at the peaked end, as a fraction of Nyquist. */
#define OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_PEAK 0.8

/**
 * @brief How far a peaked low-pass hands its Nyquist target over: 0 at or below Q = 1/√2, 1 at or
 *        above OMX_EQ_LOWPASS_HANDOVER_Q_PEAK, linear between.
 * @param q Quality factor, positive.
 * @return The weight, in [0, 1].
 * @note RT-safe and thread-safe: arithmetic only.
 */
static inline double omx_eq_lowpass_nyquist_handover(double q) {
  const double v = (q - OMX_EQ_LOWPASS_HANDOVER_Q_HI) / (OMX_EQ_LOWPASS_HANDOVER_Q_PEAK - OMX_EQ_LOWPASS_HANDOVER_Q_HI);
  return v <= 0.0 ? 0.0 : (v >= 1.0 ? 1.0 : v);
}

/**
 * @brief How far a low-Q low-pass raises its Nyquist target across the hand-over (omx-dsp#119): 0
 *        at or above Q = 1/√2, 1 at or below OMX_EQ_LOWPASS_HANDOVER_Q_LO, linear between. At
 *        the band's end the target is the prototype's Nyquist value times one plus this weight.
 * @param q Quality factor, positive.
 * @return The weight, in [0, 1].
 * @note RT-safe and thread-safe: arithmetic only.
 */
static inline double omx_eq_lowpass_nyquist_raise(double q) {
  const double u = (OMX_EQ_LOWPASS_HANDOVER_Q_HI - q) / (OMX_EQ_LOWPASS_HANDOVER_Q_HI - OMX_EQ_LOWPASS_HANDOVER_Q_LO);
  return u <= 0.0 ? 0.0 : (u >= 1.0 ? 1.0 : u);
}

/**
 * @brief The low-pass hand-over band for a Q, as fractions of Nyquist.
 * @param q Quality factor, positive.
 * @param band `{start, end}`: the matched design below `start`, the bilinear poles from `end`.
 * @note RT-safe and thread-safe: arithmetic only.
 */
static inline void omx_eq_lowpass_handover(double q, double band[2]) {
  double u = (OMX_EQ_LOWPASS_HANDOVER_Q_HI - q) / (OMX_EQ_LOWPASS_HANDOVER_Q_HI - OMX_EQ_LOWPASS_HANDOVER_Q_LO);
  u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
  band[0] = OMX_EQ_LOWPASS_HANDOVER_LO_AT_Q_HI + (OMX_EQ_LOWPASS_HANDOVER_LO_AT_Q_LO - OMX_EQ_LOWPASS_HANDOVER_LO_AT_Q_HI) * u;
  band[1] = OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_HI + (OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_LO - OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_HI) * u;
  band[1] += (OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_PEAK - OMX_EQ_LOWPASS_HANDOVER_HI_AT_Q_HI) * omx_eq_lowpass_nyquist_handover(q);
}

#undef OMX_CONTRACT_STAGE
/** @brief The lowest Q a section is designed at: a Q below it, a non-positive one included, reads as this. */
#define OMX_EQ_Q_MIN 1e-3
/** @brief The highest Q a section is designed at (a Q above it reads as this): ten thousand times the widest
 *         Q any control travels to, and the last before the poles round onto the unit circle in double. */
#define OMX_EQ_Q_MAX 1e6

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
 * @param q Quality factor; held in [OMX_EQ_Q_MIN, OMX_EQ_Q_MAX], so a non-positive value reads as 1e-3.
 * @param gain_db Gain, dB; read by the peaking and shelf kinds only.
 * @param sample_rate Sample rate, Hz, positive.
 * @param c The five coefficients, in double.
 * @pre `finite-params-and-positive-rate`.
 * @post `finite-coeffs`; `poles-inside-the-unit-circle` (`|a1| < 1 + a2` and `a2 < 1`).
 * @note RT-safe: `exp`, `cos`, `sin`, `sqrt` on the RT-safe allowlist (no `pow`). Thread-safe: pure.
 * @twin rbjCoeffs
 */
static inline void omx_eq_design(enum omx_eq_kind kind, double freq_hz, double q, double gain_db,
                                 double sample_rate, double c[5]) {
  OMX_PRE(sample_rate > 0.0 && isfinite(freq_hz) && isfinite(q) && isfinite(gain_db),
          "finite-params-and-positive-rate");
  const double nyquist = sample_rate * 0.5;
  const double lo = 1.0, hi = nyquist * 0.999;
  const double f0 = freq_hz < lo ? lo : (freq_hz > hi ? hi : freq_hz);
  const double qq = q < OMX_EQ_Q_MIN ? OMX_EQ_Q_MIN : (q > OMX_EQ_Q_MAX ? OMX_EQ_Q_MAX : q);
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
      /* Matched poles and a numerator fitted to the prototype at DC, at the corner and at Nyquist.
       * Near Nyquist the matched poles can no longer meet those three points without lifting the
       * passband (+0.54 dB at 0.8·Nyquist, +19 dB at the 0.999 hold, omx-dsp#105), so across the
       * hand-over band (omx_eq_lowpass_handover) the poles slide toward the prewarped bilinear
       * pair along a smoothstep of the corner: a sweeping corner
       * moves the response continuously. The numerator is fitted to the same three points
       * throughout, so DC stays 1 and the corner stays at the prototype's |H| = Q. A convex mix
       * of two stable pole pairs is stable (the stability triangle is convex). Below the
       * hand-over the section is the matched design, bit for bit.
       * Above Q = 1/√2 the prototype peaks, and near Nyquist the Nyquist value cannot be met as
       * well: the fit put both zeros on the unit circle inside the passband (72 dB off at Q 3,
       * 19.7 kHz, 44.1 kHz, omx-dsp#116). So the Nyquist target is handed over too, along the
       * same smoothstep, weighted by omx_eq_lowpass_nyquist_handover: at the band's end and from
       * Q = 0.9 up the target is 0, and with the bilinear poles that fit is the cookbook section
       * itself.
       * Below Q = 1/√2 the prototype's own Nyquist value leaves the passband short of the
       * prototype under the sliding poles, further from it than the cookbook section (0.15 dB at
       * 19166 Hz, Q 0.465, 44.1 kHz, omx-dsp#119). So the target is raised, weighted by
       * omx_eq_lowpass_nyquist_raise, along a smoothstep over the band's second half: doubled
       * at the band's end from Q = 0.5412 down, the response's highest point unmoved. Below the
       * band's middle the matched design is still flat and the target is the prototype's, so the
       * low band stays the matched design's; Q = 1/√2 keeps the prototype's value throughout. */
      omx_eq_matched_pair(w0, 1.0 / (2.0 * qq), p);
      double band[2];
      omx_eq_lowpass_handover(qq, band);
      const double r = f0 / nyquist;
      const double x = (r - band[0]) / (band[1] - band[0]);
      const double t = x <= 0.0 ? 0.0 : (x >= 1.0 ? 1.0 : x * x * (3.0 - 2.0 * x));
      const double h = 2.0 * x - 1.0;
      const double v = h <= 0.0 ? 0.0 : (h >= 1.0 ? 1.0 : h * h * (3.0 - 2.0 * h));
      const double cw = cos(w0);
      double a1 = p[0], a2 = p[1], U = p[2] * p[2];
      if (t > 0.0) {
        const double alpha = sin(w0) / (2.0 * qq);
        const double a0 = 1.0 + alpha;
        a1 = p[0] + (-2.0 * cw / a0 - p[0]) * t;
        a2 = p[1] + ((1.0 - alpha) / a0 - p[1]) * t;
        U = (1.0 + a1 + a2) * (1.0 + a1 + a2);
      }
      const double xn = nyquist / f0;
      const double un = 1.0 - xn * xn;
      const double tPi = 1.0 / (un * un + (xn / qq) * (xn / qq));
      const double dPi = 1.0 - a1 + a2;
      const double dr = 1.0 + a1 * cw + a2 * cos(2.0 * w0);
      const double di = -(a1 * sin(w0) + a2 * sin(2.0 * w0));
      const double s0 = sin(w0 / 2.0) * sin(w0 / 2.0);
      const double keep =
          (1.0 - t * omx_eq_lowpass_nyquist_handover(qq)) * (1.0 + v * omx_eq_lowpass_nyquist_raise(qq));
      if (keep == 0.0) {
        /* Handed over completely: the bilinear poles under the Nyquist zero, the cookbook's
         * double zero at z = −1 scaled to unity at DC. The fit lands here in exact arithmetic
         * but through the square root of a discriminant that is then exactly 0, so its rounding
         * would move b0 and b2 apart by 1e-8. */
        const double g = (1.0 + a1 + a2) / 4.0;
        c[0] = g; c[1] = 2.0 * g; c[2] = g; c[3] = a1; c[4] = a2;
        break;
      }
      const OmxMatchedNumerator n =
          omx_matched_fit_numerator(U, tPi * dPi * dPi * keep, qq * qq * (dr * dr + di * di), s0);
      c[0] = n.b0; c[1] = n.b1; c[2] = n.b2; c[3] = a1; c[4] = a2;
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
    case OMX_EQ_ALLPASS2:
      omx_allpass2_design(f0, qq, sample_rate, c);
      break;
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
  OMX_POST(omx_block_finite_d(c, 5u), "finite-coeffs");
  OMX_POST(omx_biquad_stable(c), "poles-inside-the-unit-circle");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "eq-design/bandwidth-q"
/**
 * @brief The cookbook's bandwidth-to-Q relation: the Q at which a section designed at `freq_hz`
 *        has its −3 dB points `bw_oct` octaves apart (for the bandpass and the notch). The
 *        `w0/sin(w0)` term is the cookbook's own correction for the bilinear warp, so the edges
 *        land at `f·2^(±bw/2)` at every rate. This is the law's one home (FreeMixer/omx-dsp#41);
 *        core's `bandwidthOctavesQ` (eq.ts) is a copy, held to test/golden/eq_bandwidth_q.json
 *        until it is deleted.
 * @param freq_hz Centre, Hz; clamped to `[1, 0.999·Nyquist]`, as omx_eq_design() clamps it.
 * @param bw_oct The width, in octaves, positive.
 * @param sample_rate Sample rate, Hz, positive.
 * @return The quality factor, in [OMX_EQ_Q_MIN, OMX_EQ_Q_MAX].
 * @pre `finite-params-and-positive-rate`.
 * @post `finite-positive-q`.
 * @note RT-safe: `sin` and `sinh` on the RT-safe allowlist. Thread-safe: pure.
 * @twin bandwidthOctavesQ
 */
static inline double omx_eq_bandwidth_q(double freq_hz, double bw_oct, double sample_rate) {
  OMX_PRE(sample_rate > 0.0 && isfinite(freq_hz) && isfinite(bw_oct) && bw_oct > 0.0,
          "finite-params-and-positive-rate");
  const double nyquist = sample_rate * 0.5;
  const double lo = 1.0, hi = nyquist * 0.999;
  const double f0 = freq_hz < lo ? lo : (freq_hz > hi ? hi : freq_hz);
  const double w0 = (2.0 * M_PI * f0) / sample_rate;
  /* a width so narrow that sinh() reaches 0 is the highest Q, not an infinite one; one so wide that it
   * overflows is the lowest, not 0 */
  const double sh = sinh(((M_LN2 / 2.0) * bw_oct * w0) / sin(w0));
  double q = 1.0 / (2.0 * (sh > 0.5 / OMX_EQ_Q_MAX ? sh : 0.5 / OMX_EQ_Q_MAX));
  if (!(q >= OMX_EQ_Q_MIN)) q = OMX_EQ_Q_MIN;
  OMX_POST(isfinite(q) && q > 0.0, "finite-positive-q");
  return q;
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

#define OMX_CONTRACT_STAGE "eq-design/complement"
/**
 * @brief The complement `1 − H` of a normalised section, as one section over the same
 *        denominator: `{1 − b0, a1 − b1, a2 − b2, a1, a2}`, core's `complementSection`. The
 *        complement of the cookbook bandpass is the cookbook notch on the bandpass's poles, the
 *        de-esser's dry remainder.
 * @param c The section `{b0, b1, b2, a1, a2}`, `a0` folded to 1.
 * @param out The complement; may be `c` itself.
 * @pre `finite-coeffs`.
 * @post `same-denominator`.
 * @note RT-safe and thread-safe: three subtractions, pure.
 * @twin complementSection
 */
static inline void omx_eq_complement(const double c[5], double out[5]) {
  OMX_PRE(omx_block_finite_d(c, 5u), "finite-coeffs");
  const double b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
  out[0] = 1.0 - b0;
  out[1] = a1 - b1;
  out[2] = a2 - b2;
  out[3] = a1;
  out[4] = a2;
  OMX_POST(out[3] == a1 && out[4] == a2, "same-denominator");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "eq-design/octaves-q"
/**
 * @brief The analogue bandwidth-to-Q relation, `√(2^N)/(2^N − 1)` for a width of `N` octaves held
 *        at or above 0.05: core's catalog `octavesToQ`, which reads a hosted plugin's width port.
 *        Unlike omx_eq_bandwidth_q() it knows no rate, so it carries no bilinear correction.
 * @param bw_oct The width, in octaves.
 * @return The quality factor.
 * @pre `finite-width`.
 * @post `finite-positive-q`.
 * @note Not for the audio thread (`pow`); thread-safe: pure.
 * @twin octavesToQ
 */
static inline double omx_eq_octaves_q(double bw_oct) {
  OMX_PRE(isfinite(bw_oct), "finite-width");
  const double bw = bw_oct > 0.05 ? bw_oct : 0.05;
  const double t = pow(2.0, bw < 64.0 ? bw : 64.0); /* 2^bw stays finite; the Q below is held at its lowest long before */
  double q = sqrt(t) / (t - 1.0);
  if (q < OMX_EQ_Q_MIN) q = OMX_EQ_Q_MIN;
  OMX_POST(isfinite(q) && q > 0.0, "finite-positive-q");
  return q;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief True when a band is exactly identity, core's `eqBandIsIdentity`: a gain-carrying kind
 *        (peaking or a shelf) at exactly 0 dB, whose zeros and poles omx_eq_design() places by the
 *        same call, so `H(z) ≡ 1` whatever its frequency or Q. Every other kind removes or nulls
 *        something at any gain.
 * @param kind The section kind.
 * @param gain_db Gain, dB.
 * @return 1 for an identity band, else 0.
 * @note RT-safe and thread-safe: two comparisons, pure.
 * @twin eqBandIsIdentity
 */
static inline int omx_eq_band_is_identity(enum omx_eq_kind kind, double gain_db) {
  return (kind == OMX_EQ_PEAKING || kind == OMX_EQ_LOWSHELF || kind == OMX_EQ_HIGHSHELF) &&
         gain_db == 0.0;
}

/** @brief One EQ band in engineering values, core's `EqBand` (a drawn `BiquadBand` with `on` set). */
struct omx_eq_band {
  enum omx_eq_kind kind; /**< The section kind. */
  double freq_hz;        /**< Centre or corner, Hz. */
  double gain_db;        /**< Gain, dB; read by the peaking and shelf kinds. */
  double q;              /**< Quality factor. */
  int on;                /**< Zero for a band switched off. */
};

/** @brief One head pass filter, core's `PassFilter`: a 12 or 24 dB/oct Butterworth corner. */
struct omx_eq_pass {
  int on;         /**< Zero when the filter is out. */
  double freq_hz; /**< The corner, Hz. */
  int slope_24;   /**< Non-zero for 24 dB/oct (two sections), zero for 12 dB/oct (one). */
};

/** @brief The most sections omx_eq_pass_design() writes: the high-pass and the low-pass at the
 *         steepest slope, two sections each (core's `passFilterMaxSections`). */
#define OMX_EQ_PASS_MAX_SECTIONS 4u

#define OMX_CONTRACT_STAGE "eq-design/bank"
/**
 * @brief An EQ's bands as the ordered bank the cascade runs, core's `eqCoeffs`: each band that is
 *        on, in order, designed by omx_eq_design() at `sample_rate`; a band that is
 *        omx_eq_band_is_identity() is in the bank at its position and parked (`enabled = 0`); a
 *        band that is off is not in the bank and the slots close up; an EQ that is off is an
 *        empty bank. The head pass filters are not here: omx_eq_pass_design().
 * @param eq_on The whole-EQ switch, `EqState.on`.
 * @param bands The bands, in order.
 * @param nbands Bands in `bands`.
 * @param sample_rate Sample rate, Hz, positive.
 * @param coeffs One `{b0, b1, b2, a1, a2}` per section written; room for `nbands`.
 * @param enabled One byte per section written: 1 runs, 0 parked; room for `nbands`.
 * @return The sections written.
 * @pre `positive-rate`.
 * @post `no-more-sections-than-bands`.
 * @note Not for the audio thread (the designs' libm calls, as the desk designs on its control
 *       thread); thread-safe on distinct output.
 * @twin eqCoeffs
 */
static inline uint32_t omx_eq_bank_design(int eq_on, const struct omx_eq_band *bands,
                                          uint32_t nbands, double sample_rate,
                                          double coeffs[][5], uint8_t *enabled) {
  OMX_PRE(sample_rate > 0.0, "positive-rate");
  uint32_t n = 0;
  if (!eq_on) return 0u;
  for (uint32_t i = 0; i < nbands; i++) {
    const struct omx_eq_band *b = &bands[i];
    if (!b->on) continue;
    omx_eq_design(b->kind, b->freq_hz, b->q, b->gain_db, sample_rate, coeffs[n]);
    enabled[n] = omx_eq_band_is_identity(b->kind, b->gain_db) ? 0u : 1u;
    n++;
  }
  OMX_POST(n <= nbands, "no-more-sections-than-bands");
  return n;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "eq-design/pass"
/**
 * @brief The head filters as the sections the strip runs at its head, core's `passFilterCoeffs`:
 *        the high-pass, then the low-pass, each only while its own `on` is set, as
 *        omx_eq_butterworth_qs() sections (always enabled: a pass filter is never identity). The
 *        whole-EQ switch does not reach them.
 * @param hpf The high-pass.
 * @param lpf The low-pass.
 * @param sample_rate Sample rate, Hz, positive.
 * @param coeffs One `{b0, b1, b2, a1, a2}` per section written.
 * @return The sections written, at most OMX_EQ_PASS_MAX_SECTIONS.
 * @pre `filters-and-positive-rate`.
 * @post `at-most-four-sections`.
 * @note Not for the audio thread (the designs' libm calls); thread-safe on distinct output.
 * @twin passFilterCoeffs
 */
static inline uint32_t omx_eq_pass_design(const struct omx_eq_pass *hpf,
                                          const struct omx_eq_pass *lpf, double sample_rate,
                                          double coeffs[OMX_EQ_PASS_MAX_SECTIONS][5]) {
  OMX_PRE(hpf && lpf && sample_rate > 0.0, "filters-and-positive-rate");
  const struct omx_eq_pass *pf[2] = {hpf, lpf};
  const enum omx_eq_kind kind[2] = {OMX_EQ_HIGHPASS, OMX_EQ_LOWPASS};
  uint32_t n = 0;
  for (int k = 0; k < 2; k++) {
    if (!pf[k]->on) continue;
    double qs[2];
    const uint32_t ns = omx_eq_butterworth_qs(pf[k]->slope_24, qs);
    for (uint32_t s = 0; s < ns; s++)
      omx_eq_design(kind[k], pf[k]->freq_hz, qs[s], 0.0, sample_rate, coeffs[n++]);
  }
  OMX_POST(n <= OMX_EQ_PASS_MAX_SECTIONS, "at-most-four-sections");
  return n;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_EQ_DESIGN_H */
