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
 * audio path; a control thread or the server calls it. Design: DSP results from C, §4.2.
 */
#ifndef OMX_MAGNITUDE_H
#define OMX_MAGNITUDE_H

#include <math.h>
#include <stdint.h>

#include "omx_biquad.h"
#include "omx_contract.h"
#include "omx_eq_design.h"

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

#define OMX_CONTRACT_STAGE "magnitude/band"
/**
 * @brief The magnitude of one band at `freq_hz`, in dB, core's `biquadMagnitudeDb`: the band
 *        designed by omx_eq_design() and evaluated by omx_biquad_magnitude_db_w() at the
 *        frequency itself. A frequency at or above Nyquist, or a non-positive one, answers 0 dB.
 *        `on` is not read: the caller passes the bands that run.
 * @param b The band.
 * @param freq_hz The frequency, Hz.
 * @param sample_rate Sample rate, Hz, positive.
 * @param floor_db The lowest value answered, dB.
 * @return The band's magnitude, dB, floored at `floor_db`.
 * @pre `finite-args-and-positive-rate`.
 * @note Not for the audio thread (a design per call); thread-safe: pure.
 */
static inline double omx_eq_band_magnitude_db(const struct omx_eq_band *b, double freq_hz,
                                              double sample_rate, double floor_db) {
  OMX_PRE(sample_rate > 0.0 && isfinite(freq_hz) && isfinite(floor_db),
          "finite-args-and-positive-rate");
  if (!(freq_hz > 0.0) || !(sample_rate > 0.0) || freq_hz >= sample_rate / 2.0) return 0.0;
  double c[5];
  omx_eq_design(b->kind, b->freq_hz, b->q, b->gain_db, sample_rate, c);
  return omx_biquad_magnitude_db_w(c, (2.0 * M_PI * freq_hz) / sample_rate, floor_db);
}

/**
 * @brief The summed magnitude of a band set at one frequency, in dB, the web surface's
 *        `magnitudeSumDb`: where a handle sits on the drawn curve. Each band floored on its own.
 * @param bands The bands.
 * @param nbands Bands in `bands`.
 * @param freq_hz The frequency, Hz.
 * @param sample_rate Sample rate, Hz, positive.
 * @param floor_db The floor each band's value is held at, dB.
 * @return The sum of omx_eq_band_magnitude_db() over the bands; 0 dB for none.
 * @note Not for the audio thread; thread-safe: pure.
 */
static inline double omx_eq_magnitude_sum_db(const struct omx_eq_band *bands, uint32_t nbands,
                                             double freq_hz, double sample_rate,
                                             double floor_db) {
  double db = 0.0;
  for (uint32_t i = 0; i < nbands; i++)
    db += omx_eq_band_magnitude_db(&bands[i], freq_hz, sample_rate, floor_db);
  return db;
}
#undef OMX_CONTRACT_STAGE

/** @brief The most points omx_eq_curve_freqs() writes for `points` grid points and `nbands`
 *         bands: the grid (at least two) and three anchors per band. */
#define OMX_EQ_CURVE_CAP(points, nbands) (((points) > 2u ? (points) : 2u) + 3u * (nbands))

#define OMX_CONTRACT_STAGE "magnitude/curve"
/**
 * @brief The frequencies a drawn curve is sampled at, eqCurveDb's: `max(2, points)` points
 *        log-spaced from `f_min` to `f_max` inclusive (`f_min·(f_max/f_min)^(i/(n−1))`), then each
 *        band's omx_eq_band_anchors() that lie strictly inside `(f_min, f_max)`, all ascending.
 * @param bands The bands.
 * @param nbands Bands in `bands`.
 * @param f_min The low edge, Hz, positive.
 * @param f_max The high edge, Hz, above `f_min`.
 * @param points The grid points.
 * @param f_out The frequencies; room for OMX_EQ_CURVE_CAP(points, nbands).
 * @return The frequencies written.
 * @pre `ordered-positive-edges`.
 * @post `ascending`.
 * @note Not for the audio thread (`pow`, an insertion sort); thread-safe on distinct output.
 */
