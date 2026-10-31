// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The upward face (fx/omx_upward_instance.h) at each of the nine oracle rates:
//   A  a ratio of 1, a range of 0 and a bypass are the input, bit for bit, on noise; a rate outside the set
//      is refused;
//   B  the settled gain on a held level is the closed form, written out here: (1 - 1/R)(T - L) under the
//      threshold, the quadratic knee UPWARD_KNEE_DB wide across it, capped at the range, 0 above (1e-3 dB)
//      at three thresholds, ratios and ranges over levels from -80 to -6 dB;
//   C  stereo-linked: the detector hears the louder leg, and the silent leg stays silent;
//   D  the times: after a fall from far above the threshold to far under it the gain is the cascade's
//      closed form at the release time, within 0.05 dB after one time constant;
//   E  the readout is the block's rise in dB;
//   F  hostile words land inside the travels, the ledger is clean, nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_upward_instance.h>

#include "instance_oracle.h"
#include "instance_no_alloc.h"

#define KNEE 6.0
#define MAXN 800000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxUpwardInstance g_u;

static double law(double t, double r, double range, double l) {
  const double x = l - t, s = 1.0 - 1.0 / r;
  double g;
  if (fabs(x) < 0.5 * KNEE) g = s * (0.5 * KNEE - x) * (0.5 * KNEE - x) / (2.0 * KNEE);
  else g = x >= 0.0 ? 0.0 : -s * x;
  return g > range ? range : g;
}

static void arm_identity(void) {
  g_arm = "A";
  OmxUpwardInstance x;
  ok(omx_upward_instance_init(&x, 11025.0f) == 0, "a rate outside the oracle set is refused", 1, 0);
  const uint32_t n = 4096u;
  stimulus(g_l, g_r, n, 0x1234u, 512u);
  ok(omx_upward_instance_init(&g_u, g_sr) == 1, "an oracle rate is taken", 0, 1);
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "the come-up values are the input, bit for bit", 1, 0);
  omx_upward_instance_resolve(&g_u, 0, -20.0f, 4.0f, 0.0f, 5.0f, 200.0f);
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "a range of 0 is the input, bit for bit", 1, 0);
  omx_upward_instance_resolve(&g_u, 1, -20.0f, 4.0f, 12.0f, 5.0f, 200.0f);
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "a bypass is the input, bit for bit", 1, 0);
  /* an idle face moves no state: a loud block at a ratio of 1, then engaged, is a fresh slot from silence */
  static OmxUpwardInstance fresh;
  omx_upward_instance_init(&fresh, g_sr);
  omx_upward_instance_resolve(&fresh, 0, -20.0f, 4.0f, 12.0f, 5.0f, 200.0f);
  omx_upward_instance_init(&g_u, g_sr);
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, n);
  omx_upward_instance_resolve(&g_u, 0, -20.0f, 4.0f, 12.0f, 5.0f, 200.0f);
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, n);
  omx_upward_instance_run(&fresh, g_l, g_r, g_xl, g_xr, n);
  ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0, "an idle face moved no state: engaged, it starts from silence", 1, 0);
  clean();
}

static double settled(float t, float r, float range, double l_db, int one_leg) {
  const uint32_t n = (uint32_t)(0.25f * g_sr);
  const float x = (float)pow(10.0, l_db / 20.0);
  for (uint32_t i = 0; i < n; i++) { g_l[i] = x; g_r[i] = one_leg ? 0.0f : x; }
  omx_upward_instance_init(&g_u, g_sr);
  omx_upward_instance_resolve(&g_u, 0, t, r, range, 1.0f, 5.0f);
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, n);
  drain();
  return 20.0 * log10((double)g_ol[n - 1u] / (double)x);
}

static void arm_law(void) {
  g_arm = "B";
  static const float TS[] = {-50.0f, -30.0f, -12.0f}, RS[] = {1.5f, 3.0f, 10.0f}, GS[] = {3.0f, 12.0f, 24.0f};
  static const double LS[] = {-80.0, -60.0, -40.0, -31.0, -30.0, -28.0, -20.0, -6.0};
  double worst = 0.0;
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
      for (int c = 0; c < 3; c++)
        for (int d = 0; d < 8; d++) {
          const double got = settled(TS[a], RS[b], GS[c], LS[d], 0), want = law(TS[a], RS[b], GS[c], LS[d]);
          worst = fmax(worst, fabs(got - want));
          ok(fabs(got - want) < 1e-3, "the settled lift is the closed form (1e-3 dB)", got, want);
        }
  printf("  %6.0f Hz: B worst %.2e dB\n", (double)g_sr, worst);
  clean();
}

