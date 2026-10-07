// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * mix_rotary.test.c — the rotary stage's oracle, docs/design/specs/2026-09-26-rotary-speaker.md §5
 * arms A and H, at the nine RME rates (32 to 192 kHz), closed forms in double.
 *
 *   A  frozen rotors: `speed: stop` with both rotors settled at rate 0 at 8 phases each; the
 *      stage is LTI and its impulse response's DTFT on a 200-point grid equals
 *      (1 − mix) + mix·(g_lo·a_d·L₃(ω; d_d)·LP(ω) + g_hi·a_h·L₃(ω; d_h)·(1 − LP(ω))), LP the
 *      designed section's own H(z), L₃ from the Lagrange coefficient formula — 0.01 dB above −40 dB.
 *   H  `on = false` and `mix = 0` memcmp-identical (subnormal input included); one block against
 *      random splits 1 … 4096 memcmp-identical; identical legs give L == R exactly.
 *
 * Pure C, `-lm`, no PipeWire. Built with -DOMX_CONTRACTS; every arm drains the ledger and leaves no
 * violation the rate does not explain (fx_rates.h).
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/fx/omx_rotary.h>

#include "fx_rates.h"

static int g_checks = 0, g_failed = 0;
static void ok(int cond, const char *what, double sr, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s @ %.0f — measured %.9g, limit %.9g\n", what, sr, measured, limit);
  }
}
static void drain_violations(const char *where, double sr) {
  const uint32_t unexplained = omx_fx_drain_ledger((float)sr);
  ok(unexplained == 0u, where, sr, (double)unexplained, 0.0);
}

/* The spec's §3 default speeds, Hz — the row lane declares them as ROTARY_*_RANGE defaults. */
#define HORN_SLOW 0.8f
#define HORN_FAST 6.8f
#define DRUM_SLOW 0.7f
#define DRUM_FAST 5.9f

static struct omx_rotary_state g_s, g_s2;

static double shape(double u) {
  u -= floor(u);
  const double t = 2.0 * u - 1.0;
  return 4.0 * t * (1.0 - fabs(t));
}
static double complex lagrange_h(double w, double d) {
  const double i0 = floor(d);
  double complex h = 0.0;
  for (int j = 0; j < 4; j++) {
    const double qj = i0 - 1.0 + j;
    double L = 1.0;
    for (int k = 0; k < 4; k++)
      if (k != j) L *= (d - (i0 - 1.0 + k)) / (qj - (i0 - 1.0 + k));
    h += L * cexp(-_Complex_I * w * qj);
  }
  return h;
}
static double complex section_h(const float c[5], double w) {
  const double complex z1 = cexp(-_Complex_I * w), z2 = z1 * z1;
  return ((double)c[0] + (double)c[1] * z1 + (double)c[2] * z2) /
         (1.0 + (double)c[3] * z1 + (double)c[4] * z2);
}

#define IRLEN 4096u
static float g_l[IRLEN], g_r[IRLEN];

static void arm_a(double sr) {
  static const double mixes[3] = {0.5, 1.0, 0.25};
  static const double bals[3] = {0.0, 1.0, -0.4};
  double worst = 0.0;
  for (int k = 0; k < 8; k++) {
    const float ud = (float)k / 8.0f + 0.03f, uh = (float)k / 8.0f + 0.07f;
    const double mix = mixes[k % 3], bal = bals[k % 3];
    omx_rotary_init(&g_s);
    omx_rotor_settle(&g_s.drum, 0.0f, ud);
    omx_rotor_settle(&g_s.horn, 0.0f, uh);
    struct omx_rotary p;
    omx_rotary_resolve(&p, &g_s, 1, OMX_ROTARY_STOP, HORN_SLOW, HORN_FAST, DRUM_SLOW, DRUM_FAST,
                       1.0f, (float)bal, (float)mix, (float)sr);
    memset(g_l, 0, sizeof g_l);
    g_l[0] = 1.0f;
    memcpy(g_r, g_l, sizeof g_l);
    omx_rotary_process(g_l, g_r, IRLEN, &p, &g_s);
    const double dd = OMX_ROTARY_BASE_MS * 1e-3 * sr + OMX_ROTARY_DRUM_DOPPLER_MS * 1e-3 * sr * (1.0 + shape(ud));
    const double dh = OMX_ROTARY_BASE_MS * 1e-3 * sr + OMX_ROTARY_HORN_DOPPLER_MS * 1e-3 * sr * (1.0 + shape(uh));
    const double ad = 1.0 - OMX_ROTARY_DRUM_AM * 0.5 * (1.0 - shape(ud + 0.25));
    const double ah = 1.0 - OMX_ROTARY_HORN_AM * 0.5 * (1.0 - shape(uh + 0.25));
    for (int g = 1; g <= 200; g++) {
      const double f = 20.0 * pow(0.45 * sr / 20.0, (double)(g - 1) / 199.0), w = 2.0 * M_PI * f / sr;
      double complex H = 0.0;
      for (uint32_t i = 0; i < IRLEN; i++) H += (double)g_l[i] * cexp(-_Complex_I * w * (double)i);
      const double complex lp = section_h(p.xo, w);
      const double complex ref = (1.0 - mix) + mix * ((double)p.g_lo * ad * lagrange_h(w, dd) * lp +
                                                      (double)p.g_hi * ah * lagrange_h(w, dh) * (1.0 - lp));
      if (20.0 * log10(cabs(ref)) < -40.0) continue;
      const double e = fabs(20.0 * log10(cabs(H) / cabs(ref)));
      if (e > worst) worst = e;
    }
  }
  printf("arm A @ %6.0f: worst |H − closed form| %.3g dB over 8 frozen phase pairs × 200 points\n", sr, worst);
  ok(worst <= 0.01, "arm A: the frozen stage is its closed form", sr, worst, 0.01);
  drain_violations("arm A: no contract violation", sr);
}

