// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_fft.h
 * @brief The Hann window and the radix-2 FFTs: complex f32 forward, real f32 forward (n/2 bins
 *        from one half-length complex transform) and complex f64 forward and inverse.
 *
 * Pure arithmetic over caller-owned arrays: no allocation, no table, no state, so any thread may
 * call them. They are analysis transforms for an operator-cadence thread, never a process callback.
 *
 * Every twiddle rides a rotate-as-you-go recurrence held in DOUBLE, started from cos/sin of the
 * stage angle. A float recurrence compounds its rounding across the len/2 rotations of a stage and
 * left an analyser grass floor near −95 dBc at 16384; the double one measures relRMS ≤ 2e-7 against
 * a long-double reference at 1024..65536 (test/kernels/fft.c). The f32 transforms and the window
 * are the OpenMixer engine's own, moved here unchanged in behaviour: their output is bit-exact with
 * the engine's on the fixed vector set of test/golden/fft.digest.
 */
#ifndef OMX_FFT_H
#define OMX_FFT_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "omxdsp.h"
#include "omx_contract.h"

/**
 * @brief 1 when `n` is a power of two and at least 2: the lengths the radix-2 transforms take.
 * @param n The length.
 * @return 1 for 2, 4, 8, …; 0 otherwise.
 * @note RT-safe: two operations. Thread-safe.
 */
static inline int omx_fft_length_ok(uint32_t n) { return n >= 2u && (n & (n - 1u)) == 0u; }

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fft/hann"
/**
 * @brief The Hann window, in place: `buf[i] *= 0.5 − 0.5·cos(2π·i/(n−1))`.
 *
 * cos(iθ) comes from the Chebyshev recurrence `c[i+1] = 2cosθ·c[i] − c[i−1]`, run in double so the
 * error stays near 1e-13 over a 32768-step window: one multiply and one subtract per sample instead
 * of a libm call, and no table.
 * @param buf The block, `n` samples, windowed in place.
 * @param n Its length; `n < 2` is a no-op (nothing to taper).
 * @note Allocation-free, O(n). Thread-safe on distinct buffers.
 */
