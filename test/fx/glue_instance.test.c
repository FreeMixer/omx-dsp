// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The glue face (fx/omx_glue_instance.h) at each of the nine oracle rates:
//   A  a bypass and a mix of 0 are the input, bit for bit; a rate outside the set is refused;
//   B  each detent is the comp slot with the detent's value, written out here, bit for bit, on noise,
//      the auto release among them: the face mints no DSP and its tables are what they say;
//   C  the settled gain on a held level is the closed form with GLUE_KNEE_DB of knee plus the make-up
//      (1e-3 dB), at each ratio; the readout is that reduction;
//   D  the detector's high-pass: with a corner at 200 Hz, a 40 Hz sine is pulled 2nd-order less, the level
//      the detector hears being -28 dB of the sine's within 1 dB, while a 4 kHz sine pulls as it did (0.1 dB);
//   E  hostile words land inside the travels, the ledger is clean, nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_glue_instance.h>

#include "instance_oracle.h"
#include "instance_no_alloc.h"

#define KNEE 3.0
static const float RATIOS[3] = {2, 4, 10};
static const float ATTACKS[6] = {0.1f, 0.3f, 1, 3, 10, 30};
static const float RELEASES[4] = {100, 300, 600, 1200};
#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxGlueInstance g_g;
static OmxCompInstance g_c;

/* A block cut small and the ledger drained between: at 32, 64 and 128 kHz the slot's poles write records. */
static void grun(OmxGlueInstance *g, const float *l, const float *r, float *ol, float *orr, uint32_t n) {
  drain();
  for (uint32_t o = 0; o < n; o += 16u) {
    omx_glue_instance_run(g, l + o, r + o, ol + o, orr + o, n - o < 16u ? n - o : 16u);
    drain();
  }
}
static void crun(OmxCompInstance *c, const float *l, const float *r, float *ol, float *orr, uint32_t n) {
  drain();
  for (uint32_t o = 0; o < n; o += 16u) {
    omx_comp_instance_run_keyed(c, NULL, NULL, l + o, r + o, ol + o, orr + o, n - o < 16u ? n - o : 16u);
    drain();
  }
}

static double law(double t, double r, double l) {
  const double x = l - t, s = 1.0 / r - 1.0;
  if (fabs(x) < 0.5 * KNEE) return s * (x + 0.5 * KNEE) * (x + 0.5 * KNEE) / (2.0 * KNEE);
  return x <= 0.0 ? 0.0 : s * x;
}

static void arm_identity(void) {
  g_arm = "A";
  OmxGlueInstance x;
  ok(omx_glue_instance_init(&x, 11025.0f) == 0, "a rate outside the oracle set is refused", 1, 0);
  const uint32_t n = 4096u;
  stimulus(g_l, g_r, n, 0x6123u, 512u);
  omx_glue_instance_init(&g_g, g_sr);
  omx_glue_instance_resolve(&g_g, 1, -30.0f, 2, 0, 0, 6.0f, 100.0f, 100.0f);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  drain();
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a bypass is the input, bit for bit", 1, 0);
  omx_glue_instance_resolve(&g_g, 0, -30.0f, 2, 0, 0, 0.0f, 0.0f, 0.0f);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  drain();
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a mix of 0 is the dry bus, bit for bit", 1, 0);
  clean();
}

static void comp_as(float t, float r, float a, float rel, int mode, float mk, float hp, float mix) {
  omx_comp_instance_init(&g_c, g_sr);
  omx_comp_instance_resolve(&g_c, 0, t, r, 3.0f, a, rel, mk, mix, (int)OMX_COMP_KINDS_COMP, (int)OMX_DETECTOR_OVERSAMPLINGS_OFF,
                            (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, (int)OMX_COMP_KEY_SOURCES_SELF, hp, 0.0f, 0.0f,
                            (int)OMX_COMP_KEY_LISTENS_OFF, 0.0f, (int)OMX_COMP_DETECT_LINKS_LINKED, (int)OMX_COMP_DUCKS_OFF,
                            -12.0f, mode);
}

static void arm_detents(void) {
  g_arm = "B";
  const uint32_t n = 8192u;
  stimulus(g_l, g_r, n, 0x6124u, 1024u);
  for (int ra = 0; ra < 3; ra++)
    for (int at = 0; at < 6; at++)
      for (int rl = 0; rl < 5; rl++) {
        const int aut = rl == 4;
        omx_glue_instance_init(&g_g, g_sr);
        omx_glue_instance_resolve(&g_g, 0, -24.0f, ra, at, rl, 2.0f, 120.0f, 80.0f);
        grun(&g_g, g_l, g_r, g_ol, g_or, n);
        comp_as(-24.0f, RATIOS[ra], ATTACKS[at], aut ? RELEASES[0] : RELEASES[rl], aut ? (int)OMX_COMP_RELEASE_MODES_AUTO : (int)OMX_COMP_RELEASE_MODES_FIXED, 2.0f, 120.0f, 80.0f);
        crun(&g_c, g_l, g_r, g_xl, g_xr, n);
        drain();
        ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0,
           "the detent is the comp slot with its written value, bit for bit", ra * 100 + at * 10 + rl, 0);
      }
  clean();
}

