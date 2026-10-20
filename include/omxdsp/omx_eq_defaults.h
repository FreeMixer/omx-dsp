// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_eq_defaults.h
 * @brief THE default rule for the bands of a fresh EQ, whatever the strip, the host or the band
 *        count: one rule, no override, for the console, omx-plugins and any other host.
 *
 * Moved unchanged from omx-contract's C render (its lib/eq-defaults.mjs was the rule, removed in
 * omx-contract 3.0.0); the items it reads are this library's own (data/kernels/eq.json).
 *
 * Types   the first band is a low shelf and the last a high shelf (each switchable to a bell by the
 *         operator); every band between is a bell. A single band is a bell.
 * Gain, Q every band is EQ_BAND_DEFAULTS.gainDb and .q (OMX_EQ_BAND_DEFAULTS_GAIN_DB, _Q).
 * Centres
 *   4 bands   the declared four (EQ_DEFAULT_CENTRES_FOUR_BAND_HZ): 100, 400, 2000, 8000 Hz,
 *             ISO 266 preferred values.
 *   N bands   N slots of equal width on a log axis across EQ_FREQ_RANGE; band i sits in the
 *             middle of slot i (lo * (hi / lo) ** ((i + 0.5) / N)) and takes the nearest preferred
 *             frequency on that axis, from the ISO 266 R10 series (ISO_THIRD_OCTAVE_CENTRES_HZ);
 *             when two bands would share a value in R10 the same is done on its R20 refinement
 *             (EQ_CENTRE_SERIES_R20_HZ). Never a repeated centre: more bands than R20 holds is no
 *             answer.
 *
 * test/eq_default_bands.c prints the rule for 1..62 bands; `make eq-default-bands` holds it equal to
 * test/golden/eq_default_bands.txt, which tools/eq-defaults-golden.mjs writes from the rule's
 * JavaScript spelling over data/. Needs -lm.
 */
#ifndef OMX_EQ_DEFAULTS_H
#define OMX_EQ_DEFAULTS_H

#include <math.h>
#include <omxdsp/omxdsp_definitions.h>

/**
 * @brief The default band centres (Hz) of an EQ with `count` bands.
 * @param count How many bands the EQ starts with.
 * @param out Out: `count` centres, ascending and distinct.
 * @return `count`, or 0 for a count that is 0 or has no distinct preferred centres (out then holds
 *         no answer).
 * @note RT-safe once (no allocation, no lock), but O(count x 61) logs: read it off the audio path.
 *       Thread-safe: pure.
 */
static inline unsigned omx_eq_default_centres(unsigned count, float *out) {
  static const double r10[] = OMX_ISO_THIRD_OCTAVE_CENTRES_HZ_INIT;
  static const double r20[] = OMX_EQ_CENTRE_SERIES_R20_HZ_INIT;
  static const double four[] = OMX_EQ_DEFAULT_CENTRES_FOUR_BAND_HZ_INIT;
  if (count == 0) return 0;
  if (count == 4) {
    for (unsigned i = 0; i < 4; i++) out[i] = (float)four[i];
    return 4;
  }
  const double lo = log((double)OMX_EQ_FREQ_RANGE_MIN);
  const double span = log((double)OMX_EQ_FREQ_RANGE_MAX / (double)OMX_EQ_FREQ_RANGE_MIN);
  for (unsigned s = 0; s < 2; s++) {
    const double *series = s ? r20 : r10;
    const unsigned n = s ? OMX_EQ_CENTRE_SERIES_R20_HZ_COUNT : OMX_ISO_THIRD_OCTAVE_CENTRES_HZ_COUNT;
    int distinct = count <= n;
    for (unsigned i = 0; distinct && i < count; i++) {
      const double t = lo + span * ((double)i + 0.5) / (double)count;
      unsigned best = 0;
      double best_d = 1e300;
      for (unsigned k = 0; k < n; k++) {
        const double d = fabs(log(series[k]) - t);
        if (d < best_d) { best_d = d; best = k; }
      }
      out[i] = (float)series[best];
      if (i > 0 && out[i] == out[i - 1]) distinct = 0;
    }
    if (distinct) return count;
  }
  return 0;
}

/**
 * @brief The default type of band `index` of an EQ with `count` bands: a low shelf first, a high
 *        shelf last, bells between (a single band is a bell).
 * @param index The band, 0-based.
 * @param count How many bands the EQ has.
 * @return An OMX_EQ_BAND_TYPES_* member.
 * @note RT-safe, thread-safe: pure.
 */
static inline int omx_eq_default_type(unsigned index, unsigned count) {
  if (count < 2) return OMX_EQ_BAND_TYPES_BELL;
  return index == 0 ? OMX_EQ_BAND_TYPES_LOW_SHELF : index == count - 1 ? OMX_EQ_BAND_TYPES_HIGH_SHELF : OMX_EQ_BAND_TYPES_BELL;
}

#endif /* OMX_EQ_DEFAULTS_H */
