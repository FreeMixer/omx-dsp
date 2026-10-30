/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_sweep.h — the exponential sine sweep, its Farina inverse filter, and the deconvolution that
 * recovers a room's impulse response from a swept capture. Moved from OpenMixer core's
 * TypeScript (room-sweep-stimulus.ts).
 *
 *   sweep     `x(t) = sin(K·(e^{t/L} − 1))`, `K = 2π·f0·L`, `L = T / ln(f1/f0)`, T the realised
 *             duration `round(T·rate)/rate`; Hann fades of the lengths the caller asks at both ends;
 *   inverse   the time-reversed sweep under the +6 dB/oct envelope `e^{t/L}`, scaled so that
 *             `sweep ⊛ inverse` is exactly 1 at lag N − 1;
 *   deconvolution  the linear convolution through omx_fft_radix2_d: the linear impulse response
 *             sits at lag N − 1 and the k-th harmonic `L·ln(k)·rate` samples before it.
 *
 * The fade lengths are the caller's (core's default is `DEFAULT_FADE_FRACTION` of the realised
 * duration, which omx-contract does not declare yet). Control thread only, never the audio
 * callback: no allocation (every buffer and scratch is the caller's), no lock, no global state.
 */
#ifndef OMX_SWEEP_H
#define OMX_SWEEP_H

#include <stdint.h>

/**
 * @brief The sweep's length in samples, `round(duration_s · sample_rate)`.
 * @param sample_rate Hz.
 * @param duration_s Seconds.
 * @return The length; 0 when the product is not finite and positive or exceeds 2³¹.
 * @note Allocation-free, O(1). Thread-safe: pure.
 */
uint32_t omx_sweep_length(double sample_rate, double duration_s);

/**
 * @brief Generate the sweep and its Farina inverse (core's `generateSweep`).
 * @param sample_rate Hz, > 0.
 * @param f0 Start frequency, Hz, > 0.
 * @param f1 End frequency, Hz, above f0 and below Nyquist.
 * @param duration_s Requested duration, seconds; the realised one is `n / sample_rate`.
 * @param fade_in_s Hann fade-in, seconds (core: 0.02 of the realised duration by default).
 * @param fade_out_s Hann fade-out, seconds.
 * @param stimulus Receives the sweep, `n` samples.
 * @param inverse Receives the inverse filter, `n` samples.
 * @param n The length, {@link omx_sweep_length} of the rate and duration, ≥ 2.
 * @param sweep_rate_s Receives `L`, seconds.
 * @return 1; 0 when the sweep cannot exist (core's `RangeError`), touching nothing.
 * @note Allocation-free, O(n). Thread-safe on distinct buffers.
 * @twin generateSweep
 */
int omx_sweep_generate(double sample_rate, double f0, double f1, double duration_s, double fade_in_s,
                       double fade_out_s, double *stimulus, double *inverse, uint32_t n, double *sweep_rate_s);

/**
 * @brief The transform length a linear convolution of `la` and `lb` samples runs at.
 * @param la One signal's length.
 * @param lb The other's.
 * @return The smallest power of two ≥ la + lb − 1 (at least 1); 0 when both are empty, or when
 *         la + lb − 1 is over 2³¹, which none holds.
 * @pre `length-fits-a-power-of-two`.
 * @note Allocation-free, O(log n). Thread-safe: pure.
 */
uint32_t omx_sweep_convolve_size(uint32_t la, uint32_t lb);

/**
 * @brief Linear convolution `a ⊛ b` through the FFT (core's `convolve`).
 * @param a One signal.
 * @param la Its length.
 * @param b The other.
 * @param lb Its length.
 * @param scratch `4 · omx_sweep_convolve_size(la, lb)` doubles; on return its first
 *        `la + lb − 1` entries are the convolution.
 * @return The convolution's length, `la + lb − 1` (0 when either is empty, or when it is over 2³¹).
 * @note Allocation-free, O(n log n). Thread-safe on distinct scratch.
 */
uint32_t omx_sweep_convolve(const double *a, uint32_t la, const double *b, uint32_t lb, double *scratch);

/**
 * @brief The linear impulse response cut out of a recorded response (core's
 *        `recoverImpulseResponse`): deconvolve with the inverse, then take `length` samples
 *        starting `pre_roll` before the lag N − 1 (zero outside the deconvolution).
 * @param response The recorded response.
 * @param nr Its length.
 * @param inverse The sweep's inverse filter.
 * @param n Its length, the sweep's.
 * @param pre_roll Samples kept before the linear-response lag.
 * @param ir Receives the impulse response, `length` samples.
 * @param length How many.
 * @param scratch `4 · omx_sweep_convolve_size(nr, n)` doubles.
 * @note Allocation-free, O(n log n). Thread-safe on distinct buffers.
 * @twin recoverImpulseResponse
 */
void omx_sweep_recover_ir(const double *response, uint32_t nr, const double *inverse, uint32_t n, uint32_t pre_roll,
                          double *ir, uint32_t length, double *scratch);

/**
 * @brief Where the k-th harmonic lands before the linear response, samples: `L·ln(k)·rate` (core's
 *        `harmonicDelaySamples`).
 * @param sweep_rate_s The sweep's `L`, seconds.
 * @param sample_rate Hz.
 * @param order The harmonic, ≥ 1 (1 is the linear response itself).
 * @return The delay, fractional; NAN for an order under 1.
 * @note Allocation-free, O(1). Thread-safe: pure.
 * @twin harmonicDelaySamples
 */
double omx_sweep_harmonic_delay(double sweep_rate_s, double sample_rate, double order);

#endif /* OMX_SWEEP_H */
