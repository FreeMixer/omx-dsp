// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The clipper face (fx/omx_clipper_instance.h) against its closed form, written out here in double, at
// each of the nine oracle rates:
//   A  the come-up values pass a signal under full scale bit for bit, on both legs, aliased or not; a
//      bypass and a block under the ceiling and the knee likewise; a rate outside the set is refused;
//   B  the hard clip: y = clamp(drive x, -c, c) sample for sample, at drives 0, 6, 24 dB and ceilings
//      0, -6, -24 dB, within 4 eps of c;
//   C  the soft clip: y = a + (c - a) d / (d + c - a) above the knee a = c (1 - CLIPPER_KNEE_SPAN s), the
//      straight line below it, odd, monotonic, never past the ceiling, and leaving the line at slope 1;
//   D  the readout is 20 log10(y / u) of the block's loudest sample, 0 under the knee;
//   E  the legs do not touch: a clip on one leaves the other bit for bit;
//   F  hostile words land inside the travels, the ledger is clean, nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_clipper_instance.h>

#include "instance_oracle.h"
#include "instance_no_alloc.h"

#include <float.h>

#define MAXN 4096u
#define SPAN 0.5
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN];
static OmxClipperInstance g_c;

static double law(double u, double c, double a) {
  const double m = fabs(u);
  if (m <= a) return u;
  const double room = c - a, d = m - a;
  const double y = room > 0.0 ? a + room * d / (d + room) : c;
  return u < 0.0 ? -y : y;
}

/* a ramp from -2 to 2 on the left, the same read backward on the right */
static void sweep(uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    g_l[i] = -2.0f + 4.0f * (float)i / (float)(n - 1u);
    g_r[n - 1u - i] = g_l[i];
  }
}

static void arm_identity(void) {
  g_arm = "A";
  OmxClipperInstance x;
  ok(omx_clipper_instance_init(&x, 22050.0f) == 0, "a rate outside the oracle set is refused", 1, 0);
  ok(omx_clipper_instance_init(&g_c, g_sr) == 1, "an oracle rate is taken", 0, 1);
  const uint32_t n = 1024u;
  sine(g_l, n, 997.0, g_sr, 0.9);
  sine(g_r, n, 1499.0, g_sr, 1.0);
  omx_clipper_instance_run(&g_c, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0,
     "at its come-up values a signal under full scale is itself, bit for bit", 1, 0);
  memcpy(g_ol, g_l, n * sizeof(float));
  memcpy(g_or, g_r, n * sizeof(float));
  omx_clipper_instance_run(&g_c, g_ol, g_or, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "aliased buffers likewise", 1, 0);
  omx_clipper_instance_resolve(&g_c, 1, 24.0f, -24.0f, 100.0f);
  omx_clipper_instance_run(&g_c, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a bypass is the input, bit for bit", 1, 0);
  clean();
}

static void check_law(float drive_db, float ceil_db, float soft, const char *what) {
  const uint32_t n = 2048u;
  sweep(n);
  omx_clipper_instance_init(&g_c, g_sr);
  omx_clipper_instance_resolve(&g_c, 0, drive_db, ceil_db, soft);
  omx_clipper_instance_run(&g_c, g_l, g_r, g_ol, g_or, n);
  const double g = pow(10.0, (double)drive_db / 20.0), c = pow(10.0, (double)ceil_db / 20.0);
  const double a = c * (1.0 - SPAN * (double)soft * 0.01);
  uint32_t bad = 0u, past = 0u, down = 0u;
  for (uint32_t i = 0; i < n; i++) {
    bad += fabs((double)g_ol[i] - law(g * (double)g_l[i], c, a)) > 8.0 * FLT_EPSILON * fmax(c, 1.0) * (soft > 0.0f ? 4.0 : 1.0);
    bad += fabs((double)g_or[i] - law(g * (double)g_r[i], c, a)) > 8.0 * FLT_EPSILON * fmax(c, 1.0) * (soft > 0.0f ? 4.0 : 1.0);
    past += fabs((double)g_ol[i]) > c * (1.0 + 4.0 * FLT_EPSILON);
    if (i > 0u) down += g_ol[i] + 8.0f * FLT_EPSILON < g_ol[i - 1u];
  }
  ok(bad == 0u, what, bad, 0);
  ok(past == 0u, "no sample past the ceiling", past, 0);
  ok(down == 0u, "the curve never turns back", down, 0);
}

static void arm_hard(void) {
  g_arm = "B";
  static const float dr[] = {0.0f, 6.0f, 24.0f}, ce[] = {0.0f, -6.0f, -24.0f};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) check_law(dr[i], ce[j], 0.0f, "the hard clip is clamp(drive x, -c, c), sample for sample");
  clean();
}

