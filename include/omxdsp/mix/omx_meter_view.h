// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_meter_view.h
 * @brief The view half of the console's meters: how a measured value is drawn, the same on every surface.
 *
 * The measure half is omx_meter.h: a block's peak, the slot that holds it until read, the gain-reduction reading.
 * This is what a surface does with a value once it has it: the fill of a dB scale from the floor to 0 dBFS, the
 * peak marker that takes a louder value at once and falls by elapsed time, the clip word that stays lit until it
 * is cleared, the stepped count of an LED ladder with its hysteresis (a bar has no release, it follows the live
 * fill), the gain-reduction reading recovered over the time elapsed at the reader's tick, and what a subscriber
 * that decimates keeps of the frames it drops. The numbers are declared once in data/primitives/meter.json
 * (METER_VIEW, METER_KINDS, METER_DECIMATION) and read here from omxdsp_definitions.h.
 *
 * The marker falls by elapsed time, never per frame or per change: the same seconds give the same fall at any
 * draw rate, and a marker over silence falls although nothing new arrives. Moved from the console's meter law
 * (meterFill, meterRelease, meterStep, meterBarStep), its per-frame release replaced by the declared fall.
 *
 * Pure, header-only. Not in omxdsp.h: include <omxdsp/mix/omx_meter_view.h>.
 */
#ifndef OMX_METER_VIEW_H
#define OMX_METER_VIEW_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include <omxdsp/mix/omx_meter.h>
#include <omxdsp/omxdsp_definitions.h>

/**
 * @brief One meter's view state, owned by the surface that draws it (one per drawn meter). Zero-initialised:
 *        an empty marker, no clip.
 */
struct omx_meter_view {
  double peak;      /**< The fill the marker last took, 0..1. */
  double elapsed_s; /**< Seconds of draw ticks since it took it. */
  double live;      /**< The live fill of the last tick, 0..1. */
  double marker;    /**< The marker drawn at the last tick, 0..1. */
  bool clip;        /**< The clip word: lit until omx_meter_view_clip_clear(). */
};

/**
 * @brief A value held to the display's 0..1.
 * @param x The value; NaN reads 0.
 * @return `x` held to 0..1.
 * @note Thread-safe: pure.
 * @thread audio
 */
static inline double omx_meter_view_clamp(double x) {
  return x > 0.0 ? (x < 1.0 ? x : 1.0) : 0.0;
}

/**
 * @brief The fill of a dB level: linear in dB from `floor_db` (empty) to 0 dBFS (full).
 * @param db The level, dB (dBFS for a level meter); NaN and -inf read empty.
 * @param floor_db The floor, dB, < 0 (OMX_METER_VIEW_FLOOR_DB).
 * @return 0 at or below the floor, 1 at or above 0 dB, `(db - floor_db) / -floor_db` between.
 * @note Two compares and a division. Thread-safe: pure.
 * @thread audio
 * @twin meterFillDb
 */
static inline double omx_meter_view_fill_db(double db, double floor_db) {
  if (!(db > floor_db)) return 0.0;
  if (db >= 0.0) return 1.0;
  return (db - floor_db) / -floor_db;
}

/**
 * @brief The fill of a linear peak (1 is 0 dBFS), on the dB scale of omx_meter_view_fill_db().
 * @param level The peak, linear; at or below 0 (and NaN) reads empty, at or above 1 full.
 * @param floor_db The floor, dB, < 0 (OMX_METER_VIEW_FLOOR_DB).
 * @return omx_meter_view_fill_db(20·log10(level), floor_db).
 * @note One log10. Thread-safe: pure.
 * @thread audio
 * @twin meterFill
 */
static inline double omx_meter_view_fill(double level, double floor_db) {
  if (!(level > 0.0)) return 0.0;
  if (level >= 1.0) return 1.0;
  return omx_meter_view_fill_db(20.0 * log10(level), floor_db);
}

