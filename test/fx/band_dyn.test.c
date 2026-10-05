// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file band_dyn.test.c
 * @brief The band-dynamics word's oracle, docs/design/specs/2026-09-26-native-dynamic-eq.md §5
 *        O1–O9, at every declared rate (O10 is the de-esser's own battery, out of scope here).
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_band_dyn.h>
#include <omxdsp/omx_eq_design.h>

#include "fx_rates.h"

/** @brief The shared band-dynamics knee, dB — `BAND_DYN_KNEE_DB`, spec §3b. */
#define KNEE_DB 6.0f
/** @brief Where the 4-stage peak cascade settles on a 1 kHz sine, dB under its peak (measured −0.742 at six rates). */
#define PEAK_SETTLE_DB (-0.742)
/** @brief Samples held in one scratch leg: 2 s at the highest declared rate. */
#define MAXN 400000u

static int g_fail = 0, g_checks = 0;
static float g_x[MAXN], g_y[MAXN], g_u[MAXN];

static void check(int cond, const char *what, double sr) {
  g_checks++;
  if (!cond) {
    g_fail++;
    printf("  FAIL @ %.0f Hz: %s\n", sr, what);
  }
}

static double db(double x) { return 20.0 * log10(x < 1e-30 ? 1e-30 : x); }

/** @brief Goertzel magnitude of `x` at `f` over `n` samples (an integer number of periods). */
static double tone_mag(const float *x, uint32_t n, double f, double sr) {
  const double w = 2.0 * M_PI * f / sr;
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    re += x[i] * cos(w * i);
    im -= x[i] * sin(w * i);
  }
  return 2.0 * sqrt(re * re + im * im) / n;
}

/** @brief A section's complex response at angular frequency `w`. */
static void section_at(const float c[5], double w, double *re, double *im) {
  const double cr1 = cos(w), ci1 = -sin(w), cr2 = cos(2 * w), ci2 = -sin(2 * w);
  const double nr = c[0] + c[1] * cr1 + c[2] * cr2, ni = c[1] * ci1 + c[2] * ci2;
  const double dr = 1.0 + c[3] * cr1 + c[4] * cr2, di = c[3] * ci1 + c[4] * ci2;
  const double den = dr * dr + di * di;
  *re = (nr * dr + ni * di) / den;
  *im = (ni * dr - nr * di) / den;
}

/** @brief `|1 + (g − 1)·B(f)|` in dB — the held-gain closed form, §1. */
static double held_db(const float c[5], double g, double f, double sr) {
  double re, im;
  section_at(c, 2.0 * M_PI * f / sr, &re, &im);
  const double r = 1.0 + (g - 1.0) * re, i = (g - 1.0) * im;
  return db(sqrt(r * r + i * i));
}

enum b_kind { B_BELL, B_LOW, B_HIGH, B_SABOTAGE_LP };

/** @brief Design `B`: the bell's bandpass or a shelf half `½(1 ± A₁)`; the sabotage is the cookbook lowpass. */
static void design_b(enum b_kind k, double f, double q, double sr, float c[5]) {
  if (k == B_BELL) {
    omx_eq_design_f(OMX_EQ_BANDPASS, f, q, 0.0, sr, c);
    return;
  }
  if (k == B_SABOTAGE_LP) {
    omx_eq_design_f(OMX_EQ_LOWPASS, f, M_SQRT1_2, 0.0, sr, c);
    return;
  }
  double a[5];
  omx_eq_design(OMX_EQ_ALLPASS1, f, q, 0.0, sr, a);
  const double h = k == B_LOW ? 0.5 * (1.0 + a[0]) : 0.5 * (1.0 - a[0]);
  c[0] = (float)h;
  c[1] = (float)(k == B_LOW ? h : -h);
  c[2] = 0.0f;
  c[3] = (float)a[0];
  c[4] = 0.0f;
}

static struct omx_band_dyn atom(double sr, int mode, float thr, float ratio, float range, float att, float rel,
                                const float c[5]) {
  struct omx_band_dyn p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.dyn.enabled = 1;
  p.dyn.gc.mode = mode;
  p.dyn.gc.thresh_db = thr;
  p.dyn.gc.ratio = ratio;
  p.dyn.gc.knee_db = KNEE_DB;
  p.dyn.gc.range_db = range;
  p.dyn.gc.makeup_lin = 1.0f;
  p.dyn.detect = OMX_DETECT_PEAK;
  p.dyn.ovs_mode = OMX_DYN_OVS_OFF;
  p.dyn.attack_ms = att;
  p.dyn.attack_coeff = omx_pole_from_time_ms(att, (float)sr);
  p.dyn.release_coeff = omx_pole_from_time_ms(rel, (float)sr);
  memcpy(p.b_c, c, sizeof p.b_c);
  return p;
}

