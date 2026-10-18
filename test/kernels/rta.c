// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/analysis/omx_rta.h>

#include "../fx/fx_rates.h"

/* ---- the analyser's spectrum readings and half-band decimator --------------------------------
 *
 * The engine's oracles (the engine's mix_dsp.test.c: the decimator and the known sinusoid;
 * mix_align.test.c: the complex reading), moved with the code. The decimator's passband, stopband
 * and three-stage cascade, and the known sinusoid, then run again at each of the nine oracle rates. */

/* The engine's low zoom: three ÷2 stages of a 65536-sample window feed an 8192-point FFT. */
#define RTA_LOW_FFT 8192u
#define RTA_LOW_FACTOR 8u
#define RTA_LOW_WINDOW (RTA_LOW_FFT * RTA_LOW_FACTOR)
#define RTA_N 1024u
#define RTA_BIN 64u

static float rta_x[RTA_LOW_WINDOW];
static float rta_s1[RTA_LOW_WINDOW / 2u], rta_s2[RTA_LOW_WINDOW / 4u], rta_s3[RTA_LOW_WINDOW / 8u];
static float rta_work_re[RTA_LOW_FFT], rta_work_im[RTA_LOW_FFT], rta_mags[RTA_LOW_FFT / 2u];
static float rta_re[RTA_N / 2u], rta_im[RTA_N / 2u], rta_mags_ref[RTA_N / 2u];

/* RMS amplitude of a pure sine over the interior, `guard` samples clear of each clamped edge. */
static float rta_sine_amp_interior(const float *x, uint32_t n, uint32_t guard) {
  if (n <= 2 * guard) return 0.0f;
  double sumsq = 0.0;
  uint32_t count = 0;
  for (uint32_t i = guard; i < n - guard; i++) {
    sumsq += (double)x[i] * (double)x[i];
    count++;
  }
  return (float)(sqrt(2.0) * sqrt(sumsq / (double)count));
}

/* A tone well inside the passband survives one ÷2 stage near unity. */
static void rta_halfband_passband(float sr) {
  const uint32_t n = 65536;
  for (uint32_t i = 0; i < n; i++) rta_x[i] = sinf(2.0f * (float)M_PI * 1000.0f * (float)i / sr);
  const uint32_t n_out = omx_decimate_halfband(rta_x, n, rta_s1);
  ok(n_out == n / 2, "halfband decimate: n_out == n_in/2", n_out, n / 2);
  const float amp = rta_sine_amp_interior(rta_s1, n_out, 200);
  ok(amp > 0.97f && amp < 1.03f, "halfband passband: a 1 kHz tone survives one ÷2 stage near unity", amp, 1.0);
}

/* A tone above the Fs/4 cutoff (16 kHz at 48 kHz, which would alias to 8 kHz) is suppressed. */
static void rta_halfband_stopband(float sr, float hz) {
  const uint32_t n = 65536;
  for (uint32_t i = 0; i < n; i++) rta_x[i] = sinf(2.0f * (float)M_PI * hz * (float)i / sr);
  omx_decimate_halfband(rta_x, n, rta_s1);
  const float amp = rta_sine_amp_interior(rta_s1, n / 2, 200);
  ok(amp < 0.05f, "halfband stopband: a tone above the Fs/4 cutoff is suppressed, not aliased", amp, 0.05);
}

/* Three cascaded stages on a tone on a bin of the 8192-point FFT at the ÷8 rate: it peaks at
 * exactly that bin, near unity. */
static void rta_halfband_cascade(float sr) {
  const uint32_t k0 = 100;
  const float eff_sr = sr / (float)RTA_LOW_FACTOR;
  const float f0 = (float)k0 * eff_sr / (float)RTA_LOW_FFT;
  for (uint32_t i = 0; i < RTA_LOW_WINDOW; i++) rta_x[i] = sinf(2.0f * (float)M_PI * f0 * (float)i / sr);
  const uint32_t n1 = omx_decimate_halfband(rta_x, RTA_LOW_WINDOW, rta_s1);
  const uint32_t n2 = omx_decimate_halfband(rta_s1, n1, rta_s2);
  const uint32_t n3 = omx_decimate_halfband(rta_s2, n2, rta_s3);
  ok(n3 == RTA_LOW_FFT, "halfband cascade: three ÷2 stages of the low window land on the low FFT", n3, RTA_LOW_FFT);
  omx_rta_spectrum(rta_s3, RTA_LOW_FFT, rta_work_re, rta_work_im, rta_mags);
  uint32_t peak = 1;
  for (uint32_t k = 1; k < RTA_LOW_FFT / 2; k++)
    if (rta_mags[k] > rta_mags[peak]) peak = k;
  ok(peak == k0, "halfband cascade: a bin-aligned tone peaks at exactly its own low-tier bin", peak, k0);
  ok(rta_mags[k0] > 0.85f && rta_mags[k0] < 1.15f, "halfband cascade: the low-tier level reads near unity", rta_mags[k0], 1.0);
}

