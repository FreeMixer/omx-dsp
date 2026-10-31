// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The leveler to a loudness target (fx/omx_loudlevel_instance.h) at each of the nine oracle rates:
//   A  a range of 0 and a bypass are the input, bit for bit; a rate outside the set is refused;
//   B  on a steady stereo 997 Hz sine, whose loudness is -0.691 + 10 log10(2 |K|^2 A^2 / 2), |K| from the
//      K-weighting's two sections, the gain settles on target - L (0.02 dB) up and down, and an independent
//      meter on the output reads the target (0.05 LU);
//   C  the range: a target past the range stops the gain on the range exactly;
//   D  the speed: in the walk, the gain moves speedDbPerS dB in a second (one tick's step);
//   E  the gate and the window: nothing moves before the window has filled; in silence after the walk
//      the gain holds, bit for bit; each window reads its own span of the energy (0.1 LU);
//   F  one run cut into blocks of 1, 64, 1024 is the same output, bit for bit; the gain between ticks is a
//      line: no step between two samples larger than the walk's per-tick step allows;
//   G  hostile words land inside the travels, the ledger is clean, nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_loudlevel_instance.h>

#include "instance_oracle.h"
#include "instance_no_alloc.h"

#include <complex.h>

#define MAXN 2600000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxLoudlevelInstance g_v;
static struct omx_loudness g_m;

static double k_power(double hz) {
  double c[2][5];
  omx_loudness_kweight_design((double)g_sr, c);
  const double complex z = cexp(I * 2.0 * M_PI * hz / (double)g_sr), zi = 1.0 / z;
  double p = 1.0;
  for (int k = 0; k < 2; k++) {
    const double complex h = (c[k][0] + c[k][1] * zi + c[k][2] * zi * zi) / (1.0 + c[k][3] * zi + c[k][4] * zi * zi);
    p *= creal(h * conj(h));
  }
  return p;
}
static double sine_lufs(double a) { return -0.691 + 10.0 * log10(k_power(997.0) * a * a); }

static void vrun(const float *l, const float *r, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 4096u) {
    omx_loudlevel_instance_run(&g_v, l + o, r + o, ol + o, orr + o, n - o < 4096u ? n - o : 4096u);
    drain();
  }
}