static uint32_t g_seed = 1u;
static float noise(void) {
  g_seed = g_seed * 1664525u + 1013904223u;
  return (float)((int32_t)g_seed) / 2147483648.0f;
}

/** @brief The closed-form characteristic in double: `sign(range)·min(|gr|, |range|)`. */
static double closed_d(int mode, double thr, double ratio, double range, double level_db) {
  const double x = level_db - thr, h = 0.5 * KNEE_DB;
  double gr;
  if (mode == OMX_DYN_ABOVE)
    gr = x <= -h ? 0.0 : (x < h ? (1.0 / ratio - 1.0) * (x + h) * (x + h) / (2.0 * KNEE_DB) : (1.0 / ratio - 1.0) * x);
  else
    gr = x >= h ? 0.0 : (x > -h ? (1.0 - ratio) * (x - h) * (x - h) / (2.0 * KNEE_DB) : (ratio - 1.0) * x);
  const double m = fabs(range);
  const double a = fabs(gr) < m ? fabs(gr) : m;
  return range > 0.0 ? a : -a;
}

/** O1 — rest is the static band, bit for bit. O2 — off touches no sample and no state word. */
static void o1_o2(double sr) {
  const uint32_t n = (uint32_t)sr;
  float stat[5], bc[5], st_l[4] = {0};
  omx_eq_design_f(OMX_EQ_PEAKING, 1000.0, 2.0, 4.0, sr, stat);
  design_b(B_BELL, 1000.0, 2.0, sr, bc);
  g_seed = 7u;
  for (uint32_t i = 0; i < n; i++) g_u[i] = omx_biquad(0.01f * noise(), stat, st_l);
  memcpy(g_y, g_u, n * sizeof(float));
  struct omx_band_dyn p = atom(sr, OMX_DYN_ABOVE, 0.0f, 2.0f, -6.0f, 5.0f, 100.0f, bc);
  struct omx_band_dyn_state st;
  omx_band_dyn_state_init(&st);
  for (uint32_t i = 0; i < n; i += 256u) omx_band_dyn_process(g_y + i, NULL, n - i < 256u ? n - i : 256u, &p, &st);
  check(memcmp(g_y, g_u, n * sizeof(float)) == 0, "O1 rest: output memcmp the static band", sr);
  struct omx_band_dyn_state zero;
  omx_band_dyn_state_init(&zero);
  omx_band_dyn_state_init(&st);
  p.enabled = 0;
  memcpy(g_y, g_u, n * sizeof(float));
  omx_band_dyn_process(g_y, g_x, n, &p, &st);
  check(memcmp(g_y, g_u, n * sizeof(float)) == 0 && memcmp(&st, &zero, sizeof st) == 0,
        "O2 off: output and every state word unmoved", sr);
}

/** O3 — the static curve, both modes, cut and boost: returns the worst deviation for the spread. */
static void o3(double sr, double out[4][16]) {
  static const struct { int mode; float range; } CFG[4] = {
      {OMX_DYN_ABOVE, -6.0f}, {OMX_DYN_ABOVE, 6.0f}, {OMX_DYN_BELOW, -6.0f}, {OMX_DYN_BELOW, 6.0f}};
  float bc[5];
  design_b(B_BELL, 1000.0, 2.0, sr, bc);
  const uint32_t n = (uint32_t)(sr * 0.6);
  for (int c = 0; c < 4; c++) {
    for (int li = 0; li < 16; li++) {
      const double lvl_db = -60.0 + 4.0 * li;
      const double a = pow(10.0, lvl_db / 20.0);
      for (uint32_t i = 0; i < n; i++) g_y[i] = (float)(a * sin(2.0 * M_PI * 1000.0 * i / sr));
      struct omx_band_dyn p = atom(sr, CFG[c].mode, -30.0f, 2.0f, CFG[c].range, 5.0f, 100.0f, bc);
      struct omx_band_dyn_state st;
      omx_band_dyn_state_init(&st);
      omx_band_dyn_process(g_y, NULL, n, &p, &st);
      const struct omx_band_dyn_block bk = omx_band_dyn_begin(&p.dyn);
      const float level = omx_env_level(&st.env, &bk.ep);
      const double d = omx_band_dyn_offset_db(&p.dyn.gc, level);
      const double want_exact = closed_d(CFG[c].mode, -30.0, 2.0, CFG[c].range, db(level));
      check(fabs(d - want_exact) <= 0.01, "O3 the characteristic on the measured level", sr);
      check(fabs(db(level) - lvl_db - PEAK_SETTLE_DB) <= 0.1, "O3 the peak detector settles at its pinned offset", sr);
      check(CFG[c].range < 0 ? (d <= 0.0 && d >= CFG[c].range) : (d >= 0.0 && d <= CFG[c].range),
            "O3/L5 the offset is signed and bounded by range", sr);
      out[c][li] = d;
    }
  }
}