/**
 * @brief One draw tick of the peak marker: a fill at or above the marker is taken at once; below it the marker
 *        falls `fall_db_per_s` for each second elapsed since it took its value, never under the live fill.
 *
 * The fall is reckoned from the value the marker took and the time elapsed since, so the same seconds give the
 * same marker at any draw rate, and a tick that brings no new value (silence) still moves it.
 * @param v The view's state, read and written.
 * @param fill The live fill this tick, 0..1 (held to it).
 * @param dt_s Seconds since the last tick; a negative or NaN one counts as 0.
 * @param fall_db_per_s The fall, dB per second (OMX_METER_VIEW_FALL_DB_PER_SECOND).
 * @param floor_db The scale's floor, dB, < 0 (OMX_METER_VIEW_FLOOR_DB): the fall in fill is `fall / -floor`.
 * @return The marker to draw, 0..1.
 * @note A few flops. Thread-safe on disjoint states.
 * @thread audio
 * @twin meterMarker
 */
static inline double omx_meter_view_step(struct omx_meter_view *v, double fill, double dt_s, double fall_db_per_s,
                                         double floor_db) {
  const double f = omx_meter_view_clamp(fill);
  v->elapsed_s += dt_s > 0.0 ? dt_s : 0.0;
  double m = v->peak - fall_db_per_s * v->elapsed_s / -floor_db;
  if (!(m > f)) {
    m = f;
    v->peak = f;
    v->elapsed_s = 0.0;
  }
  v->live = f;
  v->marker = m;
  return m;
}

/**
 * @brief The clip word for one tick: lit by a clip, and it stays lit until omx_meter_view_clip_clear().
 * @param v The view's state, read and written.
 * @param clip Whether the measure half saw a clip since the last tick (a sustained run, omx_meter_clip_run).
 * @return The clip word to draw.
 * @note Thread-safe on disjoint states.
 * @thread audio
 * @twin meterClip
 */
static inline bool omx_meter_view_clip(struct omx_meter_view *v, bool clip) {
  v->clip = v->clip || clip;
  return v->clip;
}

/**
 * @brief Clear the clip word (the operator's gesture, or a reset strip).
 * @param v The view's state, written.
 * @note Thread-safe on disjoint states.
 * @thread audio
 */
static inline void omx_meter_view_clip_clear(struct omx_meter_view *v) {
  v->clip = false;
}

/**
 * @brief A stepped display's count (an LED ladder of `steps` segments), with hysteresis against the count it
 *        showed last: it holds while the unrounded value stays within `0.5 + hysteresis` steps of it.
 * @param prev The count shown last, or a negative value for none (a fresh or reset strip): plain rounding.
 * @param fill The fill, 0..1 (held to it).
 * @param steps Segments of the ladder.
 * @param hysteresis Fraction of one step (OMX_METER_VIEW_STEP_HYSTERESIS_DOUBLE).
 * @return The count, 0..`steps`.
 * @note Thread-safe: pure.
 * @thread audio
 * @twin meterStep
 */
static inline int32_t omx_meter_view_ladder(int32_t prev, double fill, uint32_t steps, double hysteresis) {
  const double raw = omx_meter_view_clamp(fill) * (double)steps;
  if (prev >= 0 && fabs(raw - (double)prev) <= 0.5 + hysteresis) return prev;
  return (int32_t)floor(raw + 0.5);
}

/**
 * @brief A stepped BAR for one tick: a linear peak to its fill on the declared scale, then its count against the
 *        last one. The bar follows the live fill both ways; it has no release (the fall is the marker's).
 * @param prev The count shown last, or negative for none.
 * @param level The peak, linear (1 is 0 dBFS).
 * @param steps Segments of the ladder.
 * @return omx_meter_view_ladder(prev, omx_meter_view_fill(level, OMX_METER_VIEW_FLOOR_DB), steps,
 *         OMX_METER_VIEW_STEP_HYSTERESIS_DOUBLE).
 * @note Thread-safe: pure.
 * @thread audio
 * @twin meterBarStep
 */
