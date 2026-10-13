// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_rta.h
 * @brief The analyser's spectrum readings (a Hann-windowed FFT kept as magnitudes or as complex
 *        values) and its half-band ÷2 decimator, for the low-frequency zoom.
 *
 * One-shot block transforms over caller-owned scratch: each reading takes a fresh block (a ring
 * snapshot), keeps no state and allocates nothing. They run on an analysis thread, never in a
 * process callback. The window and the transforms are omx_fft.h's; the half-band table and its
 * interior dot product are omx_oversampler.h's.
 *
 * Moved from the OpenMixer engine (mix_dsp.h) unchanged in behaviour:
 * test/golden/engine_kernels.sha256 holds its output to the engine's, bit for bit.
 */
#ifndef OMX_RTA_H
#define OMX_RTA_H

#include <math.h>
#include <stdint.h>

#include "omx_fft.h"
#include "omx_oversampler.h"

/**
 * @brief Window a copy of `block` and transform it into `work_re`/`work_im`, unscaled.
 *
 * The one window and the one FFT both readings below share: a magnitude spectrum and a complex
 * spectrum differ only in what they keep afterwards. From n ≥ 4 the transform is the real-input
 * half-length one (omx_fft_real_half); n = 2 keeps the plain complex transform.
 * @param block The block, `n` samples, read only.
 * @param n Its length, a power of two ≥ 2.
 * @param work_re Scratch and the real parts out, `n` floats.
 * @param work_im Scratch and the imaginary parts out, `n` floats.
 * @note Allocation-free, O(n log n). Thread-safe on distinct scratch.
 */
static inline void omx_rta_transform(const float *block, uint32_t n, float *work_re,
                                     float *work_im) {
  for (uint32_t i = 0; i < n; i++) work_re[i] = block[i];
  omx_hann_window(work_re, n);
  if (n >= 4) {
    omx_fft_real_half(work_re, work_im, n);
  } else {
    for (uint32_t i = 0; i < n; i++) work_im[i] = 0.0f;
    omx_fft_radix2(work_re, work_im, n);
  }
}

/**
 * @brief Linear-magnitude spectrum of a real block: `mags[k]` is the amplitude at `k·rate/n` Hz.
 *
 * The `4/n` scale corrects for the Hann window's 0.5 coherent gain, so a full-scale, bin-aligned
 * sinusoid reads back at about unity (0 dBFS).
 * @param block The block, `n` samples, read only.
 * @param n Its length, a power of two ≥ 2.
 * @param work_re Scratch, `n` floats.
 * @param work_im Scratch, `n` floats.
 * @param mags The magnitudes out, `n/2` floats.
 * @note Allocation-free, O(n log n). Thread-safe on distinct scratch.
 */
static inline void omx_rta_spectrum(const float *block, uint32_t n, float *work_re,
                                    float *work_im, float *mags) {
  omx_rta_transform(block, n, work_re, work_im);
  const float scale = 4.0f / (float)n;
  for (uint32_t k = 0; k < n / 2; k++) {
    float re = work_re[k], im = work_im[k];
    mags[k] = sqrtf(re * re + im * im) * scale;
  }
}

/**
 * @brief The same transform keeping phase: `out_re[k]`/`out_im[k]` for k < n/2, scaled as
 *        omx_rta_spectrum scales magnitude, so `hypot(out_re[k], out_im[k]) == mags[k]`.
 *
 * A pure delay leaves every magnitude unchanged and shows only as a phase ramp, -2πfτ, so
 * measuring a delay between two signals needs this reading, and averaging it across frames lets
 * a stable phase relationship add and an unstable one cancel (coherence).
 * @param block The block, `n` samples, read only.
 * @param n Its length, a power of two ≥ 2.
 * @param work_re Scratch, `n` floats.
 * @param work_im Scratch, `n` floats.
 * @param out_re The real parts out, `n/2` floats; may alias `work_re`.
 * @param out_im The imaginary parts out, `n/2` floats; may alias `work_im`.
 * @note Allocation-free, O(n log n). Thread-safe on distinct scratch.
 */