/** O4 — the held-gain response and the bound, g pinned by BELOW mode on a −60 dBFS probe. */
static double o4(double sr, enum b_kind k, double f0, float range, int assert_bound) {
  float bc[5];
  design_b(k, f0, 2.0, sr, bc);
  const double g = pow(10.0, range / 20.0);
  const uint32_t n = (uint32_t)sr;
  double worst_over = -1e9;
  for (double f = 20.0; f <= fmin(20000.0, 0.45 * sr); f *= 1.25) {
    const double fi = round(f);
    for (uint32_t i = 0; i < n; i++) g_y[i] = g_x[i] = (float)(0.001 * sin(2.0 * M_PI * fi * i / sr));
    struct omx_band_dyn p = atom(sr, OMX_DYN_BELOW, 0.0f, 2.0f, range, 5.0f, 100.0f, bc);
    struct omx_band_dyn_state st;
    omx_band_dyn_state_init(&st);
    omx_band_dyn_process(g_y, NULL, n, &p, &st);
    const uint32_t skip = n / 2u;
    const double got = db(tone_mag(g_y + skip, n - skip, fi, sr) / tone_mag(g_x + skip, n - skip, fi, sr));
    const double want = held_db(bc, g, fi, sr);
    const double over = got - (range > 0 ? range : 0.0);
    if (over > worst_over) worst_over = over;
    if (assert_bound) {
      check(fabs(got - want) <= 0.02, "O4 held gain matches |1 + (g-1)B(f)| within 0.02 dB", sr);
      check(over <= 0.01, range > 0 ? "O4 boost never exceeds rangeDb" : "O4 cut never adds gain", sr);
    }
  }
  return worst_over;
}

/** O5/O6 — attack and release constants: Erlang-4 at t = τ, 56.65 % up and 43.35 % left. */
static void o5_o6(double sr, double *att_ms, double *rel_ms) {
  float bc[5];
  design_b(B_LOW, 5000.0, 0.7, sr, bc);
  struct omx_band_dyn p = atom(sr, OMX_DYN_ABOVE, -30.0f, 2.0f, -6.0f, 5.0f, 100.0f, bc);
  struct omx_band_dyn_state st;
  omx_band_dyn_state_init(&st);
  const struct omx_band_dyn_block bk = omx_band_dyn_begin(&p.dyn);
  const uint32_t n = (uint32_t)(sr * 1.5);
  const float hi = 0.316227766f;
  double t_up = -1.0, t_dn = -1.0;
  for (uint32_t i = 0; i < n; i++) {
    float x = hi;
    omx_band_dyn_process(&x, NULL, 1u, &p, &st);
    if (t_up < 0 && omx_env_level(&st.env, &bk.ep) >= 0.566530f * hi) t_up = 1000.0 * i / sr;
  }
  const float top = omx_env_level(&st.env, &bk.ep);
  for (uint32_t i = 0; i < n; i++) {
    float x = 0.0f;
    omx_band_dyn_process(&x, NULL, 1u, &p, &st);
    if (t_dn < 0 && omx_env_level(&st.env, &bk.ep) <= 0.433470f * top) t_dn = 1000.0 * i / sr;
  }
  check(fabs(t_up - 5.0) <= 0.25, "O5 attack reaches 56.65 % at attackMs (±5 %)", sr);
  check(fabs(t_dn - 100.0) <= 5.0, "O6 release falls to 43.35 % at releaseMs (±5 %)", sr);
  *att_ms = t_up;
  *rel_ms = t_dn;
}