static uint32_t g_seed = 12345u;
static uint32_t rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return g_seed;
}

static void arm_h(double sr) {
  static float x[IRLEN], y1l[IRLEN], y1r[IRLEN], y2l[IRLEN], y2r[IRLEN];
  for (uint32_t i = 0; i < IRLEN; i++) x[i] = ((float)(rnd() >> 8) / 16777216.0f * 2.0f - 1.0f) * 0.7f;
  x[5] = 1e-40f;
  struct omx_rotary p;
  /* on = false: nothing touched. */
  omx_rotary_init(&g_s);
  omx_rotary_resolve(&p, &g_s, 0, OMX_ROTARY_FAST, HORN_SLOW, HORN_FAST, DRUM_SLOW, DRUM_FAST, 1.0f, 0.0f, 1.0f, (float)sr);
  memcpy(y1l, x, sizeof x);
  memcpy(y1r, x, sizeof x);
  memcpy(&g_s2, &g_s, sizeof g_s);
  omx_rotary_process(y1l, y1r, IRLEN, &p, &g_s);
  ok(!memcmp(y1l, x, sizeof x) && !memcmp(&g_s, &g_s2, sizeof g_s), "arm H: on=false is identity, state untouched", sr, 0, 0);
  /* mix = 0: bit-identical, the rotors still turning. */
  omx_rotary_resolve(&p, &g_s, 1, OMX_ROTARY_FAST, HORN_SLOW, HORN_FAST, DRUM_SLOW, DRUM_FAST, 1.0f, 0.0f, 0.0f, (float)sr);
  omx_rotary_process(y1l, y1r, IRLEN, &p, &g_s);
  ok(!memcmp(y1l, x, sizeof x) && !memcmp(y1r, x, sizeof x), "arm H: mix=0 is bit-identical", sr, 0, 0);
  ok(g_s.horn.lfo.phase != 0.0f, "arm H: mix=0 keeps the cabinet turning", sr, g_s.horn.lfo.phase, 0);
  /* one block against random splits; identical legs read identically. */
  for (int trial = 0; trial < 3; trial++) {
    omx_rotary_init(&g_s);
    omx_rotary_init(&g_s2);
    omx_rotor_settle(&g_s.horn, HORN_SLOW, 0.1f);
    omx_rotor_settle(&g_s2.horn, HORN_SLOW, 0.1f);
    omx_rotary_resolve(&p, &g_s, 1, OMX_ROTARY_FAST, HORN_SLOW, HORN_FAST, DRUM_SLOW, DRUM_FAST, 1.0f, 0.2f, 0.8f, (float)sr);
    memcpy(y1l, x, sizeof x); memcpy(y1r, x, sizeof x); memcpy(y2l, x, sizeof x); memcpy(y2r, x, sizeof x);
    omx_rotary_process(y1l, y1r, IRLEN, &p, &g_s);
    for (uint32_t at = 0; at < IRLEN;) {
      uint32_t n = 1u + rnd() % (trial == 0 ? 7u : trial == 1 ? 512u : 4096u);
      if (n > IRLEN - at) n = IRLEN - at;
      omx_rotary_process(y2l + at, y2r + at, n, &p, &g_s2);
      at += n;
    }
    ok(!memcmp(y1l, y2l, sizeof x) && !memcmp(y1r, y2r, sizeof x), "arm H: block splits are bit-identical", sr, trial, 0);
    ok(!memcmp(y1l, y1r, sizeof x), "arm H: identical legs give L == R", sr, trial, 0);
  }
  ok(omx_rotary_latency() == 0u, "arm H: latency is zero", sr, omx_rotary_latency(), 0);
  drain_violations("arm H: no contract violation", sr);
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t k = 0; k < OMX_FX_RME_RATE_COUNT; k++) {
    arm_a(OMX_FX_RME_RATES[k]);
    arm_h(OMX_FX_RME_RATES[k]);
  }
  printf("mix_rotary: %d checks, %d failed\n", g_checks - g_failed, g_failed);
  return g_failed ? 1 : 0;
}
