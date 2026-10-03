// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The 31-band graphic EQ kernel (omx_geq.h) against its closed forms — the engine's spec
 * docs/design/specs/2026-09-26-graphic-eq-31.md §5, at every declared rate (OMX_DECLARED_RATES,
 * the generated header's list, never this file's own). Contracts are compiled in and the last
 * line asserts the ledger came out empty.
 *
 *   A  one band: each of the 31 sections alone at ±15 and ±6 dB — |H| measured from the kernel's
 *      impulse response on a 1/24-octave grid equals the closed form of the section (0.005 dB).
 *   B  standard cascade: all +12, alternating ±12, the one-band and tilt rows and 20 random fader
 *      sets on the 0.5 dB grid — |H| at the centres and midpoints equals the product of the 31
 *      closed forms (0.02 dB); the grid maximum is G's C half (0.01 dB).
 *
 * The centres are the ISO base-2 third-octave law 1000·2^((k−17)/3), whose rounding the ISO list
 * is; the band Q is the ISO third-octave bandwidth 2^(1/6)/(2^(1/3) − 1). Both are the oracle's
 * own closed forms: the kernel is handed coefficient sets and reads neither.
 *
 * These are the closed-form arms of the engine's mix_geq.test.c, moved with the kernel; the
 * identity, thread and cost arms are test/fx/geq.test.c.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_contract_limits.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/fx/omx_geq.h>

#include "fx_rates.h"

static int g_checks, g_failed;

static void ok(int cond, const char *what, double got, double want) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s: got %.6g want %.6g\n", what, got, want);
  }
}

static uint32_t g_rng = 0x9e3779b9u;
static float rnd01(void) {
  g_rng = g_rng * 1664525u + 1013904223u;
  return (float)(g_rng >> 8) / 16777216.0f;
}

static double centre_hz(int k) { return 1000.0 * pow(2.0, (double)(k - 17) / 3.0); }
static double band_q(void) { return pow(2.0, 1.0 / 6.0) / (pow(2.0, 1.0 / 3.0) - 1.0); }

/** @brief |H(e^{jω})| of one normalised section `{b0,b1,b2,a1,a2}`, in dB, in double. */
static double section_db(const double c[5], double f, double sr) {
  const double w = 2.0 * M_PI * f / sr, c1 = cos(w), s1 = sin(w), c2 = cos(2.0 * w), s2 = sin(2.0 * w);
  const double nr = c[0] + c[1] * c1 + c[2] * c2, ni = -(c[1] * s1 + c[2] * s2);
  const double dr = 1.0 + c[3] * c1 + c[4] * c2, di = -(c[3] * s1 + c[4] * s2);
  return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

/** @brief The closed form of a whole setting: the sum of the live sections' dB responses. */
static double closed_db(const float gains[OMX_GEQ_BANDS], double f, double sr) {
  double s = 0.0;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    if (gains[k] == 0.0f) continue;
    double c[5];
    omx_eq_design(OMX_EQ_PEAKING, centre_hz(k), band_q(), gains[k], sr, c);
    s += section_db(c, f, sr);
  }
  return s;
}

/** @brief Resolve an atom from section gains: the channel EQ's matched-Z bell, kept in double. */
static void resolve(struct omx_geq *p, const float gains[OMX_GEQ_BANDS], double sr) {
  double c[OMX_GEQ_BANDS][5];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) omx_eq_design(OMX_EQ_PEAKING, centre_hz(k), band_q(), gains[k], sr, c[k]);
  omx_geq_set(p, 1, (const double(*)[5])c, gains);
}

/** @brief The impulse length that lets the lowest live section decay below −150 dB, plus margin. */
static uint32_t impulse_len(const float gains[OMX_GEQ_BANDS], double sr) {
  int lo = OMX_GEQ_BANDS;
  for (int k = 0; k < OMX_GEQ_BANDS; k++)
    if (gains[k] != 0.0f) {
      lo = k;
      break;
    }
  if (lo == OMX_GEQ_BANDS) return 1024u;
  return (uint32_t)(18.0 * band_q() / (M_PI * centre_hz(lo)) * sr) + 2048u;
}

/** @brief |DFT| of `h` at `f`, in dB — a rotating phasor, renormalised every 4096 samples. */
static double dft_db(const float *h, uint32_t n, double f, double sr) {
  const double w = 2.0 * M_PI * f / sr, cr = cos(w), ci = -sin(w);
  double pr = 1.0, pi = 0.0, re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    re += h[i] * pr;
    im += h[i] * pi;
    const double t = pr * cr - pi * ci;
    pi = pr * ci + pi * cr;
    pr = t;
    if ((i & 4095u) == 4095u) {
      const double m = 1.0 / hypot(pr, pi);
      pr *= m;
      pi *= m;
    }
  }
  return 10.0 * log10(re * re + im * im);
}