static void rta_halfband_short_blocks(void) {
  float out[4] = {7.0f, 7.0f, 7.0f, 7.0f};
  const float one[1] = {1.0f};
  ok(omx_decimate_halfband(one, 1, out) == 0 && out[0] == 7.0f, "halfband: fewer than two samples write nothing", out[0], 7.0);
  const float three[3] = {0.5f, 0.5f, 0.5f};
  ok(omx_decimate_halfband(three, 3, out) == 1, "halfband: an odd trailing sample is dropped", 1.0, 1.0);
  ok(fabsf(out[0] - 0.5f) < 1e-6f, "halfband: a clamped short block of DC is DC", out[0], 0.5);
}

/* A bin-aligned full-scale tone at the FFT length the analyser uses at 48 kHz (8192) lands in its
 * own bin at near unity; a distant bin carries only leakage; silence is silent. */
static void rta_known_sinusoid(float sr, uint32_t k0) {
  const uint32_t n = RTA_LOW_FFT;
  const float f0 = (float)k0 * sr / (float)n;
  for (uint32_t i = 0; i < n; i++) rta_x[i] = sinf(2.0f * (float)M_PI * f0 * (float)i / sr);
  omx_rta_spectrum(rta_x, n, rta_work_re, rta_work_im, rta_mags);
  float energy = 0.0f;
  for (uint32_t k = 0; k < n / 2; k++) energy += rta_mags[k] * rta_mags[k];
  ok(energy > 0.0f, "the spectrum is not degenerate", energy, 0.0);
  uint32_t peak = 1;
  for (uint32_t k = 1; k < n / 2; k++)
    if (rta_mags[k] > rta_mags[peak]) peak = k;
  ok(peak == k0, "a bin-aligned tone peaks at exactly its own FFT bin", peak, k0);
  ok(rta_mags[k0] > 0.85f && rta_mags[k0] < 1.15f, "a full-scale bin-aligned tone reads near unity", rta_mags[k0], 1.0);
  ok(rta_mags[k0 + 200] < rta_mags[k0] * 0.05f, "a distant bin carries only sidelobe leakage", rta_mags[k0 + 200], rta_mags[k0] * 0.05f);
  memset(rta_x, 0, n * sizeof(float));
  omx_rta_spectrum(rta_x, n, rta_work_re, rta_work_im, rta_mags);
  float worst = 0.0f;
  for (uint32_t k = 0; k < n / 2; k++)
    if (fabsf(rta_mags[k]) > worst) worst = fabsf(rta_mags[k]);
  ok(worst <= 1e-5f, "silence transforms to a silent spectrum", worst, 1e-5);
}

static double rta_wrap_pi(double a) {
  while (a > M_PI) a -= 2.0 * M_PI;
  while (a <= -M_PI) a += 2.0 * M_PI;
  return a;
}

/* A bin-centred sine: exactly `bin` cycles across the window. */
static void rta_synth(float *out, unsigned n, unsigned bin, double phase, double shift_samples) {
  for (unsigned i = 0; i < n; i++) {
    const double t = (double)i - shift_samples;
    out[i] = (float)sin(2.0 * M_PI * (double)bin * t / (double)n + phase);
  }
}

/* The complex reading: a tone lands in its own bin; a pure delay leaves every magnitude unchanged
 * and moves the phase at bin k by exactly -2πkD/n; hypot(re, im) is the magnitude reading; silence
 * is exactly zero in both parts. */