static void arm_identity(void) {
  g_arm = "A";
  OmxLoudlevelInstance *x = &g_v;
  ok(omx_loudlevel_instance_init(x, 11025.0f) == 0, "a rate outside the oracle set is refused", 1, 0);
  const uint32_t n = 8192u;
  stimulus(g_l, g_r, n, 0x4321u, 512u);
  omx_loudlevel_instance_init(&g_v, g_sr);
  vrun(g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a range of 0, the come-up value, is the input", 1, 0);
  omx_loudlevel_instance_resolve(&g_v, 1, -20.0f, 12.0f, 1.0f, -50.0f, 0);
  vrun(g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a bypass is the input", 1, 0);
  clean();
}

static void arm_settle(void) {
  g_arm = "B";
  static const double AMPS[] = {0.01, 0.3};
  static const float TGT[] = {-30.0f, -16.0f};
  const uint32_t n = (uint32_t)(5.0f * g_sr), tail = (uint32_t)(1.0f * g_sr);
  for (int k = 0; k < 2; k++) {
    sine(g_l, n, 997.0, g_sr, AMPS[k]);
    memcpy(g_r, g_l, n * sizeof(float));
    omx_loudlevel_instance_init(&g_v, g_sr);
    omx_loudlevel_instance_resolve(&g_v, 0, TGT[k], 24.0f, 6.0f, -60.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
    vrun(g_l, g_r, g_ol, g_or, n);
    const double l = sine_lufs(AMPS[k]);
    const double want = (double)TGT[k] - l;
    ok(fabs((double)omx_loudlevel_instance_gain_db(&g_v) - want) < 0.02, "the gain settles on target - L (0.02 dB)", omx_loudlevel_instance_gain_db(&g_v), want);
    ok(fabs((double)omx_loudlevel_instance_loudness_lufs(&g_v) - l) < 0.02, "the readout is the sine's closed-form loudness", omx_loudlevel_instance_loudness_lufs(&g_v), l);
    omx_loudness_init(&g_m, g_sr, 2u, NULL);
    const float *const legs[2] = {g_ol, g_or};
    omx_loudness_process(&g_m, legs, n);
    const double m = omx_loudness_momentary(&g_m);
    ok(fabs(m - (double)TGT[k]) < 0.05, "a meter on the output reads the target (0.05 LU)", m, TGT[k]);
    (void)tail;
  }
  clean();
}

static void arm_range(void) {
  g_arm = "C";
  const uint32_t n = (uint32_t)(4.0f * g_sr);
  sine(g_l, n, 997.0, g_sr, 0.01);
  memcpy(g_r, g_l, n * sizeof(float));
  omx_loudlevel_instance_init(&g_v, g_sr);
  omx_loudlevel_instance_resolve(&g_v, 0, -6.0f, 3.0f, 6.0f, -60.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
  vrun(g_l, g_r, g_ol, g_or, n);
  ok(omx_loudlevel_instance_gain_db(&g_v) == 3.0f, "a target past the range stops on the range, exactly", omx_loudlevel_instance_gain_db(&g_v), 3);
  sine(g_l, n, 997.0, g_sr, 0.9);
  memcpy(g_r, g_l, n * sizeof(float));
  omx_loudlevel_instance_init(&g_v, g_sr);
  omx_loudlevel_instance_resolve(&g_v, 0, -40.0f, 3.0f, 6.0f, -60.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
  vrun(g_l, g_r, g_ol, g_or, n);
  ok(omx_loudlevel_instance_gain_db(&g_v) == -3.0f, "and downward", omx_loudlevel_instance_gain_db(&g_v), -3);
  clean();
}

static void arm_speed(void) {
  g_arm = "D";
  const uint32_t n = (uint32_t)(0.6f * g_sr), m = (uint32_t)(1.0f * g_sr);
  sine(g_l, n + m, 997.0, g_sr, 0.01);
  memcpy(g_r, g_l, (n + m) * sizeof(float));
  omx_loudlevel_instance_init(&g_v, g_sr);
  omx_loudlevel_instance_resolve(&g_v, 0, -10.0f, 24.0f, 2.0f, -60.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
  vrun(g_l, g_r, g_ol, g_or, n);
  const double g0 = omx_loudlevel_instance_gain_db(&g_v);
  vrun(g_l + n, g_r + n, g_ol + n, g_or + n, m);
  const double g1 = omx_loudlevel_instance_gain_db(&g_v);
  ok(fabs((g1 - g0) - 2.0) < 0.04, "in the walk the gain moves its speed: 2 dB in a second", g1 - g0, 2.0);
  clean();
}

static void arm_gate(void) {
  g_arm = "E";
  const uint32_t n = (uint32_t)(3.2f * g_sr), early = (uint32_t)(0.39f * g_sr);
  sine(g_l, n, 997.0, g_sr, 0.01);
  memcpy(g_r, g_l, n * sizeof(float));
  omx_loudlevel_instance_init(&g_v, g_sr);
  omx_loudlevel_instance_resolve(&g_v, 0, -10.0f, 24.0f, 6.0f, -60.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
  vrun(g_l, g_r, g_ol, g_or, early);
  ok(memcmp(g_ol, g_l, early * sizeof(float)) == 0, "before the momentary window has filled nothing moves", 1, 0);
  omx_loudlevel_instance_init(&g_v, g_sr);
  omx_loudlevel_instance_resolve(&g_v, 0, -10.0f, 24.0f, 6.0f, -60.0f, (int)OMX_LOUDLEVEL_WINDOWS_SHORT);
  vrun(g_l, g_r, g_ol, g_or, (uint32_t)(2.9f * g_sr));
  ok(omx_loudlevel_instance_gain_db(&g_v) == 0.0f, "before the short window has filled nothing moves", omx_loudlevel_instance_gain_db(&g_v), 0);
  /* silence after a walk: the gain holds once the window has emptied below the gate */
  omx_loudlevel_instance_init(&g_v, g_sr);
  omx_loudlevel_instance_resolve(&g_v, 0, -30.0f, 24.0f, 6.0f, -40.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
  sine(g_l, n, 997.0, g_sr, 0.1);
  memcpy(g_r, g_l, n * sizeof(float));
  vrun(g_l, g_r, g_ol, g_or, n);
  const float held = omx_loudlevel_instance_gain_db(&g_v);
  memset(g_xl, 0, n * sizeof(float));
  /* the window empties over 400 ms: as it falls past the target the gain may rise; from then it stands */
  vrun(g_xl, g_xl, g_ol, g_or, (uint32_t)(0.5f * g_sr));
  const float after = omx_loudlevel_instance_gain_db(&g_v);
  vrun(g_xl, g_xl, g_ol, g_or, n);
  ok(omx_loudlevel_instance_gain_db(&g_v) == after, "under the gate the gain holds", omx_loudlevel_instance_gain_db(&g_v), after);
  ok(omx_loudlevel_instance_loudness_lufs(&g_v) <= -40.0f, "and the detector reads under the gate", omx_loudlevel_instance_loudness_lufs(&g_v), -40);
  (void)held;
  /* the short window reads 3 s: 2 s at A and 1 s at A / 10 is the energy's mean; the momentary one reads the last */
  const double a = 0.1, k = k_power(997.0);
  const uint32_t two = (uint32_t)(2.0f * g_sr), one = (uint32_t)(1.0f * g_sr);
  for (int w = 0; w < 2; w++) {
    omx_loudlevel_instance_init(&g_v, g_sr);
    omx_loudlevel_instance_resolve(&g_v, 0, -30.0f, 24.0f, 1.0f, -70.0f, w);
    sine(g_l, two + one, 997.0, g_sr, a);
    for (uint32_t i = two; i < two + one; i++) g_l[i] = g_l[i] * 0.1f;
    memcpy(g_r, g_l, (two + one) * sizeof(float));
    vrun(g_l, g_r, g_ol, g_or, two + one);
    const double mean = w == (int)OMX_LOUDLEVEL_WINDOWS_SHORT ? (2.0 * a * a + 0.01 * a * a) / 3.0 : 0.01 * a * a;
    const double want = -0.691 + 10.0 * log10(k * mean);
    ok(fabs((double)omx_loudlevel_instance_loudness_lufs(&g_v) - want) < 0.1, "each window reads its own span of the energy (0.1 LU)",
       omx_loudlevel_instance_loudness_lufs(&g_v), want);
  }
  clean();
}

static void arm_cut(void) {
  g_arm = "F";
  static const uint32_t cuts[] = {1u, 64u, 1024u};
  const uint32_t n = (uint32_t)(1.0f * g_sr);
  stimulus(g_l, g_r, n, 0x9999u, 2048u);
  omx_loudlevel_instance_init(&g_v, g_sr);
  omx_loudlevel_instance_resolve(&g_v, 0, -10.0f, 12.0f, 6.0f, -60.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
  vrun(g_l, g_r, g_xl, g_xr, n);
  for (int c = 0; c < 3; c++) {
    omx_loudlevel_instance_init(&g_v, g_sr);
    omx_loudlevel_instance_resolve(&g_v, 0, -10.0f, 12.0f, 6.0f, -60.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
    for (uint32_t o = 0; o < n; o += cuts[c]) {
      omx_loudlevel_instance_run(&g_v, g_l + o, g_r + o, g_ol + o, g_or + o, n - o < cuts[c] ? n - o : cuts[c]);
      if ((o & 4095u) == 0u) drain();
    }
    ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0, "a block cut changes no sample", cuts[c], 0);
  }
  /* the gain is a line between ticks: one sample's ratio on a DC input moves no more than the walk allows */
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = 0.01f;
  omx_loudlevel_instance_init(&g_v, g_sr);
  omx_loudlevel_instance_resolve(&g_v, 0, -10.0f, 24.0f, 6.0f, -70.0f, (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY);
  vrun(g_l, g_r, g_ol, g_or, n);
  double worst = 0.0;
  for (uint32_t i = 1; i < n; i++) worst = fmax(worst, fabs(20.0 * log10((double)g_ol[i] / (double)g_ol[i - 1u])));
  const double allowed = 6.0 * (100.0 / (double)g_sr) * 0.01 * 1.1 + 1e-5;
  ok(worst < allowed, "no step between two samples: the walk per tick spread over the tick", worst, allowed);
  clean();
}

static void arm_hostile(void) {
  g_arm = "G";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  stimulus(g_l, g_r, 512u, 0x77u, 128u);
  g_counting = 1;
  for (int a = 0; a < 6; a++)
    for (int b = 0; b < 6; b++) {
      omx_loudlevel_instance_init(&g_v, g_sr);
      omx_loudlevel_instance_resolve(&g_v, 0, W[a], W[b], W[a], W[b], a * 50 - 1);
      omx_loudlevel_instance_run(&g_v, g_l, g_r, g_ol, g_or, 512u);
      drain();
      ok(g_v.target <= -6.0f && g_v.target >= -40.0f && g_v.range <= 24.0f && g_v.step_db > 0.0f, "hostile words land inside the travels", g_v.target, -6);
    }
  g_counting = 0;
  ok(g_allocs == 0, "nothing allocates", (double)g_allocs, 0);
  clean();
}

OMX_INSTANCE_ORACLE_MAIN("loudlevel_instance", arm_identity(); arm_settle(); arm_range(); arm_speed(); arm_gate(); arm_cut(); arm_hostile();)