static inline void omx_rta_spectrum_complex(const float *block, uint32_t n, float *work_re,
                                            float *work_im, float *out_re, float *out_im) {
  omx_rta_transform(block, n, work_re, work_im);
  const float scale = 4.0f / (float)n;
  for (uint32_t k = 0; k < n / 2; k++) {
    out_re[k] = work_re[k] * scale;
    out_im[k] = work_im[k] * scale;
  }
}

/**
 * @brief One decimated output whose half-band kernel reaches outside `[0, last]`: every tap's
 *        index clamped to the block.
 * @param in The block.
 * @param c The output's centre sample, an even index.
 * @param last The block's last index.
 * @return The filtered sample.
 * @note Allocation-free: OMX_HALFBAND_ODD_TAPS clamped tap pairs. Thread-safe: pure.
 */
static inline float omx_decimate_halfband_edge(const float *in, int32_t c, int32_t last) {
  float acc = OMX_HALFBAND_CENTER * in[c];
  for (uint32_t i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) {
    int32_t k = (int32_t)(2 * i + 1);
    int32_t lo = c - k;
    if (lo < 0) lo = 0;
    int32_t hi = c + k;
    if (hi > last) hi = last;
    acc += OMX_HALFBAND_ODD_COEF[i] * (in[lo] + in[hi]);
  }
  return acc;
}

/**
 * @brief One half-band ÷2 decimation stage of a block: the FIR fused with the downsample, so only
 *        every other filtered sample is computed.
 *
 * `n_in` samples of `in` give `n_in/2` samples of `out` (a trailing odd sample is dropped). Where
 * the kernel would reach outside the block, the edge sample repeats rather than zero-padding: the
 * block is a snapshot of recent history with no true silence at its edges, and the clamp touches
 * only the ~24 taps' worth of samples nearest each edge. The interior is omx_halfband_dot, the one
 * kernel that reads the table, which the compiler can vectorise across outputs.
 * @param in The block, `n_in` samples.
 * @param n_in Its length; below 2 nothing is written.
 * @param out `n_in/2` outputs; must not overlap `in`.
 * @return Outputs written, `n_in/2`.
 * @note Allocation-free, O(n_in), no state. Thread-safe on distinct buffers.
 */
static inline uint32_t omx_decimate_halfband(const float *in, uint32_t n_in, float *out) {
  if (n_in < 2) return 0;
  const uint32_t n_out = n_in / 2;
  const int32_t last = (int32_t)n_in - 1;
  /* The kernel's reach: 2·OMX_HALFBAND_ODD_TAPS − 1 samples either side of the centre. */
  const int32_t reach = 2 * OMX_HALFBAND_ODD_TAPS - 1;
  /* Outputs whose kernel stays inside the block: centre c = 2o with c − reach ≥ 0 and
   * c + reach ≤ last. Everything else takes the clamped edge path. */
  uint32_t o_lo = (uint32_t)((reach + 1) / 2);
  uint32_t o_hi = (last - reach) >= 0 ? (uint32_t)((last - reach) / 2) + 1 : 0;
  if (o_hi < o_lo) o_hi = o_lo;
  if (o_lo > n_out) o_lo = n_out;
  if (o_hi > n_out) o_hi = n_out;
  for (uint32_t o = 0; o < o_lo; o++) out[o] = omx_decimate_halfband_edge(in, (int32_t)(o * 2), last);
  for (uint32_t o = o_lo; o < o_hi; o++) out[o] = omx_halfband_dot(in + o * 2);
  for (uint32_t o = o_hi; o < n_out; o++) out[o] = omx_decimate_halfband_edge(in, (int32_t)(o * 2), last);
  return n_out;
}

#endif /* OMX_RTA_H */
