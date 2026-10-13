/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_room.h — ROOM ANALYSIS: the transfer curve, the impulse response and its early reflections,
 * RT60 per octave band, the room-mode finder and the narrow cut a confirmed mode proposes. Moved
 * from openmixer packages/core/src/room-transfer.ts, room-impulse.ts, room-rt60.ts, room-modes.ts
 * and room-mode-eq.ts (FreeMixer/openmixer#1602; the analysis is openmixer
 * docs/design/specs/2026-09-08-room-analysis.md §3, the transport
 * docs/design/specs/2026-10-13-dsp-results-from-c.md §6.2).
 *
 *   transfer   the H1 estimate `Sxy/Pyy` and the coherence `|Sxy|²/(Pxx·Pyy)` per bin of the band,
 *              from the cross-spectrum accumulator of omx_align_fit.h (one accumulation, several
 *              readers), or from one deconvolved impulse response; rendered as dB, unwrapped
 *              degrees and coherence;
 *   impulse    the band's bins under a Hann taper, inverse-transformed: the analytic envelope, its
 *              global peak the direct arrival, and greedy non-maximum suppression over the local
 *              maxima above a floor inside the early-reflection horizon, one taper-widened lobe
 *              apart;
 *   rt60       a three-section cookbook bandpass at each octave centre (omx_eq_design's
 *              OMX_EQ_BANDPASS, Q = 1/(√2 − 1/√2)), the Schroeder backward integral in dB, and a
 *              least-squares slope over the T20 span, extrapolated to 60 dB;
 *   modes      local maxima of a position's magnitude a set height above the band's median, with a
 *              half-power Q, confirmed when every position shows one within a bin;
 *   mode-eq    the cut a confirmed mode proposes, bounded by the notch depth law and the EQ's gain
 *              and Q travel (omx-contract's OMX_FBS_MAX_NOTCH_DEPTH_DB, OMX_EQ_GAIN_RANGE_*,
 *              OMX_EQ_Q_RANGE_*), and the residual a re-sweep reads.
 *
 * Every other number core declares for these readers (the bands, the floors, the horizon, the T20
 * span, the median margin, the position floor, the frame floor) is the caller's to pass:
 * omx-contract does not declare them yet and this tree types none of them.
 * Control thread only, never the audio callback: no allocation (every buffer and scratch is the
 * caller's, sized as each function states), no lock, no global state, no I/O.
 */
#ifndef OMX_ROOM_H
#define OMX_ROOM_H

#include <stdint.h>

#include <omxdsp/analysis/omx_align_fit.h>

/** A per-bin transfer estimate over the band `[k_lo, k_hi]` (core's `RoomTransferAccum`): bins
 * outside the band read zero. The arrays are the caller's, `fft_size / 2` entries each. */
typedef struct {
  double *re;          /**< Re H per bin */
  double *im;          /**< Im H per bin */
  double *coherence;   /**< coherence per bin, 0..1 */
  uint32_t k_lo;       /**< the band's first bin */
  uint32_t k_hi;       /**< the band's last bin */
  double sample_rate;  /**< Hz */
  uint32_t fft_size;   /**< the analysis length */
  uint32_t frames;     /**< frames folded (1 for an impulse response) */
} OmxRoomAccum;

/** One early reflection (core's `RoomReflection`). */
typedef struct {
  uint32_t samples;    /**< Arrival after the direct peak, samples */
  double arrival_ms;   /**< Arrival after the direct peak, ms */
  double distance_m;   /**< The extra path past the direct sound, `arrival_ms × 0.343` */
  double level_db;     /**< Level re the direct peak, ≤ 0 */
} OmxRoomReflection;

/** One candidate peak in one position's curve (core's `RoomModePeak`). */
typedef struct {
  double hz;              /**< The peak bin's frequency */
  double q;               /**< From the half-power width, at most `hz / (2·binHz)` */
  double above_median_db; /**< dB above the band's median */
} OmxRoomModePeak;

/** One position's candidate peaks, as {@link omx_room_mode_peaks} left them. */
typedef struct {
  uint32_t code;          /**< OMX_ANALYSIS_OK or the position's refusal */
  double sample_rate;     /**< Hz */
  uint32_t fft_size;      /**< the analysis length */
  double bin_hz;          /**< sample_rate / fft_size */
  const OmxRoomModePeak *peaks; /**< ascending frequency */
  uint32_t n;             /**< how many */
} OmxRoomModePeaks;

/** A mode confirmed at every position (core's `RoomMode`): the means of the matching peaks. */
typedef struct {
  double hz;              /**< Mean frequency */
  uint32_t positions;     /**< Positions that confirmed it */
  double q;               /**< Mean Q */
  double above_median_db; /**< Mean height above the median */
} OmxRoomMode;

/** A proposed narrow cut (core's `RoomModeEqProposal`, less the residual a re-sweep attaches). */
typedef struct {
  double hz;            /**< Where the cut sits */
  double gain_db;       /**< The cut, negative */
  double q;             /**< Inside the EQ's Q travel */
  uint32_t positions;   /**< Positions behind the mode */
  uint32_t bounded;     /**< 1 when the depth or the width hit a ceiling */
  double requested_db;  /**< The cut the room asked for */
} OmxRoomModeEqProposal;

/**
 * @brief The H1 transfer and coherence of a folded run over a band (core's
 *        `roomTransferAccumulate`).
 * @param a The accumulator (omx_align_fit.h); its reference offset is not read.
 * @param min_frames The fewest frames a coherence may be computed from (core's `MIN_FRAMES`).
 * @param low_hz The band's lower edge.
 * @param high_hz The band's upper edge.
 * @param out The estimate; its `re`, `im` and `coherence` arrays are the caller's, `fft_size / 2`
 *        entries each, and are written in full.
 * @return `OMX_ANALYSIS_OK`, or too-few-frames, shape-mismatch, empty-band, no-shared-energy.
 * @note Allocation-free, O(n). Thread-safe on distinct buffers.
 */
enum omx_analysis_refusal omx_room_transfer(const OmxXspec *a, uint32_t min_frames, double low_hz, double high_hz,
                                            OmxRoomAccum *out);

/**
 * @brief The same estimate from one impulse response (core's `accumFromImpulseResponse`):
 *        zero-padded to the next power of two (at least 2), forward-transformed, the band kept,
 *        coherence 1 inside it.
 * @param ir The impulse response.
 * @param n Its length, ≥ 1.
 * @param sample_rate Hz.
 * @param low_hz The band's lower edge.
 * @param high_hz The band's upper edge.
 * @param scratch_re `omx_room_ir_fft_size(n)` doubles of scratch.
 * @param scratch_im As many.
 * @param out The estimate; its arrays hold `omx_room_ir_fft_size(n) / 2` entries each.
 * @return `OMX_ANALYSIS_OK`, empty-band (frames 0) or no-shared-energy (frames 1).
 * @note Allocation-free, O(n log n). Thread-safe on distinct buffers.
 */
enum omx_analysis_refusal omx_room_transfer_from_ir(const double *ir, uint32_t n, double sample_rate, double low_hz,
                                                    double high_hz, double *scratch_re, double *scratch_im,
                                                    OmxRoomAccum *out);

/**
 * @brief The transform length {@link omx_room_transfer_from_ir} pads an `n`-sample response to.
 * @param n The response's length.
 * @return The smallest power of two ≥ n, at least 2.
 * @note Allocation-free, O(log n). Thread-safe: pure.
 */
uint32_t omx_room_ir_fft_size(uint32_t n);

/**
 * @brief Unwrap a run of phases in place: each step folded into (−π, π] before it accumulates.
 * @param rad The phases, radians, in bin order.
 * @param n How many.
 * @note Allocation-free, O(n). Thread-safe on distinct buffers.
 */
void omx_room_unwrap(double *rad, uint32_t n);

/**
 * @brief The display curve, one entry per band bin (core's `roomTransferCurveFromAccum`).
 * @param acc The estimate.
 * @param floor_db The magnitude floor (core's `ANALYSIS_FLOOR_DB`).
 * @param hz Receives `k·rate/n`, `k_hi − k_lo + 1` entries.
 * @param magnitude_db Receives `max(floor_db, 20·log10|H|)`.
 * @param phase_deg Receives the phase, unwrapped across the band, degrees.
 * @param coherence Receives the coherence.
 * @note Allocation-free, O(n). Thread-safe on distinct buffers.
 */
void omx_room_curve(const OmxRoomAccum *acc, double floor_db, double *hz, double *magnitude_db, double *phase_deg,
                    double *coherence);

/**
 * @brief The most reflections {@link omx_room_impulse} can name for an estimate: one per
 *        taper-widened lobe across the horizon.
 * @param acc The estimate.
 * @param window_ms The early-reflection horizon, ms.
 * @return The capacity a caller's reflection array needs.
 * @note Allocation-free, O(1). Thread-safe: pure.
 */
uint32_t omx_room_impulse_capacity(const OmxRoomAccum *acc, double window_ms);

/**
 * @brief The impulse response's direct arrival and early reflections (core's
 *        `roomImpulseFromAccum`).
 * @param acc The estimate.
 * @param window_ms The horizon past the direct arrival (core's `EARLY_REFLECTION_WINDOW_MS`).
 * @param floor_db The floor a reflection clears, dB re the direct peak (core's
 *        `EARLY_REFLECTION_FLOOR_DB`).
 * @param scratch_re `fft_size` doubles of scratch; on return it holds the envelope.
 * @param scratch_im `fft_size` doubles of scratch.
 * @param direct Receives the direct arrival's sample in the window.
 * @param out The reflections, in arrival order.
 * @param cap Its capacity, at least {@link omx_room_impulse_capacity}.
 * @param n_out Receives how many.
 * @return `OMX_ANALYSIS_OK` or no-shared-energy.
 * @pre `capacity-covers-the-horizon`.
 * @note Allocation-free, O(n log n + n·r) for r reflections. Thread-safe on distinct buffers.
 */
enum omx_analysis_refusal omx_room_impulse(const OmxRoomAccum *acc, double window_ms, double floor_db,
                                           double *scratch_re, double *scratch_im, uint32_t *direct,
                                           OmxRoomReflection *out, uint32_t cap, uint32_t *n_out);

/**
 * @brief RT60 in one octave band from a captured decay (core's `roomRt60Of`, one band).
 * @param samples The captured decay.
 * @param n Its length.
 * @param sample_rate Hz.
 * @param centre_hz The band's centre.
 * @param upper_db The T20 span's upper end, dB re the decay's start (core's `T20_UPPER_DB`).
 * @param lower_db Its lower end (core's `T20_LOWER_DB`).
 * @param scratch `n` doubles of scratch: the filtered band, then its Schroeder curve in dB.
 * @param t60_ms Receives `60 / |slope|`, ms, when the return is `OMX_ANALYSIS_OK`.
 * @return `OMX_ANALYSIS_OK`, no-signal, too-short (under 20 periods of the centre) or
 *         insufficient-decay.
 * @note Allocation-free, O(n). Thread-safe on distinct buffers.
 */
enum omx_analysis_refusal omx_room_rt60_band(const double *samples, uint32_t n, double sample_rate, double centre_hz,
                                             double upper_db, double lower_db, double *scratch, double *t60_ms);

/**
 * @brief The median of `n` values (core's `median` in room-modes.ts).
 * @param values The values.
 * @param n How many, ≥ 1.
 * @param scratch `n` doubles: a sorted copy.
 * @return The middle value, or the mean of the two middle values.
 * @note Allocation-free, O(n log n). Thread-safe on distinct buffers.
 */
double omx_room_median(const double *values, uint32_t n, double *scratch);

/**
 * @brief One position's candidate peaks (core's `roomModePeaksOf`, over the estimate the mode
 *        band selected).
 * @param acc The estimate.
 * @param floor_db The magnitude floor (core's `ANALYSIS_FLOOR_DB`).
 * @param min_above_median_db How far above the median a peak must read (core's
 *        `ROOM_MODE_MIN_ABOVE_MEDIAN_DB`).
 * @param scratch `3·(k_hi − k_lo + 1)` doubles.
 * @param peaks Receives the peaks, ascending.
 * @param cap Its capacity; `(k_hi − k_lo + 1) / 2` always suffices.
 * @param out Receives the position's record, pointing at `peaks`.
 * @note Allocation-free, O(m log m) for m band bins. Thread-safe on distinct buffers.
 */
void omx_room_mode_peaks(const OmxRoomAccum *acc, double floor_db, double min_above_median_db, double *scratch,
                         OmxRoomModePeak *peaks, uint32_t cap, OmxRoomModePeaks *out);

/**
 * @brief The modes every position confirms within one bin (core's `roomModesOf`).
 * @param positions The positions' peaks.
 * @param n_positions How many positions.
 * @param min_positions The fewest positions a mode may stand on (core's
 *        `ROOM_MODE_MIN_POSITIONS`).
 * @param out Receives the modes, ascending frequency.
 * @param cap Its capacity; the first position's peak count suffices.
 * @param n_out Receives how many.
 * @return `OMX_ANALYSIS_OK`, too-few-positions, the first refusing position's refusal, or
 *         shape-mismatch when the positions' rates or lengths differ.
 * @note Allocation-free, O(p·m²). Thread-safe on distinct buffers.
 */
enum omx_analysis_refusal omx_room_modes(const OmxRoomModePeaks *positions, uint32_t n_positions,
                                         uint32_t min_positions, OmxRoomMode *out, uint32_t cap, uint32_t *n_out);

/**
 * @brief Whether one more position left the confirmed set where it was (core's
 *        `roomModeSetIsStable`): the same count, each earlier mode matched by a later one within
 *        `tolerance_hz`, each used once.
 * @param before The earlier set.
 * @param n_before Its size.
 * @param after The later set.
 * @param n_after Its size.
 * @param tolerance_hz The match tolerance.
 * @param used `n_after` bytes of scratch.
 * @return 1 when stable.
 * @note Allocation-free, O(m²). Thread-safe on distinct buffers.
 */
int omx_room_mode_set_stable(const OmxRoomMode *before, uint32_t n_before, const OmxRoomMode *after, uint32_t n_after,
                             double tolerance_hz, unsigned char *used);

/**
 * @brief The cuts the confirmed modes propose, deepest first (core's `roomModeEqProposalsOf`).
 * @param modes The confirmed modes.
 * @param n How many.
 * @param min_positions The fewest positions a cut may stand on (core's `ROOM_MODE_MIN_POSITIONS`).
 * @param out Receives the proposals: a mode under `min_positions` or at or under the median
 *        proposes none.
 * @param cap Its capacity; `n` suffices.
 * @param n_out Receives how many.
 * @note Allocation-free, O(n²). Thread-safe on distinct buffers.
 */
void omx_room_mode_eq_proposals(const OmxRoomMode *modes, uint32_t n, uint32_t min_positions,
                                OmxRoomModeEqProposal *out, uint32_t cap, uint32_t *n_out);

/**
 * @brief How far the curve bin nearest `hz` reads above the curve's median, floored at 0 (core's
 *        `roomModeResidualDb`).
 * @param hz The curve's frequencies.
 * @param magnitude_db The curve's magnitudes.
 * @param n How many bins.
 * @param target_hz The applied cut's frequency.
 * @param scratch `n` doubles.
 * @param residual_db Receives the residual.
 * @return 1 with `residual_db` set; 0 for an empty curve (core's `undefined`).
 * @note Allocation-free, O(n log n). Thread-safe on distinct buffers.
 */
int omx_room_mode_residual_db(const double *hz, const double *magnitude_db, uint32_t n, double target_hz,
                              double *scratch, double *residual_db);

#endif /* OMX_ROOM_H */
