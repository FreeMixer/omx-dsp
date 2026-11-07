/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_band_report.h — what a corrector's live shell reports per slot: six read-only words.
 *
 * Spec 7.7 and 7.16 (one-panel derivation): omx-fbs and omx-hrp report each slot as read-only
 * controls, read by value by any host — frequency, depth, Q, on, origin and shape. They are not
 * meters, not an atom message and not a host verb. A controller (the console's planter, Zynthian's
 * engine module) copies report slot k into the omx-eq slot it owns for k, so slot k is the same find
 * on the next tick: a merge or a release never moves another slot's words.
 *
 * Every word is a float, as a control port carries it. `origin` is a CURVE_ORIGINS index (its wire
 * word is OMX_CURVE_ORIGIN_WORDS[origin]: user, fbs-fixed, fbs-live, hrp, room-mode) and `shape` a
 * CURVE_SHAPES index (OMX_CURVE_SHAPE_WORDS). A free slot is off, origin user, shape off, and every
 * number zero.
 */
#ifndef OMX_BAND_REPORT_H
#define OMX_BAND_REPORT_H

#include <omxdsp/omxdsp_definitions.h>

/** The words one report slot carries, in this order. */
#define OMX_BAND_REPORT_WORDS 6u

/** One report slot, by value. */
typedef struct {
  float freq_hz;  /**< centre, Hz; 0 on a free slot */
  float depth_db; /**< the band's gain, dB (a cut is negative; a notch reports 0, its depth is the shape) */
  float q;        /**< the band's Q */
  float on;       /**< 1 while the slot holds a band, else 0 */
  float origin;   /**< enum omx_curve_origins */
  float shape;    /**< enum omx_curve_shapes */
} OmxBandReport;

/**
 * @brief The report of a free slot.
 * @return Off, origin user, shape off, every number zero.
 * @thread audio
 */
static inline OmxBandReport omx_band_report_free(void) {
  OmxBandReport r;
  r.freq_hz = 0.0f;
  r.depth_db = 0.0f;
  r.q = 0.0f;
  r.on = 0.0f;
  r.origin = (float)OMX_CURVE_ORIGINS_USER;
  r.shape = (float)OMX_CURVE_SHAPES_OFF;
  return r;
}

#endif /* OMX_BAND_REPORT_H */
