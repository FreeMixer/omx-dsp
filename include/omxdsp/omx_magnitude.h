// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_magnitude.h
 * @brief The magnitude of a designed section or cascade, `|H(e^jω)|` in dB at the frequencies a
 *        caller passes: the curve a surface draws, from the coefficients the cascade runs.
 *
 * Term for term core's `sectionMagnitudeDb` and `eqCurveDb` (eq.ts), evaluated in double: the
 * unit-circle arithmetic directly, no approximation; a section's dB floored at the caller's floor,
 * so a full null (a notch sampled on its centre) stays finite; a cascade's dB the sum of its live
 * sections'. Held to today's TypeScript by golden vectors (test/kernels/magnitude.c). Not on the
 * audio path; a control thread or the server calls it. Design: openmixer
 * docs/design/specs/2026-10-13-dsp-results-from-c.md §4.2.
 */
#ifndef OMX_MAGNITUDE_H
#define OMX_MAGNITUDE_H

#include <math.h>
#include <stdint.h>

#include "omx_biquad.h"
#include "omx_contract.h"

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "magnitude/section"
/**
 * @brief The magnitude of one normalised section `{b0, b1, b2, a1, a2}` at the angular frequency
 *        `w`, in dB, with no clamp: the unit-circle arithmetic every evaluator here shares.
 *
 * A zero denominator is not a filter and answers 0 dB; a zero numerator is a full null and answers
 * `floor_db`.
 * @param c The coefficients, in double, `a0` folded to 1.
 * @param w The angular frequency, radians per sample.
 * @param floor_db The lowest value answered, dB.
 * @return `max(floor_db, 20·log10|H(e^jw)|)`.
 * @pre `finite-args`.
 * @note RT-safe: `cos`, `sin`, `hypot`, `log10`, no allocation. Thread-safe: pure.
 */
static inline double omx_biquad_magnitude_db_w(const double c[5], double w, double floor_db) {
  OMX_PRE(isfinite(w) && isfinite(floor_db), "finite-args");
  const double cw = cos(w), sw = sin(w), cw2 = cos(2.0 * w), sw2 = sin(2.0 * w);
  /* H(e^jw) with z^-1 = cos w - j sin w, z^-2 = cos 2w - j sin 2w. */
  const double num_re = c[0] + c[1] * cw + c[2] * cw2;
  const double num_im = -(c[1] * sw + c[2] * sw2);
  const double den_re = 1.0 + c[3] * cw + c[4] * cw2;
  const double den_im = -(c[3] * sw + c[4] * sw2);
  const double num = hypot(num_re, num_im);
  const double den = hypot(den_re, den_im);
  if (den == 0.0) return 0.0;
  const double r = num / den;
  const double db = r > 0.0 ? 20.0 * log10(r) : -INFINITY;
  return db < floor_db ? floor_db : db;
}

/**
 * @brief The magnitude of one normalised section `{b0, b1, b2, a1, a2}` at `freq_hz`, in dB.
 *
 * The frequency is clamped to `0.999·Nyquist`, as omx_eq_design() clamps a centre and eqCurveDb
 * clamps a point; a non-positive frequency answers 0 dB. Otherwise omx_biquad_magnitude_db_w().
 * @param c The coefficients, in double, `a0` folded to 1.
 * @param freq_hz The frequency, Hz.
 * @param sample_rate Sample rate, Hz, positive.
 * @param floor_db The lowest value answered, dB.
 * @return `max(floor_db, 20·log10|H(e^jω)|)`, `ω = 2π·f/sample_rate`.
 * @pre `finite-args-and-positive-rate`, `poles-inside-the-unit-circle`.
 * @note RT-safe: `cos`, `sin`, `hypot`, `log10`, no allocation. Thread-safe: pure.
 */
static inline double omx_biquad_magnitude_db_d(const double c[5], double freq_hz,
                                               double sample_rate, double floor_db) {
  OMX_PRE(sample_rate > 0.0 && isfinite(freq_hz) && isfinite(floor_db),
          "finite-args-and-positive-rate");
  OMX_PRE(omx_biquad_stable(c), "poles-inside-the-unit-circle");
  if (!(freq_hz > 0.0) || !(sample_rate > 0.0)) return 0.0;
  const double hi = sample_rate * 0.5 * 0.999;
  const double f = freq_hz > hi ? hi : freq_hz;
  return omx_biquad_magnitude_db_w(c, (2.0 * M_PI * f) / sample_rate, floor_db);
}

/**
 * @brief omx_biquad_magnitude_db_d() of a float section, the tuple omx_biquad() runs, widened
 *        exactly to double.
 * @param c The float coefficients `{b0, b1, b2, a1, a2}`.
 * @param freq_hz The frequency, Hz.
 * @param sample_rate Sample rate, Hz, positive.
 * @param floor_db The lowest value answered, dB.
 * @return The section's magnitude, dB, floored at `floor_db`.
 * @note RT-safe and thread-safe: five widening loads and omx_biquad_magnitude_db_d().
 */