static inline int32_t omx_meter_view_bar_step(int32_t prev, double level, uint32_t steps) {
  return omx_meter_view_ladder(prev, omx_meter_view_fill(level, OMX_METER_VIEW_FLOOR_DB), steps,
                               OMX_METER_VIEW_STEP_HYSTERESIS_DOUBLE);
}

/**
 * @brief The gain-reduction reading at the reader's tick: omx_meter_gr_step() over the frames the elapsed time
 *        covers, so the hold (OMX_METER_GR_HOLD_MS) and the recovery are reckoned in time, not in ticks.
 * @param m The reading's state, read and written.
 * @param deepest_db The deepest reduction held since the last tick, dB, <= 0 (omx_meter_view_decimate()).
 * @param dt_s Seconds since the last tick; a negative or NaN one counts as 0.
 * @param sr The rate the reduction was measured at, Hz, > 0.
 * @param release_pole The stage's release pole per frame at `sr`.
 * @return The reading to draw, dB, <= 0.
 * @note Omx_meter_gr_step(). Thread-safe on disjoint states.
 * @thread audio
 * @twin meterGrView
 */
static inline float omx_meter_view_gr(struct omx_meter_gr *m, float deepest_db, double dt_s, float sr,
                                      float release_pole) {
  const double frames = dt_s > 0.0 ? floor(dt_s * (double)sr + 0.5) : 0.0;
  return omx_meter_gr_step(m, deepest_db, (uint32_t)frames, sr, release_pole);
}

/**
 * @brief What a decimating subscriber keeps of two frames of one meter (METER_DECIMATION): a level the peak, a
 *        gain reduction the deepest. A NaN frame is never kept.
 * @param kind The meter's kind (enum omx_meter_kinds).
 * @param kept What the window keeps so far.
 * @param value The next frame.
 * @return The larger of the two for a level, the smaller for a gain reduction.
 * @note Thread-safe: pure.
 * @thread audio
 * @twin meterDecimate
 */
static inline double omx_meter_view_decimate(enum omx_meter_kinds kind, double kept, double value) {
  const enum omx_meter_decimation keep =
      kind == OMX_METER_KINDS_GAIN_REDUCTION ? OMX_METER_DECIMATION_DEEPEST : OMX_METER_DECIMATION_PEAK;
  if (keep == OMX_METER_DECIMATION_DEEPEST) return value < kept ? value : kept;
  return value > kept ? value : kept;
}

/**
 * @brief The unit a meter's values are in (METER_UNITS), from its kind and, for a level, its measurement
 *        (METER_MEASUREMENTS): a sample peak dBFS, a true peak dBTP, a loudness LUFS, a loudness range LU, a gain
 *        reduction dB.
 * @param kind The meter's kind (enum omx_meter_kinds).
 * @param measurement A level's measurement (enum omx_meter_measurements); a gain reduction ignores it.
 * @return The unit, or OMX_METER_UNITS_COUNT for a kind or measurement past its set.
 * @note Thread-safe: pure.
 * @thread audio
 * @twin meterUnitOf
 */
static inline enum omx_meter_units omx_meter_view_unit_of(enum omx_meter_kinds kind,
                                                          enum omx_meter_measurements measurement) {
  if (kind == OMX_METER_KINDS_GAIN_REDUCTION) return OMX_METER_UNITS_DB;
  if (kind != OMX_METER_KINDS_LEVEL) return (enum omx_meter_units)OMX_METER_UNITS_COUNT;
  switch (measurement) {
  case OMX_METER_MEASUREMENTS_PEAK: return OMX_METER_UNITS_DBFS;
  case OMX_METER_MEASUREMENTS_TRUE_PEAK: return OMX_METER_UNITS_DBTP;
  case OMX_METER_MEASUREMENTS_LUFS_M:
  case OMX_METER_MEASUREMENTS_LUFS_S:
  case OMX_METER_MEASUREMENTS_LUFS_I: return OMX_METER_UNITS_LUFS;
  case OMX_METER_MEASUREMENTS_LRA: return OMX_METER_UNITS_LU;
  }
  return (enum omx_meter_units)OMX_METER_UNITS_COUNT;
}

#endif /* OMX_METER_VIEW_H */