/** @brief The kernel's impulse response for a setting; the caller frees it. */
static float *impulse(const float gains[OMX_GEQ_BANDS], double sr, uint32_t *n_out) {
  const uint32_t n = impulse_len(gains, sr);
  float *h = calloc(n, sizeof *h);
  if (!h) abort();
  h[0] = 1.0f;
  static struct omx_geq p;
  static struct omx_geq_state st;
  resolve(&p, gains, sr);
  omx_geq_state_init(&st);
  omx_geq_process(h, NULL, n, &p, &st);
  *n_out = n;
  return h;
}

static void arm_a_one_band(void) {
  static const float G[] = {15.0f, -15.0f, 6.0f, -6.0f};
  double worst = 0.0;
  for (unsigned ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    for (int k = 0; k < OMX_GEQ_BANDS; k++) {
      for (unsigned g = 0; g < sizeof G / sizeof G[0]; g++) {
        float gains[OMX_GEQ_BANDS] = {0};
        gains[k] = G[g];
        uint32_t n;
        float *h = impulse(gains, sr, &n);
        for (int j = -24; j <= 24; j++) {
          const double f = centre_hz(k) * pow(2.0, j / 24.0);
          if (f >= 0.49 * sr) continue;
          const double e = fabs(dft_db(h, n, f, sr) - closed_db(gains, f, sr));
          if (e > worst) worst = e;
          ok(e <= 0.005, "A: one band's |H| equals its closed form (dB error)", e, 0.005);
        }
        free(h);
      }
    }
  }
  printf("A one band: 31 bands x {+15,-15,+6,-6} x %d rates, worst %.5f dB (bound 0.005)\n",
         (int)OMX_DECLARED_RATE_COUNT, worst);
}

static void arm_b_cascade(void) {
  double worst = 0.0, worst_peak = 0.0;
  for (unsigned ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    for (int set = 0; set < 24; set++) {
      float gains[OMX_GEQ_BANDS];
      for (int k = 0; k < OMX_GEQ_BANDS; k++) {
        if (set == 0) gains[k] = 12.0f;
        else if (set == 1) gains[k] = (k & 1) ? -12.0f : 12.0f;
        else if (set == 2) gains[k] = k == 17 ? 12.0f : 0.0f;
        else if (set == 3) gains[k] = -12.0f + 24.0f * (float)k / (float)(OMX_GEQ_BANDS - 1);
        else gains[k] = 0.5f * floorf(rnd01() * 61.0f) - 15.0f;
      }
      uint32_t n;
      float *h = impulse(gains, sr, &n);
      double peak_k = -1e9, peak_c = -1e9;
      for (int j = 0; j < 2 * OMX_GEQ_BANDS - 1; j++) {
        const double f = centre_hz(0) * pow(2.0, j / 6.0);
        if (f >= 0.49 * sr) continue;
        const double mk = dft_db(h, n, f, sr), mc = closed_db(gains, f, sr);
        const double e = fabs(mk - mc);
        if (e > worst) worst = e;
        if (mk > peak_k) peak_k = mk;
        if (mc > peak_c) peak_c = mc;
        ok(e <= 0.02, "B: the cascade's |H| equals the product of 31 closed forms (dB error)", e, 0.02);
      }
      const double ep = fabs(peak_k - peak_c);
      if (ep > worst_peak) worst_peak = ep;
      ok(ep <= 0.01, "G: the measured grid maximum equals the closed form's (dB error)", ep, 0.01);
      free(h);
    }
  }
  printf("B cascade: 24 settings x %d rates at centres+midpoints, worst %.5f dB (bound 0.02); "
         "G grid peak worst %.5f dB (bound 0.01)\n",
         (int)OMX_DECLARED_RATE_COUNT, worst, worst_peak);
}

int main(void) {
  omx_fx_require_rate_floor();
  printf("fx/geq_math: %d declared rates, %d sections, Q %.4f\n", (int)OMX_DECLARED_RATE_COUNT, OMX_GEQ_BANDS, band_q());
  arm_a_one_band();
  arm_b_cascade();
  printf("fx/geq_math: %d checks, %d failed; %u contracts evaluated, %u violations\n", g_checks, g_failed,
         omx_contract_log.checks, omx_contract_log.count);
  if (omx_contract_log.count != 0u) g_failed++;
  return g_failed == 0 ? 0 : 1;
}