static inline void omx_hann_window(float *buf, uint32_t n) {
  if (n < 2) return;
  const double theta = 2.0 * M_PI / (double)(n - 1);
  const double twice_cos = 2.0 * cos(theta);
  double c_prev = cos(-theta); /* c[-1] */
  double c = 1.0;              /* c[0] */
  for (uint32_t i = 0; i < n; i++) {
    buf[i] *= (float)(0.5 - 0.5 * c);
    double c_next = twice_cos * c - c_prev;
    c_prev = c;
    c = c_next;
  }
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "fft/radix2"
/**
 * @brief In-place iterative radix-2 decimation-in-time FFT, complex f32, forward (`e^(−2πi·jk/n)`),
 *        unscaled.
 *
 * Each butterfly's product `b·W^k` is formed in double and rounded to float once; the samples stay
 * float, in place, so the scratch is the caller's two arrays and nothing else. For a real input the
 * caller zeroes `im` first, or uses {@link omx_fft_real_half}.
 * @param re The real parts, `n` long, transformed in place.
 * @param im The imaginary parts, `n` long, transformed in place.
 * @param n The length, a power of two.
 * @pre `length-power-of-two`.
 * @note Allocation-free, O(n log n). Thread-safe on distinct buffers.
 */
static inline void omx_fft_radix2(float *re, float *im, uint32_t n) {
  OMX_PRE(omx_fft_length_ok(n) || n == 1u, "length-power-of-two");
  /* bit-reversal permutation */
  for (uint32_t i = 1, j = 0; i < n; i++) {
    uint32_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float tr = re[i]; re[i] = re[j]; re[j] = tr;
      float ti = im[i]; im[i] = im[j]; im[j] = ti;
    }
  }
  /* butterflies, stage by stage (len 2, 4, 8, … n) */
  for (uint32_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * M_PI / (double)len;
    const double wr = cos(ang), wi = sin(ang);
    for (uint32_t i = 0; i < n; i += len) {
      double cwr = 1.0, cwi = 0.0;
      for (uint32_t k = 0; k < len / 2; k++) {
        uint32_t a = i + k, b = a + len / 2;
        const double br = re[b], bi = im[b];
        const float vr = (float)(br * cwr - bi * cwi);
        const float vi = (float)(br * cwi + bi * cwr);
        const float ur = re[a], ui = im[a];
        re[a] = ur + vr; im[a] = ui + vi;
        re[b] = ur - vr; im[b] = ui - vi;
        const double nwr = cwr * wr - cwi * wi;
        const double nwi = cwr * wi + cwi * wr;
        cwr = nwr; cwi = nwi;
      }
    }
  }
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "fft/real-half"
/**
 * @brief The first n/2 bins of the DFT of a REAL n-sample block, from ONE complex FFT of length n/2.
 *
 * The even/odd packing: `z[k] = x[2k] + i·x[2k+1]`, `Z = FFT(z)`, and for k in [0, n/2), with
 * `M = n/2` and `Z[M] ≡ Z[0]`:
 * `E[k] = (Z[k] + conj(Z[M−k]))/2`, `O[k] = −i·(Z[k] − conj(Z[M−k]))/2`, `X[k] = E[k] + W^k·O[k]`,
 * `W = e^(−2πi/n)`. Half the butterflies of a same-length complex transform on a zero imaginary
 * part. The Nyquist bin is not returned.
 * @param work_re On entry its first n samples are the real block; on return `work_re[k]`, k < n/2,
 *        is Re X[k]. n long; its back half is scratch.
 * @param work_im n long, scratch on entry; on return `work_im[k]`, k < n/2, is Im X[k].
 * @param n The block length, a power of two ≥ 4.
 * @pre `length-power-of-two-ge-4`.
 * @note Allocation-free, O(n log n). Thread-safe on distinct buffers.
 */
static inline void omx_fft_real_half(float *work_re, float *work_im, uint32_t n) {
  OMX_PRE(omx_fft_length_ok(n) && n >= 4u, "length-power-of-two-ge-4");
  const uint32_t m = n / 2;
  /* Pack the even samples into the real leg and the odd ones into the imaginary leg, both inside
   * `work_im` (front and back halves), so the input in `work_re` is only ever read here. */
  float *zr = work_im, *zi = work_im + m;
  for (uint32_t k = 0; k < m; k++) {
    zr[k] = work_re[2 * k];
    zi[k] = work_re[2 * k + 1];
  }
  omx_fft_radix2(zr, zi, m);
  /* Split: X[k]'s real part goes to work_re[k], its imaginary part to work_re[m + k]; the input
   * is fully consumed and `work_im` is still being read as Z. k = 0 pairs with itself. */
  float *xr = work_re, *xi = work_re + m;
  const double theta = -2.0 * M_PI / (double)n;
  const double dwr = cos(theta), dwi = sin(theta);
  double wr = 1.0, wi = 0.0;
  for (uint32_t k = 0; k < m; k++) {
    const uint32_t j = (m - k) & (m - 1); /* M − k, with M ≡ 0 */
    const float ar = zr[k], ai = zi[k];
    const float br = zr[j], bi = -zi[j]; /* conj(Z[M−k]) */
    const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
    /* O = −i · (Z − conj)/2 : (dr + i·di)·(−i) = di − i·dr */
    const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
    const float orr = di, oi = -dr;
    const float twr = (float)wr, twi = (float)wi;
    xr[k] = er + (orr * twr - oi * twi);
    xi[k] = ei + (orr * twi + oi * twr);
    const double nwr = wr * dwr - wi * dwi;
    const double nwi = wr * dwi + wi * dwr;
    wr = nwr;
    wi = nwi;
  }
  memcpy(work_im, xi, m * sizeof(float));
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "fft/radix2-d"
/**
 * @brief In-place iterative radix-2 FFT, complex f64, forward or inverse.
 *
 * The same decimation-in-time butterflies and double twiddle recurrence as {@link omx_fft_radix2},
 * on double samples. Forward is `e^(−2πi·jk/n)`, unscaled; inverse is `e^(+2πi·jk/n)` with every
 * output divided by n, so inverse(forward(x)) returns x.
 * @param re The real parts, `n` long, transformed in place.
 * @param im The imaginary parts, `n` long, transformed in place.
 * @param n The length, a power of two.
 * @param inverse 0 for the forward transform, non-zero for the scaled inverse.
 * @pre `length-power-of-two`.
 * @note Allocation-free, O(n log n). Thread-safe on distinct buffers.
 */
static inline void omx_fft_radix2_d(double *re, double *im, uint32_t n, int inverse) {
  OMX_PRE(omx_fft_length_ok(n) || n == 1u, "length-power-of-two");
  for (uint32_t i = 1, j = 0; i < n; i++) {
    uint32_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      double tr = re[i]; re[i] = re[j]; re[j] = tr;
      double ti = im[i]; im[i] = im[j]; im[j] = ti;
    }
  }
  const double sign = inverse ? 1.0 : -1.0;
  for (uint32_t len = 2; len <= n; len <<= 1) {
    const double ang = sign * 2.0 * M_PI / (double)len;
    const double wr = cos(ang), wi = sin(ang);
    for (uint32_t i = 0; i < n; i += len) {
      double cwr = 1.0, cwi = 0.0;
      for (uint32_t k = 0; k < len / 2; k++) {
        uint32_t a = i + k, b = a + len / 2;
        const double vr = re[b] * cwr - im[b] * cwi;
        const double vi = re[b] * cwi + im[b] * cwr;
        const double ur = re[a], ui = im[a];
        re[a] = ur + vr; im[a] = ui + vi;
        re[b] = ur - vr; im[b] = ui - vi;
        const double nwr = cwr * wr - cwi * wi;
        const double nwi = cwr * wi + cwi * wr;
        cwr = nwr; cwi = nwi;
      }
    }
  }
  if (inverse) {
    for (uint32_t i = 0; i < n; i++) {
      re[i] /= (double)n;
      im[i] /= (double)n;
    }
  }
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_FFT_H */