static void arm_law(void) {
  g_arm = "C";
  const uint32_t n = (uint32_t)(0.5f * g_sr);
  static const double LS[] = {-40.0, -21.0, -20.0, -18.0, -6.0};
  for (int ra = 0; ra < 3; ra++)
    for (int k = 0; k < 5; k++) {
      const float x = (float)pow(10.0, LS[k] / 20.0);
      for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = x;
      omx_glue_instance_init(&g_g, g_sr);
      omx_glue_instance_resolve(&g_g, 0, -20.0f, ra, 2, 0, 3.0f, 0.0f, 100.0f);
      grun(&g_g, g_l, g_r, g_ol, g_or, n);
      drain();
      const double got = 20.0 * log10((double)g_ol[n - 1u] / (double)x), want = law(-20.0, RATIOS[ra], LS[k]) + 3.0;
      ok(fabs(got - want) < 1e-3, "the settled gain is the closed form plus the make-up (1e-3 dB)", got, want);
      /* the readout is the slot's, make-up included: at a make-up of 0 it reads the reduction */
      omx_glue_instance_init(&g_g, g_sr);
      omx_glue_instance_resolve(&g_g, 0, -20.0f, ra, 2, 0, 0.0f, 0.0f, 100.0f);
      grun(&g_g, g_l, g_r, g_ol, g_or, n);
      grun(&g_g, g_l, g_r, g_ol, g_or, 64u);
      drain();
      ok(fabs((double)omx_glue_instance_gain_reduction_db(&g_g) - (want - 3.0)) < 5e-2, "the readout is the reduction", omx_glue_instance_gain_reduction_db(&g_g), want - 3.0);
    }
  clean();
}

static double pull(double hz, float hp) {
  const uint32_t n = (uint32_t)(1.0f * g_sr), tail = (uint32_t)(0.5f * g_sr);
  sine(g_l, n, hz, g_sr, 0.5);
  memcpy(g_r, g_l, n * sizeof(float));
  omx_glue_instance_init(&g_g, g_sr);
  omx_glue_instance_resolve(&g_g, 0, -40.0f, 1, 5, 3, 0.0f, hp, 100.0f);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  drain();
  return 20.0 * log10(rms(g_ol, n - tail, n) / rms(g_l, n - tail, n));
}

static void arm_key(void) {
  g_arm = "D";
  const double lo_off = pull(40.0, 0.0f), lo_on = pull(40.0, 200.0f), hi_off = pull(4000.0, 0.0f), hi_on = pull(4000.0, 200.0f);
  /* the detector hears the 40 Hz sine 28 dB down, (40/200)^2; on the 4:1 slope the reduction falls by 3/4 of that */
  ok(fabs((lo_on - lo_off) - 0.75 * 28.0) < 1.0, "a corner at 200 Hz keeps a 40 Hz sine off the detector", lo_on - lo_off, 21.0);
  ok(fabs(hi_on - hi_off) < 0.1, "a 4 kHz sine pulls as it did", hi_on - hi_off, 0.1);
  clean();
}

static void arm_hostile(void) {
  g_arm = "E";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  static const int I[] = {-1, 99, 0, 1};
  stimulus(g_l, g_r, 512u, 0x77u, 128u);
  g_counting = 1;
  for (int a = 0; a < 6; a++)
    for (int b = 0; b < 4; b++) {
      omx_glue_instance_init(&g_g, g_sr);
      omx_glue_instance_resolve(&g_g, 0, W[a], I[b], I[b], I[b], W[a], W[a], W[a]);
      grun(&g_g, g_l, g_r, g_ol, g_or, 512u);
      drain();
      uint32_t finite = 1u;
      for (uint32_t i = 0; i < 512u; i++) finite &= g_ol[i] - g_ol[i] == 0.0f && g_or[i] - g_or[i] == 0.0f;
      ok(finite, "hostile words land inside the travels", finite, 1);
    }
  g_counting = 0;
  ok(g_allocs == 0, "nothing allocates", (double)g_allocs, 0);
  clean();
}

OMX_INSTANCE_ORACLE_MAIN("glue_instance", arm_identity(); arm_detents(); arm_law(); arm_key(); arm_hostile();)
