/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_align_fit.h — the cross-spectrum two microphones share, and the delay and polarity it
 * implies: Smart Alignment's fit, moved from OpenMixer core's TypeScript (smart-align.ts).
 *
 * A run of paired complex spectra (target and reference, both windows taken in one pass) is
 * folded into one cross-spectrum accumulator, {@link OmxXspec}: per bin `Sxy = Σ T·conj(R)`,
 * `Pxx = Σ|T|²`, `Pyy = Σ|R|²`. Two readers build on it: {@link omx_align_fit} reduces it to a
 * delay, a polarity, a coherence and a residual; the room transfer (omx_room.h) keeps it per bin.
 *
 * The fit, as core computed it:
 *   - coherence is |Sxy|²/(Pxx·Pyy) per bin, weighted by sqrt(Pxx·Pyy) across the band;
 *   - the ANCHOR is the peak of the analytic cross-correlation (the inverse transform of the
 *     band's bins alone), parabolically interpolated through the heights at the peak's lag and its
 *     two neighbours, read directly from the bins so that an exactly real cross-spectrum (an
 *     identical pair) anchors at exactly 0 ({@link omx_align_coarse_delay}); the circular
 *     correlation names the peak on `[−n/2, n/2)` for `OMX_ALIGN_ARRIVAL_EITHER` and on `[0, n)`
 *     for `OMX_ALIGN_ARRIVAL_PHYSICAL`, a fact the caller declares;
 *   - the RESIDUAL is a magnitude-weighted line through `arg H` after de-ramping every bin by the
 *     anchor's linear phase and rotating the band onto its own mean phasor; a residual over half a
 *     sample at a coherence the caller trusts refuses (the anchor found the wrong lobe);
 *   - the reference read `back` samples into its past is inside the answer: delay = anchor +
 *     residual + back;
 *   - polarity is the sign of the de-delayed band sum's real part; the residual phase is the
 *     magnitude-weighted RMS of what the delay and polarity left.
 *
 * Every limit core declares for this fit (MIN_FRAMES, DEFAULT_BAND, ALIGN_TRUST_COHERENCE) is the
 * caller's to pass: omx-contract does not declare them yet and this tree types none of them.
 * Control thread only, never the audio callback: no allocation (every buffer is the caller's), no
 * lock, no global state, no I/O, so any number of threads may run fits on their own buffers.
 */
#ifndef OMX_ALIGN_FIT_H
#define OMX_ALIGN_FIT_H

#include <stdint.h>

/** Why a fit (or a room curve over the same accumulator) has no answer; 0 when it has one. The
 * values are core's refusal words in `ALIGN_FIT_REFUSALS`, `ROOM_TRANSFER_REFUSALS`,
 * `ROOM_MODES_REFUSALS` and `ROOM_RT60_REFUSALS`, one code per word. */
enum omx_analysis_refusal {
  OMX_ANALYSIS_OK = 0,                       /**< An answer. */
  OMX_ANALYSIS_TOO_FEW_FRAMES = 1,           /**< `too-few-frames`. */
  OMX_ANALYSIS_SHAPE_MISMATCH = 2,           /**< `shape-mismatch`. */
  OMX_ANALYSIS_EMPTY_BAND = 3,               /**< `empty-band`. */
  OMX_ANALYSIS_NO_SHARED_ENERGY = 4,         /**< `no-shared-energy`. */
  OMX_ANALYSIS_RESIDUAL_EXCEEDS_HALF_SAMPLE = 5, /**< `residual-exceeds-half-sample`. */
  OMX_ANALYSIS_TOO_FEW_POSITIONS = 6,        /**< `too-few-positions`. */
  OMX_ANALYSIS_TOO_SHORT = 7,                /**< `too-short`. */
  OMX_ANALYSIS_NO_SIGNAL = 8,                /**< `no-signal`. */
  OMX_ANALYSIS_INSUFFICIENT_DECAY = 9        /**< `insufficient-decay`. */
};

/** Which of a circular correlation peak's two names is true for the pair (core's `AlignArrival`). */
enum omx_align_arrival {
  OMX_ALIGN_ARRIVAL_EITHER = 0,  /**< The target may lead or lag: the peak is named on [−n/2, n/2). */
  OMX_ALIGN_ARRIVAL_PHYSICAL = 1 /**< The target cannot precede the reference: named on [0, n). */
};

/** A cross-spectrum accumulator over caller-owned arrays. Initialise with {@link omx_xspec_init};
 * fold frames with {@link omx_xspec_add}. */
typedef struct {
  double *xr;           /**< Σ Re(T·conj R) per bin, `bins` entries */
  double *xi;           /**< Σ Im(T·conj R) per bin */
  double *pxx;          /**< Σ |T|² per bin */
  double *pyy;          /**< Σ |R|² per bin */
  uint32_t bins;        /**< fft_size / 2 */
  uint32_t fft_size;    /**< The analysis length every frame must share */
  double sample_rate;   /**< The rate every frame must share, Hz */
  double back_samples;  /**< The first frame's reference offset, samples */
  uint32_t frames;      /**< Frames offered, matched or not */
  uint32_t mismatch;    /**< Non-zero once a frame's length, rate or bin count disagreed */
  uint32_t back_mismatch; /**< Non-zero once a frame's reference offset disagreed */
} OmxXspec;

/** What a fit answers (core's `AlignFit`). */
typedef struct {
  double delay_ms;      /**< Positive: the target is late. The correction is its negation. */
  uint32_t invert;      /**< 1 when the target's polarity must flip for the two to sum. */
  double coherence;     /**< 0..1, energy-weighted across the band. */
  double residual_deg;  /**< Phase the delay and polarity did not explain, degrees RMS. */
  uint32_t frames;      /**< Frames averaged. */
} OmxAlignFit;

/** Where the analytic cross-correlation peaked (core's `CoarseDelay`). */
typedef struct {
  double samples; /**< The peak's delay, named on the branch the arrival selects. */
  double lag;     /**< The circular lag, 0 .. n: the name-free fact. */
  double peak;    /**< The envelope's height at the peak, in the cross-spectrum's units. */
} OmxCoarseDelay;

/** What APPLY would write (core's `AlignCorrection`). */
typedef struct {
  double delay_ms;  /**< The channel's new alignment delay, inside the travel. */
  uint32_t inverted; /**< The channel's new polarity inversion. */
  uint32_t clamped;  /**< 1 when the delay wanted lay outside the travel. */
} OmxAlignCorrection;

/**
 * @brief Start an accumulator over zeroed caller-owned arrays.
 * @param a The accumulator.
 * @param xr Σ Re(T·conj R), `fft_size / 2` entries, zeroed here.
 * @param xi Σ Im(T·conj R), `fft_size / 2` entries, zeroed here.
 * @param pxx Σ |T|², `fft_size / 2` entries, zeroed here.
 * @param pyy Σ |R|², `fft_size / 2` entries, zeroed here.
 * @param fft_size The analysis length every frame must share, ≥ 2.
 * @param sample_rate The rate every frame must share, Hz, > 0.
 * @param back_samples How many samples into its own past every reference window was read (0 for
 *        the ordinary reading).
 * @pre `fft-size-ge-2`, `rate-positive`, `buffers-present`.
 * @note Allocation-free, O(n). Thread-safe on distinct accumulators.
 */
void omx_xspec_init(OmxXspec *a, double *xr, double *xi, double *pxx, double *pyy, uint32_t fft_size,
                    double sample_rate, double back_samples);

/**
 * @brief Fold one paired frame into the accumulator, as core's frame loop does.
 * @param a The accumulator.
 * @param t_re Target real parts, `nbins` entries.
 * @param t_im Target imaginary parts.
 * @param r_re Reference real parts.
 * @param r_im Reference imaginary parts.
 * @param nbins How many bins the frame carries; fewer than `fft_size / 2` is a mismatch.
 * @param fft_size The frame's analysis length.
 * @param sample_rate The frame's rate, Hz.
 * @param back_samples The frame's reference offset, samples.
 * @return 1 when the frame agrees with the first frame in shape and offset. 0 otherwise: a shape
 *         disagreement (length, rate, too few bins) is not folded and every later reader refuses
 *         `shape-mismatch`; an offset disagreement alone is folded, the fit refuses it and the
 *         room curve, which reads no offset, does not.
 * @note Allocation-free, O(n). Thread-safe on distinct accumulators.
 */
int omx_xspec_add(OmxXspec *a, const double *t_re, const double *t_im, const double *r_re, const double *r_im,
                  uint32_t nbins, uint32_t fft_size, double sample_rate, double back_samples);

/**
 * @brief The band's first and last bin as core selects them: `max(1, ceil(low/binHz))` and
 *        `min(bins − 2, floor(high/binHz))`.
 * @param fft_size The analysis length.
 * @param sample_rate Hz.
 * @param low_hz The band's lower edge.
 * @param high_hz The band's upper edge.
 * @param k_lo Receives the first bin.
 * @param k_hi Receives the last bin.
 * @return 1 when the band selects at least two bins (`k_hi > k_lo`); 0 (`empty-band`) otherwise.
 * @note Allocation-free, O(1). Thread-safe: pure.
 */
int omx_xspec_band(uint32_t fft_size, double sample_rate, double low_hz, double high_hz, uint32_t *k_lo,
                   uint32_t *k_hi);

/**
 * @brief The coarse delay: where the analytic cross-correlation of the band's bins peaks.
 * @param cross_re The cross-spectrum's real parts, `fft_size / 2` entries.
 * @param cross_im Its imaginary parts.
 * @param fft_size The analysis length, a power of two.
 * @param k_lo The band's first bin (clamped at 0).
 * @param k_hi Its last bin (clamped at fft_size/2 − 1).
 * @param arrival Which name the peak gets.
 * @param scratch_re `fft_size` doubles of scratch.
 * @param scratch_im `fft_size` doubles of scratch.
 * @param out Receives the peak.
 * @return 1 with `out` filled; 0 when the band is empty, carries no energy or `fft_size` is not a
 *         power of two (core's `null`).
 * @note Allocation-free, O(n log n) for the transform that finds the peak, then one cos/sin pair
 *       per band bin for the three heights. Thread-safe on distinct scratch.
 */
int omx_align_coarse_delay(const double *cross_re, const double *cross_im, uint32_t fft_size, int32_t k_lo,
                           int32_t k_hi, enum omx_align_arrival arrival, double *scratch_re, double *scratch_im,
                           OmxCoarseDelay *out);

/**
 * @brief The fit: delay, polarity, coherence and residual over a folded run (core's
 *        `fitAlignment`).
 * @param a The accumulator.
 * @param min_frames The fewest frames a coherence may be computed from (core's `MIN_FRAMES`).
 * @param low_hz The band's lower edge (core's `DEFAULT_BAND.lowHz` by default).
 * @param high_hz The band's upper edge.
 * @param arrival Which name the correlation peak gets; an offset run names it `either`.
 * @param trust_coherence The coherence at and above which a residual over half a sample refuses
 *        (core's `ALIGN_TRUST_COHERENCE`).
 * @param scratch_re `fft_size` doubles of scratch.
 * @param scratch_im `fft_size` doubles of scratch.
 * @param out Receives the fit when the return is `OMX_ANALYSIS_OK`; `frames` is set either way.
 * @return `OMX_ANALYSIS_OK`, or the refusal: too-few-frames, shape-mismatch, empty-band,
 *         no-shared-energy, residual-exceeds-half-sample.
 * @note Allocation-free, O(n log n). Thread-safe on distinct accumulators and scratch.
 */
enum omx_analysis_refusal omx_align_fit(const OmxXspec *a, uint32_t min_frames, double low_hz, double high_hz,
                                        enum omx_align_arrival arrival, double trust_coherence,
                                        double *scratch_re, double *scratch_im, OmxAlignFit *out);

/**
 * @brief The correction a fit implies for the channel, against what it already holds (core's
 *        `alignCorrection`): the delay is `applied − fit`, clamped to the travel, and an inversion
 *        flips whatever is set.
 * @param fit_delay_ms The fit's delay.
 * @param fit_invert The fit's polarity answer.
 * @param applied_delay_ms The channel's alignment delay now.
 * @param applied_inverted The channel's polarity inversion now.
 * @param travel_min The delay travel's lower end, ms.
 * @param travel_max Its upper end, ms.
 * @param out Receives the correction.
 * @note Allocation-free, O(1). Thread-safe: pure.
 */
void omx_align_correction(double fit_delay_ms, uint32_t fit_invert, double applied_delay_ms, uint32_t applied_inverted,
                          double travel_min, double travel_max, OmxAlignCorrection *out);

#endif /* OMX_ALIGN_FIT_H */
