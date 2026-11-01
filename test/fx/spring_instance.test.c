// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The spring (fx/omx_spring_instance.h) at each of the nine oracle rates:
//   A  a mix of 0 and a bypass are the input, bit for bit; a rate outside the set is refused;
//   B  the first return of an impulse: the left leg's wet starts at length x sr / 1000 samples, the right
//      at SPRING_RIGHT_RATIO of it, none sooner;
//   C  the dispersion, read off the wet's first return by a DFT's phase around f, is N (1 - a^2) /
//      (1 + 2 a cos w + a^2) samples at 200 Hz, 1 and 3 kHz, within 1 % , and in ms the same at every rate;
//      the corner reads the tension, written out here (1200 to 3600 Hz);
//   D  the decay: the energy of a low burst falls at 60 dB per decayS, within 3 dB over a measured span;
//      at any setting a long run of loud noise stays bounded (the loop under SPRING_LOOP_MAX);
//   E  one run cut into blocks of 1, 64, 1024 is the same output, bit for bit;
//   F  the engage edge and a length change clear the tank: an old tail does not come back;
//   G  hostile words land inside the travels, the ledger is clean, nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_spring_instance.h>

#include "instance_oracle.h"
#include "instance_no_alloc.h"

#include <complex.h>
#include <float.h>

#define N_SECTIONS 40.0
#define RATIO 1.13
#define MAXN 2000000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxSpringInstance g_s;

static void srun(const float *l, const float *r, float *ol, float *orr, uint32_t n) {
  /* the ledger drains between short runs: at 32, 64 and 128 kHz nothing is written, elsewhere nothing either */
  for (uint32_t o = 0; o < n; o += 4096u) {
    omx_spring_instance_run(&g_s, l + o, r + o, ol + o, orr + o, n - o < 4096u ? n - o : 4096u);
    drain();
  }
}

static void arm_identity(void) {
  g_arm = "A";
  ok(omx_spring_instance_init(&g_s, 11025.0f) == 0, "a rate outside the oracle set is refused", 1, 0);
  const uint32_t n = 4096u;
  stimulus(g_l, g_r, n, 0x1111u, 512u);
  omx_spring_instance_init(&g_s, g_sr);
  srun(g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a mix of 0, the come-up value, is the input", 1, 0);
  omx_spring_instance_resolve(&g_s, 1, 2.0f, 35.0f, 50.0f, 4000.0f, 100.0f);
  srun(g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "a bypass is the input", 1, 0);
  clean();
}

static void arm_return(void) {
  g_arm = "B";
  static const float LEN[] = {20.0f, 35.0f, 60.0f};
  for (int k = 0; k < 3; k++) {
    const uint32_t n = (uint32_t)(0.1f * g_sr);
    memset(g_l, 0, n * sizeof(float));
    memset(g_r, 0, n * sizeof(float));
    g_l[0] = g_r[0] = 1.0f;
    omx_spring_instance_init(&g_s, g_sr);
    omx_spring_instance_resolve(&g_s, 0, 2.0f, LEN[k], 50.0f, 10000.0f, 100.0f);
    srun(g_l, g_r, g_ol, g_or, n);
    const uint32_t dl = (uint32_t)lrint((double)LEN[k] * 0.001 * (double)g_sr), dr = (uint32_t)lrint((double)LEN[k] * RATIO * 0.001 * (double)g_sr);
    uint32_t fl = n, fr = n;
    for (uint32_t i = 0; i < n; i++) {
      if (g_ol[i] != 0.0f && fl == n) fl = i;
      if (g_or[i] != 0.0f && fr == n) fr = i;
    }
    ok(fl == dl, "the left leg's first return is at its length", fl, dl);
    ok(fr == dr, "the right leg's first return is at the ratio's length", fr, dr);
  }
  clean();
}

/* the group delay at hz of x[from..from+n), by the DFT's phase across +-delta */
static double phase_delay(const float *x, uint32_t from, uint32_t n, double hz) {
  const double d = 5.0, w0 = 2.0 * M_PI * (hz - d) / (double)g_sr, w1 = 2.0 * M_PI * (hz + d) / (double)g_sr;
  double complex a = 0.0, b = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    const double t = (double)i;
    a += (double)x[from + i] * cexp(-I * w0 * t);
    b += (double)x[from + i] * cexp(-I * w1 * t);
  }
  return -carg(b * conj(a)) / (w1 - w0);
}

