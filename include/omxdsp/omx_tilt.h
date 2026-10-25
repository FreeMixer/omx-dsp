// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_tilt.h
 * @brief The tilt: a first-order low shelf and its complementary high shelf about one pivot, one
 *        section, unity at the pivot, `1/a` at DC and `a` at Nyquist.
 *
 * The analogue prototype is `H(s) = (a·s/w₀ + 1)/(s/w₀ + a)`: the low shelf `1/a` below the pivot
 * and the high shelf `a` above it, multiplied, are this one first-order section, with
 * `|H(j w₀)| = 1`. It is taken to the rate by the bilinear transform prewarped at the pivot, so with
 * `Ω = tan(π f/fs) / tan(π f₀/fs)` the section's magnitude is exactly the prototype's,
 *
 *     |H(f)|² = (a²Ω² + 1) / (Ω² + a²),
 *
 * unity at the pivot at every rate, `1/a` at DC and `a` at Nyquist. Its slope at the pivot is
 * `20·log10(2)·(a² − 1)/(a² + 1)` dB per octave, so a slope `s` in dB per octave is
 * `a² = (1 + σ)/(1 − σ)`, `σ = s / (20·log10 2)`: a first-order section's slope never reaches its
 * asymptote, 6.02 dB per octave, and the knob that names a slope is held inside it. A knob that names
 * a SPAN `t` in dB, `−t/2` at DC and `+t/2` at Nyquist, is `a = 10^(t/40)` (omx_tilt_a_of_span), the
 * tone face's reading; the key tilts read a slope.
 *
 * Contract: no allocation, no lock, no libc beyond <math.h>. The design runs on the control thread;
 * the section runs in double (omx_biquad_d()), the key path's precision.
 */
#ifndef OMX_TILT_H
#define OMX_TILT_H

#include <math.h>

#include "omx_biquad.h"
#include "omx_contract.h"

/** @brief A first-order section's asymptotic slope, dB per octave: `20·log10(2)`. */
#define OMX_TILT_OCTAVE_DB 6.0205999132796239

/** @brief The widest span omx_tilt_a_of_span() designs, dB: a bound that keeps `a` finite, far past
 *         any travel a face declares. */
#define OMX_TILT_SPAN_MAX_DB 120.0