static inline double omx_biquad_magnitude_db(const float c[5], double freq_hz, double sample_rate,
                                             double floor_db) {
  const double d[5] = {c[0], c[1], c[2], c[3], c[4]};
  return omx_biquad_magnitude_db_d(d, freq_hz, sample_rate, floor_db);
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "magnitude/cascade"
/**
 * @brief The magnitude of a cascade of double sections at each of `n` frequencies, in dB: the sum
 *        of every live section's omx_biquad_magnitude_db_d(), each floored on its own, as
 *        eqCurveDb sums its bands. A parked section (`enabled[b] == 0`) is skipped; no section
 *        answers 0 dB everywhere.
 * @param coeffs One double `{b0, b1, b2, a1, a2}` set per section.
 * @param enabled One byte per section, or NULL for all on.
 * @param nbands Sections in `coeffs`.
 * @param freq_hz The frequencies, Hz, in any order.
 * @param n Frequencies in `freq_hz` and values written to `db_out`.
 * @param sample_rate Sample rate, Hz, positive.
 * @param floor_db The floor each section's value is held at, dB.
 * @param db_out The magnitude at each frequency, dB.
 * @pre `finite-args-and-positive-rate`.
 * @post `finite-out`.
 * @note RT-safe: O(nbands·n), no allocation. Thread-safe on distinct output; reentrant.
 */
static inline void omx_biquad_cascade_magnitude_db_d(const double coeffs[][5],
                                                     const uint8_t *enabled, uint32_t nbands,
                                                     const double *freq_hz, uint32_t n,
                                                     double sample_rate, double floor_db,
                                                     double *db_out) {
  OMX_PRE(sample_rate > 0.0 && isfinite(floor_db), "finite-args-and-positive-rate");
  for (uint32_t i = 0; i < n; i++) {
    double db = 0.0;
    for (uint32_t b = 0; b < nbands; b++)
      if (!enabled || enabled[b])
        db += omx_biquad_magnitude_db_d(coeffs[b], freq_hz[i], sample_rate, floor_db);
    db_out[i] = db;
  }
  OMX_POST(omx_block_finite_d(db_out, n), "finite-out");
}

/**
 * @brief omx_biquad_cascade_magnitude_db_d() over the float bank the cascade runs
 *        (omx_biquad_cascade()'s `coeffs` and `enabled`), each section widened exactly to double.
 * @param coeffs One float `{b0, b1, b2, a1, a2}` set per section.
 * @param enabled One byte per section, or NULL for all on.
 * @param nbands Sections in `coeffs`.
 * @param freq_hz The frequencies, Hz, in any order.
 * @param n Frequencies in `freq_hz` and values written to `db_out`.
 * @param sample_rate Sample rate, Hz, positive.
 * @param floor_db The floor each section's value is held at, dB.
 * @param db_out The magnitude at each frequency, dB.
 * @pre `finite-args-and-positive-rate`.
 * @post `finite-out`.
 * @note RT-safe: O(nbands·n), no allocation. Thread-safe on distinct output; reentrant.
 */
static inline void omx_biquad_cascade_magnitude_db(const float coeffs[][5], const uint8_t *enabled,
                                                   uint32_t nbands, const double *freq_hz,
                                                   uint32_t n, double sample_rate, double floor_db,
                                                   double *db_out) {
  OMX_PRE(sample_rate > 0.0 && isfinite(floor_db), "finite-args-and-positive-rate");
  for (uint32_t i = 0; i < n; i++) {
    double db = 0.0;
    for (uint32_t b = 0; b < nbands; b++)
      if (!enabled || enabled[b])
        db += omx_biquad_magnitude_db(coeffs[b], freq_hz[i], sample_rate, floor_db);
    db_out[i] = db;
  }
  OMX_POST(omx_block_finite_d(db_out, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The three frequencies a band anchors a drawn curve at, as eqCurveDb adds them: its centre
 *        and its half-bandwidth edges `f ∓ f/(2·max(0.1, q))`, so a narrow feature is sampled at
 *        its extremum whatever the grid's density.
 * @param freq_hz The band's centre or corner, Hz.
 * @param q The band's quality factor.
 * @param out `{lower edge, centre, upper edge}`, Hz.
 * @note RT-safe and thread-safe: three arithmetic stores, pure.
 */
static inline void omx_eq_band_anchors(double freq_hz, double q, double out[3]) {
  const double half = freq_hz / (2.0 * (q > 0.1 ? q : 0.1));
  out[0] = freq_hz - half;
  out[1] = freq_hz;
  out[2] = freq_hz + half;
}

#endif /* OMX_MAGNITUDE_H */