static void rta_complex(void) {
  rta_synth(rta_x, RTA_N, RTA_BIN, 0.7, 0.0);
  omx_rta_spectrum_complex(rta_x, RTA_N, rta_work_re, rta_work_im, rta_re, rta_im);
  unsigned peak = 0;
  double best = -1.0;
  for (unsigned k = 0; k < RTA_N / 2u; k++) {
    const double m = hypot((double)rta_re[k], (double)rta_im[k]);
    if (m > best) { best = m; peak = k; }
  }
  ok(peak == RTA_BIN, "complex: the peak bin is the bin the tone was synthesised at", peak, RTA_BIN);
  ok(best > 0.4, "complex: the peak carries real amplitude", best, 0.4);
  ok(isfinite(atan2((double)rta_im[RTA_BIN], (double)rta_re[RTA_BIN])), "complex: the phase at the tone's bin is a number", 0.0, 0.0);

  const double D = 3.0;
  rta_synth(rta_x, RTA_N, RTA_BIN, 0.0, 0.0);
  omx_rta_spectrum(rta_x, RTA_N, rta_work_re, rta_work_im, rta_mags_ref);
  omx_rta_spectrum_complex(rta_x, RTA_N, rta_work_re, rta_work_im, rta_re, rta_im);
  const double phase_ref = atan2((double)rta_im[RTA_BIN], (double)rta_re[RTA_BIN]);
  rta_synth(rta_x, RTA_N, RTA_BIN, 0.0, D);
  omx_rta_spectrum(rta_x, RTA_N, rta_work_re, rta_work_im, rta_mags);
  omx_rta_spectrum_complex(rta_x, RTA_N, rta_work_re, rta_work_im, rta_re, rta_im);
  const double phase_del = atan2((double)rta_im[RTA_BIN], (double)rta_re[RTA_BIN]);
  double worst = 0.0;
  for (unsigned k = 0; k < RTA_N / 2u; k++) {
    const double d = fabs((double)rta_mags[k] - (double)rta_mags_ref[k]);
    if (d > worst) worst = d;
  }
  ok(worst <= 1e-5, "complex: a pure delay leaves every magnitude bin unchanged", worst, 1e-5);
  const double want = rta_wrap_pi(-2.0 * M_PI * (double)RTA_BIN * D / (double)RTA_N);
  const double got = rta_wrap_pi(phase_del - phase_ref);
  ok(fabs(got - want) <= 1e-3, "complex: the phase shift at bin k is exactly -2πkD/n", got, want);

  rta_synth(rta_x, RTA_N, RTA_BIN, 1.3, 0.0);
  omx_rta_spectrum(rta_x, RTA_N, rta_work_re, rta_work_im, rta_mags);
  omx_rta_spectrum_complex(rta_x, RTA_N, rta_work_re, rta_work_im, rta_re, rta_im);
  worst = 0.0;
  for (unsigned k = 0; k < RTA_N / 2u; k++) {
    const double d = fabs(hypot((double)rta_re[k], (double)rta_im[k]) - (double)rta_mags[k]);
    if (d > worst) worst = d;
  }
  ok(worst <= 1e-6, "complex: hypot(re, im) equals the magnitude reading, bin for bin", worst, 1e-6);

  memset(rta_x, 0, RTA_N * sizeof(float));
  omx_rta_spectrum_complex(rta_x, RTA_N, rta_work_re, rta_work_im, rta_re, rta_im);
  int finite = 1, zero = 1;
  for (unsigned k = 0; k < RTA_N / 2u; k++) {
    if (!isfinite(rta_re[k]) || !isfinite(rta_im[k])) finite = 0;
    if (rta_re[k] != 0.0f || rta_im[k] != 0.0f) zero = 0;
  }
  ok(finite, "complex: silence gives finite parts, never a NaN", finite, 1.0);
  ok(zero, "complex: silence gives exactly zero in both parts", zero, 1.0);
}

static void arm_rta(void) {
  g_arm = "rta";
  rta_halfband_passband(48000.0f);
  rta_halfband_stopband(48000.0f, 16000.0f);
  rta_halfband_cascade(48000.0f);
  rta_halfband_short_blocks();
  rta_known_sinusoid(48000.0f, 256u); /* 1500 Hz */
  rta_complex();
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    const float sr = OMX_FX_ORACLE_RATES[ri];
    rta_halfband_passband(sr);
    rta_halfband_stopband(sr, sr / 3.0f);
    rta_halfband_cascade(sr);
    rta_known_sinusoid(sr, 256u);
  }
  expect_clean();
}
