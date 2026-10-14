// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_sweep.c — the Farina sweep and its deconvolution (<omxdsp/analysis/omx_sweep.h>), moved from
 * OpenMixer core's TypeScript (room-sweep-stimulus.ts).
 *
 * Every reading is in double and in the order core's TypeScript computed it, so the parity table
 * (test/analysis/room_ts.test.c) holds this file to core's answers within the stated tolerances.
 */
#include <omxdsp/analysis/omx_sweep.h>

#include <math.h>
#include <string.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_fft.h>

#define OMX_CONTRACT_STAGE "analysis/sweep"

/* Round half up, as the TS `Math.round` core computed with. */
static double round_half_up(double x) {
  double r = floor(x);
  if (x - r >= 0.5) r += 1.0;
  return r;
}

uint32_t omx_sweep_length(double sample_rate, double duration_s) {
  const double n = round_half_up(duration_s * sample_rate);
  if (!(n > 0.0) || !(n <= 2147483648.0)) return 0;
  return (uint32_t)n;
}

/* Raised-cosine fades: the first `fade_in` samples rise 0→1, the last `fade_out` fall 1→0. */
static void hann_fades(double *x, uint32_t n, double fade_in, double fade_out) {
  double fi = fade_in > 0.0 ? fade_in : 0.0;
  fi = fi < (double)n ? fi : (double)n;
  for (uint32_t i = 0; (double)i < fi; i++) x[i] *= 0.5 * (1.0 - cos((M_PI * (double)i) / fi));
  double fo = fade_out > 0.0 ? fade_out : 0.0;
  fo = fo < (double)n ? fo : (double)n;
  for (uint32_t i = 0; (double)i < fo; i++) x[n - 1u - i] *= 0.5 * (1.0 - cos((M_PI * (double)i) / fo));
}

int omx_sweep_generate(double sample_rate, double f0, double f1, double duration_s, double fade_in_s,
                       double fade_out_s, double *stimulus, double *inverse, uint32_t n, double *sweep_rate_s) {
  if (!(sample_rate > 0.0) || !(f0 > 0.0) || !(f1 > f0) || !(f1 < sample_rate / 2.0) || !(duration_s > 0.0))
    return 0;
  if (n < 2u || n != omx_sweep_length(sample_rate, duration_s)) return 0;
  const double realised = (double)n / sample_rate;
  const double L = realised / log(f1 / f0);
  const double K = 2.0 * M_PI * f0 * L;

  for (uint32_t i = 0; i < n; i++) {
    const double t = (double)i / sample_rate;
    stimulus[i] = sin(K * (exp(t / L) - 1.0));
  }
  hann_fades(stimulus, n, round_half_up(fade_in_s * sample_rate), round_half_up(fade_out_s * sample_rate));

  /* The time-reversed sweep under the +6 dB/oct envelope e^{tRev/L}. */
  for (uint32_t i = 0; i < n; i++) {
    const double t_rev = (double)(n - 1u - i) / sample_rate;
    inverse[i] = stimulus[n - 1u - i] * exp(t_rev / L);
  }
  /* Scaled so the identity is exactly 1 at full overlap: one dot product, no transform. */
  double peak = 0.0;
  for (uint32_t i = 0; i < n; i++) peak += stimulus[i] * inverse[n - 1u - i];
  if (peak != 0.0)
    for (uint32_t i = 0; i < n; i++) inverse[i] /= peak;
  *sweep_rate_s = L;
  return 1;
}

uint32_t omx_sweep_convolve_size(uint32_t la, uint32_t lb) {
  if (la == 0u && lb == 0u) return 0;
  const uint64_t out_len = (uint64_t)la + (uint64_t)lb - 1u;
  uint32_t size = 1u;
  while ((uint64_t)size < out_len) size <<= 1;
  return size;
}

uint32_t omx_sweep_convolve(const double *a, uint32_t la, const double *b, uint32_t lb, double *scratch) {
  if (la == 0u || lb == 0u) return 0;
  const uint32_t out_len = la + lb - 1u;
  const uint32_t size = omx_sweep_convolve_size(la, lb);
  double *ar = scratch, *ai = scratch + size, *br = scratch + 2u * size, *bi = scratch + 3u * size;
  memset(scratch, 0, 4u * (size_t)size * sizeof(double));
  for (uint32_t i = 0; i < la; i++) ar[i] = a[i];
  for (uint32_t i = 0; i < lb; i++) br[i] = b[i];
  if (size >= 2u) {
    omx_fft_radix2_d(ar, ai, size, 0);
    omx_fft_radix2_d(br, bi, size, 0);
  }
  for (uint32_t k = 0; k < size; k++) {
    const double re = ar[k] * br[k] - ai[k] * bi[k];
    const double im = ar[k] * bi[k] + ai[k] * br[k];
    ar[k] = re;
    ai[k] = im;
  }
  if (size >= 2u) omx_fft_radix2_d(ar, ai, size, 1);
  return out_len;
}

void omx_sweep_recover_ir(const double *response, uint32_t nr, const double *inverse, uint32_t n, uint32_t pre_roll,
                          double *ir, uint32_t length, double *scratch) {
  const uint32_t len = omx_sweep_convolve(response, nr, inverse, n, scratch);
  const int64_t start = (int64_t)n - 1 - (int64_t)pre_roll;
  for (uint32_t i = 0; i < length; i++) {
    const int64_t idx = start + (int64_t)i;
    ir[i] = idx >= 0 && idx < (int64_t)len ? scratch[idx] : 0.0;
  }
}

double omx_sweep_harmonic_delay(double sweep_rate_s, double sample_rate, double order) {
  if (!(order >= 1.0)) return NAN;
  return sweep_rate_s * log(order) * sample_rate;
}

#undef OMX_CONTRACT_STAGE