static void arm_link(void) {
  g_arm = "C";
  /* the linked detector hears the louder leg, so one leg at L lifts as a pair at L does */
  const double got = settled(-20.0f, 3.0f, 24.0f, -40.0, 1), want = law(-20.0, 3.0, 24.0, -40.0);
  ok(fabs(got - want) < 1e-3, "one leg lifts as the louder leg of a pair", got, want);
  ok(g_or[0] == 0.0f, "the silent leg stays silent", g_or[0], 0);
  /* the detector is RMS: a sine of amplitude A reads A / sqrt 2, 3.01 dB under its peak */
  const double a = 0.01;
  const uint32_t n = (uint32_t)(1.5f * g_sr), tail = (uint32_t)(0.1f * g_sr);
  sine(g_l, n, 1000.0, g_sr, a);
  memcpy(g_r, g_l, n * sizeof(float));
  omx_upward_instance_init(&g_u, g_sr);
  omx_upward_instance_resolve(&g_u, 0, -20.0f, 3.0f, 24.0f, 200.0f, 200.0f);
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, n);
  drain();
  const double lift = 20.0 * log10(rms(g_ol, n - tail, n) / rms(g_l, n - tail, n));
  const double rms_want = law(-20.0, 3.0, 24.0, 20.0 * log10(a / sqrt(2.0)));
  ok(fabs(lift - rms_want) < 0.05, "a sine lifts as its RMS level says (0.05 dB)", lift, rms_want);
  clean();
}

static void arm_times(void) {
  g_arm = "D";
  /* hold loud, fall to quiet: the detector's four one-poles run at the release's pole^4; the lift follows it */
  const float rel = 100.0f;
  const uint32_t n0 = (uint32_t)(0.2f * g_sr), n1 = (uint32_t)(0.1f * g_sr);
  const double hi = 0.5, lo = 1e-3;
  for (uint32_t i = 0; i < n0; i++) g_l[i] = g_r[i] = (float)hi;
  for (uint32_t i = n0; i < n0 + n1; i++) g_l[i] = g_r[i] = (float)lo;
  omx_upward_instance_init(&g_u, g_sr);
  omx_upward_instance_resolve(&g_u, 0, 0.0f, 4.0f, 24.0f, 1.0f, rel);
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, n0 + n1);
  const double rc = exp(-4.0 / ((double)rel * 0.001 * (double)g_sr));
  double s[4] = {hi * hi, hi * hi, hi * hi, hi * hi};
  for (uint32_t i = 0; i < n1; i++) {
    double d = lo * lo;
    for (int k = 0; k < 4; k++) {
      const double c = d > s[k] ? 0.0 : rc;
      s[k] = c * s[k] + (1.0 - c) * d;
      d = s[k];
    }
  }
  const double level_db = 10.0 * log10(s[3]);
  const double want = law(0.0, 4.0, 24.0, level_db);
  const double got = 20.0 * log10((double)g_ol[n0 + n1 - 1u] / lo);
  ok(fabs(got - want) < 0.05, "the lift after one release time is the cascade's closed form", got, want);
  clean();
}

static void arm_readout(void) {
  g_arm = "E";
  const double got = settled(-20.0f, 2.0f, 24.0f, -40.0, 0);
  /* a second block of the held level: the first holds the fall from the detector's silence */
  omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, 64u);
  drain();
  const double read = (double)omx_upward_instance_gain_db(&g_u);
  ok(fabs(read - got) < 5e-3, "the readout is the block's rise, dB", read, got);
  clean();
}

static void arm_hostile(void) {
  g_arm = "F";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  stimulus(g_l, g_r, 512u, 0x77u, 128u);
  g_counting = 1;
  for (int a = 0; a < 6; a++)
    for (int b = 0; b < 6; b++) {
      omx_upward_instance_init(&g_u, g_sr);
      omx_upward_instance_resolve(&g_u, 0, W[a], W[b], W[a], W[b], W[a]);
      omx_upward_instance_run(&g_u, g_l, g_r, g_ol, g_or, 512u);
      drain();
      uint32_t finite = 1u;
      for (uint32_t i = 0; i < 512u; i++) finite &= g_ol[i] - g_ol[i] == 0.0f && g_or[i] - g_or[i] == 0.0f;
      ok(finite && g_u.atom.gc.ratio >= 1.0f && g_u.atom.gc.range_db >= 0.0f, "hostile words land inside the travels", finite, 1);
    }
  g_counting = 0;
  ok(g_allocs == 0, "nothing allocates", (double)g_allocs, 0);
  clean();
}

OMX_INSTANCE_ORACLE_MAIN("upward_instance", arm_identity(); arm_law(); arm_link(); arm_times(); arm_readout(); arm_hostile();)
