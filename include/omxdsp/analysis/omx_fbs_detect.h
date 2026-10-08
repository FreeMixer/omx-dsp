/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * FBS feedback detection: one magnitude spectrum per push in, the confirmed rings out.
 *
 * Spec: openmixer docs/design/specs/2026-07-10-auto-feedback-corrector.md, "The detector is one C
 * engine"; rule R-101.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/fbs_detect.h (omx-dsp#15, lean-engine
 * spec §1(a)): the engine keeps the rings and the control-thread tick that feed it a spectrum, and
 * plants what it confirms in the strip's own EQ bands; the detection itself lives here.
 *
 * The gates are core's declaration (`DEFAULT_THRESHOLDS`, `thresholdsFor`), read here from the
 * generated omx_contract_limits.h or handed in per detector; this file restates none of them.
 * NEVER called from the audio callback: the detector allocates nothing, takes every buffer from
 * the caller, holds no global mutable state and does no I/O.
 */
#ifndef OMX_FBS_DETECT_H
#define OMX_FBS_DETECT_H

#include <omxcontract/omx_contract_limits.h>
#include <stdint.h>

/* Every constant this engine reads is declared once in `@freemixer/core`'s `feedback.ts` and reaches
 * it through the generated omx_contract_limits.h: the gates (OMX_DEFAULT_THRESHOLDS_*,
 * OMX_GROWTH_WINDOW_FRAMES, OMX_DEEP_PLATEAU_FRAMES, OMX_SUBHARMONIC_RADIUS_BINS), the ratio
 * neighbourhoods (OMX_FBS_PAPR_GUARD_BINS, OMX_FBS_PNPR_RADIUS_BINS, OMX_FBS_PNPR_GUARD_BINS,
 * OMX_FBS_PHPR_HARMONICS, OMX_FBS_PSHR_SELF_SKIP_BINS), the dB floor (OMX_FBS_FLOOR_LIN_DOUBLE),
 * the depth law and the notch Q (OMX_FBS_MAX_NOTCH_DEPTH_DB, OMX_FBS_DEFAULT_NOTCH_Q) and the
 * console's per-channel pool (OMX_FBS_TRACK_SLOTS). */

/** A detector pool: ring-out finds (`fixed`) or show finds (`live`). */
typedef enum { OMX_FBS_POOL_FIXED = 0, OMX_FBS_POOL_LIVE = 1 } OmxFbsPool;

/** Every gate a push confirms on — core's `DetectThresholds`, field for field. The three
 * opt-in routes are off while their `*_on` flag is 0. */
typedef struct {
  double papr_db;
  double pnpr_db;
  double phpr_db;
  uint32_t persist_frames;
  uint32_t growth_confirm_frames;
  uint32_t sub_clean_frames;
  double growth_min_total_rise_db;
  double growth_max_single_frame_share;
  double growth_min_db_per_frame;
  double growth_max_dip_db;
  double pshr_min_db;
  int plateau_on;
  double plateau_floor_db;
  int deep_plateau_on;
  double deep_plateau_floor_db;
  int trend_on;
  double trend_min_rise_db;
} OmxFbsThresholds;

/** One confirmed ring — core's `NotchCandidate` less its pool, which is the detector's. */
typedef struct {
  uint32_t bin;
  double freq_hz;
  double depth_db;
  double q;
  uint32_t persistence;
  double level_db;
} OmxFbsCandidate;

/** One tracked candidate bin's memory. Caller-owned; the detector lays nothing out itself. */
typedef struct {
  int32_t bin;
  uint32_t streak;
  uint32_t growing_run;
  uint32_t sub_clean_run;
  uint32_t n_hist;
  uint32_t n_deep;
  double hist[OMX_GROWTH_WINDOW_FRAMES];
  double deep[OMX_DEEP_PLATEAU_FRAMES];
  int confirmed;
  uint64_t confirm_seq;
  uint64_t seen_frame;
} OmxFbsSlot;

/** One channel's detector. Every pointer is caller-owned memory of the stated size. */
typedef struct {
  OmxFbsThresholds t;
  OmxFbsPool pool;
  double sample_rate;
  uint32_t fft_size;
  int32_t *bin_slot;  /**< nbins_cap entries: the slot tracking each bin, or −1 */
  uint32_t nbins_cap;
  OmxFbsSlot *slots;  /**< n_slots entries */
  uint32_t *free_stack; /**< n_slots entries */
  uint32_t n_slots;
  uint32_t n_free;
  uint64_t frame;
  uint64_t seq;
} OmxFbsDetector;

/** What one push concluded. Every array is caller-owned with room for the detector's n_slots. */
typedef struct {
  OmxFbsCandidate *confirmed; /**< newly confirmed on this frame, ascending bin */
  uint32_t n_confirmed;
  OmxFbsCandidate *snapshot;  /**< every confirmed bin still ringing, in confirmation order */
  uint32_t n_snapshot;
  uint32_t *active;           /**< every candidate bin this frame, ascending */
  uint32_t n_active;
  uint32_t untracked;         /**< candidate bins this frame that found no free slot */
} OmxFbsResult;

/**
 * @brief The corpus-tuned default gates, read from the generated header.
 * @return core's `DEFAULT_THRESHOLDS`: every opt-in route off.
 */
OmxFbsThresholds omx_fbs_thresholds_default(void);

/**
 * @brief Initialise a detector over caller-owned memory.
 * @param d The detector, its configuration already filled: `t`, `pool`, `sample_rate` (Hz, > 0),
 *          `fft_size` (> 0), `bin_slot` (`nbins_cap` entries, the widest frame a push may carry),
 *          `slots` and `free_stack` (`n_slots` entries each, how many bins may be tracked at once).
 *          Every other field is reset.
 */
void omx_fbs_init(OmxFbsDetector *d);

/**
 * @brief Score one spectrum frame.
 * @param d The detector.
 * @param mags Linear magnitudes, `nbins` of them.
 * @param nbins Bin count, ≤ the detector's nbins_cap.
 * @param out Receives the frame's decisions.
 * @note Allocation-free, lock-free, no I/O. Not for the audio callback.
 */
void omx_fbs_push(OmxFbsDetector *d, const double *mags, uint32_t nbins, OmxFbsResult *out);

/**
 * @brief The ramp gate over a dB history, oldest first.
 * @param levels_db The history.
 * @param n Its length.
 * @param t The gates.
 * @return 1 when the history is a monotone, distributed, linear-in-dB run-away.
 */
int omx_fbs_is_growing(const double *levels_db, uint32_t n, const OmxFbsThresholds *t);

/**
 * @brief Peak-to-subharmonic ratio at `bin`: its level minus the strongest line near bin/2 or
 *        bin/3.
 * @param mags Linear magnitudes.
 * @param nbins Their count.
 * @param bin The candidate bin, < nbins.
 * @return dB; +INFINITY when there is no line below to measure.
 */
double omx_fbs_pshr(const double *mags, uint32_t nbins, uint32_t bin);

#endif /* OMX_FBS_DETECT_H */