static void arm_dispersion(void) {
  g_arm = "C";
  static const double F[] = {200.0, 1000.0, 3000.0};
  static const float TN[] = {0.0f, 50.0f, 100.0f};
  for (int t = 0; t < 3; t++) {
    const double fc = 1200.0 + (3600.0 - 1200.0) * (double)TN[t] * 0.01;
    const double tn = tan(M_PI * fc / (double)g_sr), a = (tn - 1.0) / (tn + 1.0);
    /* a spring as long as the chain's whole smear would overlap its second trip: decay short, read the first */
    const uint32_t d = (uint32_t)lrint(0.06 * (double)g_sr), n = d - 1u;
    memset(g_l, 0, (d + n) * sizeof(float));
    memset(g_r, 0, (d + n) * sizeof(float));
    g_l[0] = 1.0f;
    omx_spring_instance_init(&g_s, g_sr);
    omx_spring_instance_resolve(&g_s, 0, 0.5f, 60.0f, TN[t], 10000.0f, 100.0f);
    srun(g_l, g_r, g_ol, g_or, d + n);
    for (int f = 0; f < 3; f++) {
      const double w = 2.0 * M_PI * F[f] / (double)g_sr;
      const double want = N_SECTIONS * (1.0 - a * a) / (1.0 + 2.0 * a * cos(w) + a * a);
      const double got = phase_delay(g_ol, d, n, F[f]);
      ok(fabs(got - want) < 0.01 * want + 1.0, "the chain delays each note by its closed form (1 %)", got, want);
    }
  }
  clean();
}

static void arm_decay(void) {
  g_arm = "D";
  const float decay = 2.0f;
  const uint32_t burst = (uint32_t)(0.02f * g_sr), n = (uint32_t)(1.6f * g_sr), w = (uint32_t)(0.2f * g_sr);
  sine(g_l, burst, 300.0, g_sr, 0.5);
  memset(g_l + burst, 0, (n - burst) * sizeof(float));
  memcpy(g_r, g_l, n * sizeof(float));
  omx_spring_instance_init(&g_s, g_sr);
  omx_spring_instance_resolve(&g_s, 0, decay, 35.0f, 50.0f, 10000.0f, 100.0f);
  srun(g_l, g_r, g_ol, g_or, n);
  const uint32_t t0 = (uint32_t)(0.4f * g_sr), t1 = (uint32_t)(1.2f * g_sr);
  const double e0 = 20.0 * log10(rms(g_ol, t0, t0 + w)), e1 = 20.0 * log10(rms(g_ol, t1, t1 + w));
  const double want = -60.0 * (double)(t1 - t0) / (double)g_sr / (double)decay;
  ok(fabs((e1 - e0) - want) < 3.0, "the tail falls at 60 dB per decayS (3 dB)", e1 - e0, want);
  /* bounded: a long loud run at the longest decay */
  stimulus(g_l, g_r, n, 0x2222u, 0u);
  omx_spring_instance_init(&g_s, g_sr);
  omx_spring_instance_resolve(&g_s, 0, 8.0f, 20.0f, 0.0f, 10000.0f, 100.0f);
  srun(g_l, g_r, g_ol, g_or, n);
  double peak = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    const double v = fmax(fabs((double)g_ol[i]), fabs((double)g_or[i]));
    if (v > peak) peak = v;
  }
  const double bound = 0.75 / (1.0 - 0.97) * N_SECTIONS;
  ok(peak < bound && peak - peak == 0.0, "a long loud run at the longest decay stays bounded", peak, bound);
  clean();
}

