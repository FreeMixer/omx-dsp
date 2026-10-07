/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * HRP per-note harmonic baseline: what THIS instrument's harmonics normally are, at THIS pitch.
 *
 * Spec: openmixer docs/design/specs/2026-08-18-hrp-polyphonic-architecture.md §20–§23, §42 (increment 8).
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/hrp_baseline.h (omx-dsp#15, lean-engine
 * spec §1(a)). The engine keeps the spectrum rings, the control-thread tick and the strip's EQ
 * bands that apply a correction; the analysis itself lives here.
 *
 * ## Note-relative, never channel-absolute (§20)
 *
 * The model is `baseline[midi note][harmonic]`: the same instrument produces different harmonic
 * relationships at different pitches, so one absolute-frequency profile per channel would blur
 * every note it has ever played into one shape and call the blur "normal". A voice that changes
 * note reads (and learns) a different row.
 *
 * ## A robust estimator, not a mean (§22)
 *
 * Each accepted measurement moves the stored level toward itself by AT MOST `step_db`. A bounded
 * step is a poor man's median: one 20 dB outlier — a mic bump, a collision the attribution
 * missed — moves the baseline by half a decibel, where an arithmetic mean would carry it for the
 * rest of the show. The first accepted measurement seeds the row outright (there is nothing to
 * be robust against yet).
 *
 * ## Ripeness — absence is a fact
 *
 * A row answers a baseline only after `ripe_updates` accepted measurements. Before that the
 * honest answer is NO ANSWER, and the deviation tier does nothing (§54: a missed correction
 * beats a wrong cut). Ripeness is per (note, harmonic): a note the instrument has never held
 * long enough has no baseline, however long its neighbours have.
 *
 * ## What is deliberately NOT here
 *
 * The acceptance GATES (§21 — stable voice, unambiguous present partial) are the caller's:
 * they are facts about the tracker and the attribution, and this module must stay testable by
 * feeding it numbers. Allocation-free, no globals, caller owns the memory. Never called from
 * the RT callback.
 */
#ifndef OMX_HRP_BASELINE_H
#define OMX_HRP_BASELINE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_attribute.h>

/** MIDI note range the model spans — the full byte, so no playable note falls outside it. */
#define OMX_HRP_BASELINE_NOTES 128

/** One channel's learned model: a level and an update count per (note, harmonic). */
typedef struct {
  float base_db[OMX_HRP_BASELINE_NOTES][OMX_HRP_HARMONICS];
  uint16_t updates[OMX_HRP_BASELINE_NOTES][OMX_HRP_HARMONICS];
} OmxHrpBaseline;

typedef struct {
  /** The most one accepted measurement may move a learned level, in dB. */
  float step_db;
  /** Accepted measurements before a (note, harmonic) row answers at all. */
  uint16_t ripe_updates;
} OmxHrpBaselineConfig;

/** step 0.5 dB / ripe 24: at the ~100 ms analysis tick, a note must hold (stable, confident,
 *  unambiguous) for ~2.4 s in total before its baseline speaks — soundcheck listening, not a
 *  snap judgement — and after that a full outlier costs half a decibel. */
static inline OmxHrpBaselineConfig omx_hrp_baseline_config_default(void) {
  OmxHrpBaselineConfig c;
  c.step_db = 0.5f;
  c.ripe_updates = 24;
  return c;
}

/** Empty model: nothing learned, every row unripe. Also the §51 reset. */
static inline void omx_hrp_baseline_init(OmxHrpBaseline *b) { memset(b, 0, sizeof(*b)); }

/** How many (note, harmonic) rows currently answer — the "what has it learned" readout. */
static inline uint32_t omx_hrp_baseline_ripe_rows(const OmxHrpBaseline *b,
                                                  const OmxHrpBaselineConfig *cfg) {
  uint32_t n = 0;
  for (int note = 0; note < OMX_HRP_BASELINE_NOTES; note++)
    for (uint32_t k = 0; k < OMX_HRP_HARMONICS; k++)
      if (b->updates[note][k] >= cfg->ripe_updates) n++;
  return n;
}

/**
 * Accept one gated measurement of harmonic `harmonic` (1-based; harmonic 1 IS the fundamental)
 * of the note `midi_note`, at `level_db`. The caller has already applied §21's gates; a note or
 * harmonic outside the model is ignored rather than clamped — clamping would file a piccolo's
 * overflow under the highest note the model holds, which is a different note.
 */
static inline void omx_hrp_baseline_learn(OmxHrpBaseline *b, int midi_note, uint32_t harmonic,
                                          float level_db, const OmxHrpBaselineConfig *cfg) {
  if (midi_note < 0 || midi_note >= OMX_HRP_BASELINE_NOTES) return;
  if (harmonic < 1 || harmonic > OMX_HRP_HARMONICS) return;
  const uint32_t k = harmonic - 1;
  uint16_t *n = &b->updates[midi_note][k];
  float *base = &b->base_db[midi_note][k];
  if (*n == 0) {
    *base = level_db;
  } else {
    float step = level_db - *base;
    if (step > cfg->step_db) step = cfg->step_db;
    if (step < -cfg->step_db) step = -cfg->step_db;
    *base += step;
  }
  if (*n < UINT16_MAX) (*n)++;
}

/**
 * The learned level for (note, harmonic), when it is RIPE. Returns 1 and fills `out_db`, or
 * returns 0 — no baseline, and the caller does nothing (§54).
 */
static inline int omx_hrp_baseline_read(const OmxHrpBaseline *b, int midi_note, uint32_t harmonic,
                                        const OmxHrpBaselineConfig *cfg, float *out_db) {
  if (midi_note < 0 || midi_note >= OMX_HRP_BASELINE_NOTES) return 0;
  if (harmonic < 1 || harmonic > OMX_HRP_HARMONICS) return 0;
  const uint32_t k = harmonic - 1;
  if (b->updates[midi_note][k] < cfg->ripe_updates) return 0;
  *out_db = b->base_db[midi_note][k];
  return 1;
}

#endif /* OMX_HRP_BASELINE_H */