static void arm_soft(void) {
  g_arm = "C";
  static const float dr[] = {0.0f, 12.0f}, ce[] = {0.0f, -12.0f}, so[] = {25.0f, 50.0f, 100.0f};
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++)
      for (int k = 0; k < 3; k++) check_law(dr[i], ce[j], so[k], "the soft clip is the hyperbola over the knee, sample for sample");
  /* the slope at the knee is 1: a step just over it moves the output by the same step */
  const double a = 0.5, c = 1.0, e = 1e-6;
  const double y = law(a + e, c, a);
  ok(fabs((y - a) / e - 1.0) < 1e-5, "the law leaves the line at slope 1", (y - a) / e, 1.0);
  const float yf = omx_clipper_sample(0.5f + 1e-3f, 1.0f, 0.5f);
  ok(fabs((double)yf - law(0.5 + 1e-3, 1.0, 0.5)) < 4.0 * FLT_EPSILON, "the word agrees just over the knee", yf, 0.501);
  clean();
}

static void arm_readout(void) {
  g_arm = "D";
  omx_clipper_instance_init(&g_c, g_sr);
  sine(g_l, 1024u, 997.0, g_sr, 0.5);
  memcpy(g_r, g_l, 1024u * sizeof(float));
  omx_clipper_instance_run(&g_c, g_l, g_r, g_ol, g_or, 1024u);
  ok(omx_clipper_instance_gain_reduction_db(&g_c) == 0.0f, "under the knee the readout is 0", omx_clipper_instance_gain_reduction_db(&g_c), 0);
  omx_clipper_instance_resolve(&g_c, 0, 0.0f, -6.0f, 0.0f);
  g_l[100] = 1.0f;
  omx_clipper_instance_run(&g_c, g_l, g_r, g_ol, g_or, 1024u);
  const double want = -20.0 * log10(1.0 / pow(10.0, -6.0 / 20.0));
  ok(fabs((double)omx_clipper_instance_gain_reduction_db(&g_c) - want) < 2e-3, "1.0 under a -6 dB ceiling reads -6 dB",
     omx_clipper_instance_gain_reduction_db(&g_c), want);
  clean();
}

static void arm_legs(void) {
  g_arm = "E";
  const uint32_t n = 1024u;
  sine(g_l, n, 997.0, g_sr, 2.0);
  sine(g_r, n, 1499.0, g_sr, 0.25);
  omx_clipper_instance_init(&g_c, g_sr);
  omx_clipper_instance_resolve(&g_c, 0, 0.0f, -6.0f, 0.0f);
  omx_clipper_instance_run(&g_c, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_or, g_r, n * sizeof(float)) == 0, "a clip on one leg leaves the other bit for bit", 1, 0);
  clean();
}

static void arm_hostile(void) {
  g_arm = "F";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  stimulus(g_l, g_r, 512u, 0x77u, 128u);
  g_counting = 1;
  for (int a = 0; a < 6; a++)
    for (int b = 0; b < 6; b++)
      for (int c = 0; c < 6; c++) {
        omx_clipper_instance_init(&g_c, g_sr);
        omx_clipper_instance_resolve(&g_c, 0, W[a], W[b], W[c]);
        omx_clipper_instance_run(&g_c, g_l, g_r, g_ol, g_or, 512u);
        ok(g_c.ceiling_lin > 0.0f && g_c.ceiling_lin <= 1.0f + 1e-6f && g_c.knee_lin > 0.0f && g_c.drive_lin >= 1.0f - 1e-6f,
           "hostile words land inside the travels", g_c.ceiling_lin, 1);
      }
  g_counting = 0;
  ok(g_allocs == 0, "nothing allocates", (double)g_allocs, 0);
  clean();
}

OMX_INSTANCE_ORACLE_MAIN("clipper_instance", arm_identity(); arm_hard(); arm_soft(); arm_readout(); arm_legs(); arm_hostile();)