static inline uint32_t omx_eq_curve_freqs(const struct omx_eq_band *bands, uint32_t nbands,
                                          double f_min, double f_max, uint32_t points,
                                          double *f_out) {
  OMX_PRE(f_min > 0.0 && f_max > f_min && isfinite(f_max), "ordered-positive-edges");
  const uint32_t g = points > 2u ? points : 2u;
  const double ratio = f_max / f_min;
  uint32_t n = 0;
  for (uint32_t i = 0; i < g; i++) f_out[n++] = f_min * pow(ratio, (double)i / (double)(g - 1u));
  for (uint32_t b = 0; b < nbands; b++) {
    double a[3];
    omx_eq_band_anchors(bands[b].freq_hz, bands[b].q, a);
    for (int j = 0; j < 3; j++)
      if (a[j] > f_min && a[j] < f_max) f_out[n++] = a[j];
  }
  for (uint32_t i = 1; i < n; i++) {
    const double v = f_out[i];
    uint32_t j = i;
    for (; j > 0 && f_out[j - 1] > v; j--) f_out[j] = f_out[j - 1];
    f_out[j] = v;
  }
  int ascending = 1;
  for (uint32_t i = 1; i < n; i++) ascending &= f_out[i - 1] <= f_out[i];
  OMX_POST(ascending, "ascending");
  (void)ascending; /* read only by the post-condition, compiled out in a release build */
  return n;
}

/**
 * @brief The summed response of a band set across a log axis, core's `eqCurveDb`: the frequencies
 *        of omx_eq_curve_freqs(), and at each the sum of every band's omx_eq_band_magnitude_db()
 *        at that frequency held at or below `0.999·Nyquist`. No band is a flat 0 dB line.
 * @param bands The bands.
 * @param nbands Bands in `bands`.
 * @param f_min The low edge, Hz, positive.
 * @param f_max The high edge, Hz, above `f_min`.
 * @param points The grid points.
 * @param sample_rate Sample rate, Hz, positive: the live graph rate.
 * @param floor_db The floor each band's value is held at, dB.
 * @param f_out The frequencies; room for OMX_EQ_CURVE_CAP(points, nbands).
 * @param db_out The summed magnitude at each frequency, dB; the same room.
 * @return The points written.
 * @pre `positive-rate`.
 * @post `finite-out`.
 * @note Not for the audio thread; thread-safe on distinct output.
 */
static inline uint32_t omx_eq_curve_db(const struct omx_eq_band *bands, uint32_t nbands,
                                       double f_min, double f_max, uint32_t points,
                                       double sample_rate, double floor_db, double *f_out,
                                       double *db_out) {
  OMX_PRE(sample_rate > 0.0, "positive-rate");
  const uint32_t n = omx_eq_curve_freqs(bands, nbands, f_min, f_max, points, f_out);
  const double hi = sample_rate / 2.0 * 0.999;
  for (uint32_t i = 0; i < n; i++)
    db_out[i] = omx_eq_magnitude_sum_db(bands, nbands, f_out[i] < hi ? f_out[i] : hi,
                                        sample_rate, floor_db);
  OMX_POST(omx_block_finite_d(db_out, n), "finite-out");
  return n;
}

/**
 * @brief The bands a console EQ draws, the web surface's `eqStateBands`: while the EQ is on, its
 *        bands that are on, in order; then the high-pass and the low-pass, each while it is on
 *        and its corner is positive, as one band per omx_eq_butterworth_qs() section at 0 dB.
 *        The head filters run whatever the EQ's switch says, so they are drawn whatever it says.
 * @param eq_on The whole-EQ switch.
 * @param bands The EQ's bands.
 * @param nbands Bands in `bands`.
 * @param hpf The high-pass, or NULL for none.
 * @param lpf The low-pass, or NULL for none.
 * @param out The bands drawn, each with `on` set; room for `nbands + OMX_EQ_PASS_MAX_SECTIONS`.
 * @return The bands written.
 * @post `no-more-than-room`.
 * @note RT-safe and thread-safe on distinct output: copies, no libm.
 */
static inline uint32_t omx_eq_curve_bands(int eq_on, const struct omx_eq_band *bands,
                                          uint32_t nbands, const struct omx_eq_pass *hpf,
                                          const struct omx_eq_pass *lpf, struct omx_eq_band *out) {
  uint32_t n = 0;
  if (eq_on)
    for (uint32_t i = 0; i < nbands; i++)
      if (bands[i].on) {
        out[n] = bands[i];
        out[n++].on = 1;
      }
  const struct omx_eq_pass *pf[2] = {hpf, lpf};
  const enum omx_eq_kind kind[2] = {OMX_EQ_HIGHPASS, OMX_EQ_LOWPASS};
  for (int k = 0; k < 2; k++) {
    if (!pf[k] || !pf[k]->on || !(pf[k]->freq_hz > 0.0)) continue;
    double qs[2];
    const uint32_t ns = omx_eq_butterworth_qs(pf[k]->slope_24, qs);
    for (uint32_t s = 0; s < ns; s++) {
      const struct omx_eq_band b = {kind[k], pf[k]->freq_hz, 0.0, qs[s], 1};
      out[n++] = b;
    }
  }
  OMX_POST(n <= nbands + OMX_EQ_PASS_MAX_SECTIONS, "no-more-than-room");
  return n;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MAGNITUDE_H */