/** O7 — zero latency: an engaged band answers an impulse at index 0. */
static void o7(double sr) {
  float bc[5];
  design_b(B_BELL, 1000.0, 2.0, sr, bc);
  struct omx_band_dyn p = atom(sr, OMX_DYN_BELOW, 0.0f, 2.0f, -6.0f, 5.0f, 100.0f, bc);
  struct omx_band_dyn_state st;
  omx_band_dyn_state_init(&st);
  float x[64] = {0};
  x[0] = 1.0f;
  omx_band_dyn_process(x, NULL, 64u, &p, &st);
  check(x[0] != 0.0f && omx_band_dyn_latency(&p) == 0, "O7 first non-zero output at index 0", sr);
}

/** O8 — no denormal state after a 0 dBFS burst and 2 s of zeros. */
static void o8(double sr) {
  float bc[5];
  design_b(B_HIGH, 3000.0, 0.7, sr, bc);
  struct omx_band_dyn p = atom(sr, OMX_DYN_ABOVE, -30.0f, 2.0f, -12.0f, 5.0f, 100.0f, bc);
  struct omx_band_dyn_state st;
  omx_band_dyn_state_init(&st);
  const uint32_t burst = (uint32_t)(sr * 0.1), quiet = (uint32_t)(sr * 2.0);
  g_seed = 3u;
  for (uint32_t i = 0; i < burst; i++) g_y[i] = noise();
  for (uint32_t i = 0; i < quiet; i++) g_x[i] = 0.0f;
  omx_band_dyn_process(g_y, NULL, burst, &p, &st);
  for (uint32_t i = 0; i < quiet; i += 256u) omx_band_dyn_process(g_x + i, NULL, quiet - i < 256u ? quiet - i : 256u, &p, &st);
  int ok = 1;
  const float *w = (const float *)&st;
  for (size_t k = 0; k < sizeof st / sizeof(float); k++) ok &= w[k] == 0.0f || fabsf(w[k]) >= FLT_MIN;
  check(ok, "O8 every state word is zero or normal", sr);
}

int main(void) {
  omx_fx_require_rate_floor();
  double curve[OMX_DECLARED_RATE_COUNT][4][16], att[OMX_DECLARED_RATE_COUNT], rel[OMX_DECLARED_RATE_COUNT];
  for (unsigned r = 0; r < OMX_DECLARED_RATE_COUNT; r++) {
    const double sr = OMX_DECLARED_RATES[r];
    o1_o2(sr);
    o3(sr, curve[r]);
    const double cut = fmax(o4(sr, B_BELL, 1000.0, -12.0f, 1), fmax(o4(sr, B_LOW, 200.0, -12.0f, 1), o4(sr, B_HIGH, 8000.0, -12.0f, 1)));
    const double boost = fmax(o4(sr, B_BELL, 1000.0, 12.0f, 1), fmax(o4(sr, B_LOW, 200.0, 12.0f, 1), o4(sr, B_HIGH, 8000.0, 12.0f, 1)));
    const double sab = o4(sr, B_SABOTAGE_LP, 200.0, -12.0f, 0);
    check(sab > 0.1, "O4 sabotage: the non-complementary lowpass as B exceeds the input", sr);
    o5_o6(sr, &att[r], &rel[r]);
    o7(sr);
    o8(sr);
    printf("  %6.0f Hz: O4 worst over bound cut %+.4f boost %+.4f dB, sabotage %+.3f dB; O5 %.3f ms O6 %.3f ms\n", sr, cut,
           boost, sab, att[r], rel[r]);
  }
  double spread = 0.0, tspread = 0.0;
  for (int c = 0; c < 4; c++)
    for (int li = 0; li < 16; li++) {
      double lo = 1e9, hi = -1e9;
      for (unsigned r = 0; r < OMX_DECLARED_RATE_COUNT; r++) {
        lo = fmin(lo, curve[r][c][li]);
        hi = fmax(hi, curve[r][c][li]);
      }
      spread = fmax(spread, hi - lo);
    }
  for (unsigned r = 0; r < OMX_DECLARED_RATE_COUNT; r++)
    tspread = fmax(tspread, fmax(fabs(att[r] - att[0]) / att[0], fabs(rel[r] - rel[0]) / rel[0]));
  check(spread <= 0.1, "O9 the static curve agrees across rates within 0.1 dB", 0.0);
  check(tspread <= 0.02, "O9 the time constants agree across rates within 2 %", 0.0);
  printf("  O9 rate spread: offset %.4f dB, time constants %.2f %%\n", spread, 100.0 * tspread);
  printf("%s: %d checks, %d failures\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