static void arm_cut(void) {
  g_arm = "E";
  static const uint32_t cuts[] = {1u, 64u, 1024u};
  const uint32_t n = (uint32_t)(0.2f * g_sr);
  stimulus(g_l, g_r, n, 0x3333u, 2048u);
  omx_spring_instance_init(&g_s, g_sr);
  omx_spring_instance_resolve(&g_s, 0, 2.0f, 35.0f, 50.0f, 4000.0f, 40.0f);
  srun(g_l, g_r, g_xl, g_xr, n);
  for (int c = 0; c < 3; c++) {
    omx_spring_instance_init(&g_s, g_sr);
    omx_spring_instance_resolve(&g_s, 0, 2.0f, 35.0f, 50.0f, 4000.0f, 40.0f);
    for (uint32_t o = 0; o < n; o += cuts[c]) omx_spring_instance_run(&g_s, g_l + o, g_r + o, g_ol + o, g_or + o, n - o < cuts[c] ? n - o : cuts[c]);
    drain();
    ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0, "a block cut changes no sample", cuts[c], 0);
  }
  clean();
}

static void arm_clear(void) {
  g_arm = "F";
  const uint32_t n = (uint32_t)(0.2f * g_sr);
  stimulus(g_l, g_r, n, 0x4444u, 0u);
  memset(g_xl, 0, n * sizeof(float));
  omx_spring_instance_init(&g_s, g_sr);
  omx_spring_instance_resolve(&g_s, 0, 4.0f, 35.0f, 50.0f, 4000.0f, 100.0f);
  srun(g_l, g_r, g_ol, g_or, n);
  omx_spring_instance_resolve(&g_s, 1, 4.0f, 35.0f, 50.0f, 4000.0f, 100.0f);
  omx_spring_instance_resolve(&g_s, 0, 4.0f, 35.0f, 50.0f, 4000.0f, 100.0f);
  srun(g_xl, g_xl, g_ol, g_or, n);
  uint32_t back = 0u;
  for (uint32_t i = 0; i < n; i++) back += g_ol[i] != 0.0f || g_or[i] != 0.0f;
  ok(back == 0u, "after a bypass the old tail does not come back", back, 0);
  srun(g_l, g_r, g_ol, g_or, n);
  omx_spring_instance_resolve(&g_s, 0, 4.0f, 50.0f, 50.0f, 4000.0f, 100.0f);
  srun(g_xl, g_xl, g_ol, g_or, n);
  back = 0u;
  for (uint32_t i = 0; i < n; i++) back += g_ol[i] != 0.0f;
  ok(back == 0u, "after a length change the old tail does not come back", back, 0);
  /* a mix of 0 moves no state: noise at 0, then engaged over silence, is silence */
  omx_spring_instance_init(&g_s, g_sr);
  srun(g_l, g_r, g_ol, g_or, n);
  omx_spring_instance_resolve(&g_s, 0, (float)OMX_SPRING_DECAY_S_RANGE_DEFAULT, (float)OMX_SPRING_LENGTH_MS_RANGE_DEFAULT,
                              (float)OMX_SPRING_TENSION_PCT_RANGE_DEFAULT, (float)OMX_SPRING_DAMP_HZ_RANGE_DEFAULT, 100.0f);
  srun(g_xl, g_xl, g_ol, g_or, n);
  back = 0u;
  for (uint32_t i = 0; i < n; i++) back += g_ol[i] != 0.0f;
  ok(back == 0u, "a mix of 0 wrote nothing into the tank", back, 0);
  clean();
}

static void arm_hostile(void) {
  g_arm = "G";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  stimulus(g_l, g_r, 512u, 0x77u, 128u);
  g_counting = 1;
  for (int a = 0; a < 6; a++)
    for (int b = 0; b < 6; b++) {
      omx_spring_instance_init(&g_s, g_sr);
      omx_spring_instance_resolve(&g_s, 0, W[a], W[b], W[a], W[b], W[a]);
      omx_spring_instance_run(&g_s, g_l, g_r, g_ol, g_or, 512u);
      drain();
      ok(g_s.leg[0].g < 1.0f && g_s.leg[1].d < OMX_SPRING_INSTANCE_RING && g_s.wet <= 1.0f, "hostile words land inside the travels", g_s.leg[0].g, 1);
    }
  g_counting = 0;
  ok(g_allocs == 0, "nothing allocates", (double)g_allocs, 0);
  clean();
}

OMX_INSTANCE_ORACLE_MAIN("spring_instance", arm_identity(); arm_return(); arm_dispersion(); arm_decay(); arm_cut(); arm_clear(); arm_hostile();)