/** @brief The tilt's designed section, `{b0, b1, b2, a1, a2}` with `b2 = a2 = 0`, and its `a`. */
struct omx_tilt {
  double c[5]; /**< The section, normalised, a-terms subtracted. */
  double a;    /**< Nyquist's gain, `1/a` at DC; 1 is flat. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "tilt/a-of-slope"
/**
 * @brief The `a` whose tilt has slope `db_per_oct` at its pivot.
 * @param db_per_oct The slope at the pivot, dB per octave, positive brightening; held inside
 *        ±0.9999 of OMX_TILT_OCTAVE_DB, NaN reading as 0.
 * @return `a = sqrt((1 + σ)/(1 − σ))`, `σ = db_per_oct / OMX_TILT_OCTAVE_DB`; exactly 1 at 0.
 * @post `positive-finite`.
 * @note RT-safe: one sqrt. Thread-safe: pure.
 */
static inline double omx_tilt_a_of_slope(double db_per_oct) {
  double sg = db_per_oct / OMX_TILT_OCTAVE_DB;
  sg = sg == sg ? sg : 0.0;
  sg = sg > 0.9999 ? 0.9999 : sg < -0.9999 ? -0.9999 : sg;
  const double a = sg == 0.0 ? 1.0 : sqrt((1.0 + sg) / (1.0 - sg));
  OMX_POST(a > 0.0 && a - a == 0.0, "positive-finite");
  return a;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "tilt/a-of-span"
/**
 * @brief The `a` whose tilt SPANS `span_db` from DC to Nyquist: `−span_db/2` at DC, `+span_db/2` at
 *        Nyquist, 0 dB at the pivot (the tone face's reading of a tilt knob, omx-dsp#188 R3b).
 * @param span_db The span, dB, positive brightening; held inside ±OMX_TILT_SPAN_MAX_DB, NaN reading
 *        as 0.
 * @return `a = 10^(span_db/40)`; exactly 1 at 0. A span of 6 dB is a ±3 dB pair whose slope at the
 *         pivot is `20·log10(2)·(a² − 1)/(a² + 1)`, 2.0 dB per octave.
 * @post `positive-finite`.
 * @note RT-safe: one exp. Thread-safe: pure.
 */
static inline double omx_tilt_a_of_span(double span_db) {
  double sp = span_db == span_db ? span_db : 0.0;
  sp = sp > OMX_TILT_SPAN_MAX_DB ? OMX_TILT_SPAN_MAX_DB : sp < -OMX_TILT_SPAN_MAX_DB ? -OMX_TILT_SPAN_MAX_DB : sp;
  const double a = sp == 0.0 ? 1.0 : exp(sp * (M_LN10 / 40.0));
  OMX_POST(a > 0.0 && a - a == 0.0, "positive-finite");
  return a;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "tilt/design"
/**
 * @brief Design the tilt at `pivot_hz` for the rate: the bilinear transform of the prototype,
 *        prewarped at the pivot.
 * @param t Receives the section and `a`.
 * @param a Nyquist's gain, positive; 1 designs the wire `{1, 0, 0, 0, 0}`.
 * @param pivot_hz The pivot, Hz, inside `(0, Nyquist)`.
 * @param sample_rate The rate, Hz, positive.
 * @pre `a-positive`, `pivot-below-nyquist`.
 * @post `pole-inside-the-unit-circle`.
 * @note Control thread: one tan. Thread-safe: writes only `t`.
 */
static inline void omx_tilt_design(struct omx_tilt *t, double a, double pivot_hz, double sample_rate) {
  OMX_PRE(a > 0.0, "a-positive");
  OMX_PRE(pivot_hz > 0.0 && pivot_hz < 0.5 * sample_rate, "pivot-below-nyquist");
  const double w = tan(M_PI * pivot_hz / sample_rate);
  const double n = 1.0 + a * w;
  t->a = a;
  t->c[0] = (a + w) / n;
  t->c[1] = (w - a) / n;
  t->c[2] = 0.0;
  t->c[3] = (a * w - 1.0) / n;
  t->c[4] = 0.0;
  if (a == 1.0) {
    t->c[0] = 1.0;
    t->c[1] = 0.0;
    t->c[3] = 0.0;
  }
  OMX_POST(fabs(t->c[3]) < 1.0, "pole-inside-the-unit-circle");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The tilt's magnitude at `freq_hz`, the closed form `sqrt((a²Ω² + 1)/(Ω² + a²))`.
 * @param a Nyquist's gain.
 * @param pivot_hz The pivot, Hz.
 * @param freq_hz The frequency, Hz, below Nyquist.
 * @param sample_rate The rate, Hz.
 * @return The linear magnitude the designed section has there.
 * @note RT-safe: two tan and one sqrt. Thread-safe: pure.
 */
static inline double omx_tilt_magnitude(double a, double pivot_hz, double freq_hz, double sample_rate) {
  const double o = tan(M_PI * freq_hz / sample_rate) / tan(M_PI * pivot_hz / sample_rate);
  return sqrt((a * a * o * o + 1.0) / (o * o + a * a));
}

/**
 * @brief One sample through the tilt.
 * @param t The designed tilt.
 * @param x The sample.
 * @param s The section's history, `{x₁, x₂, y₁, y₂}`, updated in place.
 * @return The tilted sample.
 * @note RT-safe: omx_biquad_d(). Thread-safe on distinct history.
 */
static inline float omx_tilt_tick(const struct omx_tilt *t, float x, double s[4]) { return omx_biquad_d(x, t->c, s); }

#endif /* OMX_TILT_H */
