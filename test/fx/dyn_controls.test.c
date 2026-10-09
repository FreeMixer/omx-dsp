// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The dynamics controls the contract declares beyond threshold/ratio/range/attack/release/make-up
// (omx-contract#21): the gate's HOLD and HYSTERESIS, the comp's soft KNEE and its MIX. At every
// declared rate, contracts on, each against its textbook closed form:
//
//   hold        the gate stays open exactly `hold` frames longer: the output after the key drops is
//               the no-hold output delayed by omx_dyn_hold_frames(ms, rate), sample for sample;
//   hysteresis  on a level ramp the gate opens at T and closes at T - H (H = 0: both at T), and
//               with a release the close is shaped (no (ratio-1)*H dB step);
//   knee        at the threshold the gain is the quadratic knee's closed form (1/R - 1) * K / 8 dB,
//               and at T +- K/2 it meets the hard curve;
//   mix         50 % is the average of the dry input and the fully compressed output, on the base
//               path and on the 4x path (where the dry share rides the 72-frame compensation);
//               0 % is the compensated input itself.
//
// The NEUTRAL values (hold 0, hysteresis 0, knee 0, mix 100 %) are held bit for bit against the
// previous kernel by dyn_controls_golden.test.c and by every other dynamics golden digest.
//
// Usage: fx_dyn_controls (exit 0 green, 1 a check failed, 2 the rate floor is missing).
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_dyn.h>

#include "fx_rates.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
}

static void expect_clean(float sr) {
  const uint32_t unexplained = omx_fx_drain_ledger(sr);
  ok(unexplained == 0u, "no contract violation in this arm that the rate does not explain", (double)unexplained, 0.0);
}

/* One second at the highest declared rate, plus the 4x path's compensation. */
#define N 200000u

static struct omx_dyn gate_atom(float thresh_db, float att_ms, float rel_ms, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = OMX_DYN_BELOW;
  d.gc.thresh_db = thresh_db;
  d.gc.ratio = 16.0f;
  d.gc.range_db = -80.0f;
  d.gc.makeup_lin = 1.0f;
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = OMX_DYN_OVS_OFF;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(rel_ms, sr);
  return d;
}

static struct omx_dyn comp_atom(float thresh_db, float ratio, float knee_db, float att_ms, float rel_ms,
                                float makeup_db, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = OMX_DYN_ABOVE;
  d.gc.thresh_db = thresh_db;
  d.gc.ratio = ratio;
  d.gc.knee_db = knee_db;
  d.gc.makeup_lin = omx_db_to_lin(makeup_db);
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = OMX_DYN_OVS_AUTO;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(rel_ms, sr);
  return d;
}

/* Run `d` over a mono block in uneven sub-blocks, so nothing depends on the host's quantum. */
static void run(float *x, uint32_t n, const struct omx_dyn *d) {
  static const uint32_t sizes[] = {128u, 7u, 64u, 65u, 300u, 1u};
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, omx_dyn_oversample_factor(d)); /* no handover at the start */
  for (uint32_t off = 0, k = 0; off < n; k++) {
    uint32_t m = sizes[k % 6u];
    if (off + m > n) m = n - off;
    omx_dynamics(x + off, NULL, m, d, &st);
    off += m;
  }
}

/* The first frame at or after `from` whose gain (out/in) is below one. */
static uint32_t first_below_unity(const float *out, const float *in, uint32_t from, uint32_t n) {
  for (uint32_t i = from; i < n; i++)
    if (out[i] < in[i]) return i;
  return n;
}

static void arm_hold(float sr) {
  g_arm = "hold";
  static float in[N], a[N], b[N];
  const uint32_t n = (uint32_t)(0.6f * sr);
  const uint32_t drop = (uint32_t)(0.2f * sr);
  for (uint32_t i = 0; i < n; i++) in[i] = i < drop ? 0.3f : 0.001f;
  const struct omx_dyn plain = gate_atom(-20.0f, 1.0f, 5.0f, sr);
  for (int h = 0; h < 3; h++) {
    static const float hold_ms[] = {10.0f, 50.0f, 0.5f};
    struct omx_dyn held = plain;
    held.hold_frames = omx_dyn_hold_frames(hold_ms[h], sr);
    ok(held.hold_frames == (uint32_t)lroundf(hold_ms[h] * 0.001f * sr), "the hold's frames are its ms at the rate",
       held.hold_frames, lroundf(hold_ms[h] * 0.001f * sr));
    memcpy(a, in, n * sizeof(float));
    memcpy(b, in, n * sizeof(float));
    run(a, n, &plain);
    run(b, n, &held);
    const uint32_t t0 = first_below_unity(a, in, drop, n);
    const uint32_t th = first_below_unity(b, in, drop, n);
    ok(t0 > drop && t0 < n / 2u, "without hold the gate starts closing after the key drops", t0, drop);
    ok(th == t0 + held.hold_frames, "with hold the gate stays fully open exactly hold frames longer", (double)th - t0,
       held.hold_frames);
    uint32_t mis = 0;
    for (uint32_t i = 0; i < drop; i++) mis += a[i] != b[i];
    ok(mis == 0u, "before the drop, hold changes nothing", mis, 0);
    mis = 0;
    for (uint32_t i = t0; i + held.hold_frames < n; i++) mis += b[i + held.hold_frames] != a[i];
    ok(mis == 0u, "after the hold the release is the no-hold release, delayed by the hold, bit for bit", mis, 0);
    ok(b[n - 1] < 0.001f * 0.001f, "and the gate then closes to its range", b[n - 1], 0.001f * 0.001f);
  }
}

/* A level ramp up from -40 to 0 dB and back, in dB-linear steps; the gate's open/closed state reads
 * as gain == 1 (open, level above the close point) or < 1. */
static void arm_hysteresis(float sr) {
  g_arm = "hysteresis";
  static float in[N], a[N];
  const uint32_t half = (uint32_t)(0.25f * sr), n = 2u * half;
  for (uint32_t i = 0; i < n; i++) {
    const float t = i < half ? (float)i / (float)half : (float)(n - i) / (float)half;
    in[i] = omx_db_to_lin(-40.0f + 40.0f * t);
  }
  const float T = -20.0f;
  for (int c = 0; c < 2; c++) {
    const float H = c == 0 ? 6.0f : 0.0f;
    struct omx_dyn d = gate_atom(T, 0.0f, 0.0f, sr); /* instant detector: its level is |x| */
    d.hyst_db = H;
    memcpy(a, in, n * sizeof(float));
    run(a, n, &d);
    uint32_t up = n, down = n;
    for (uint32_t i = 1; i < half; i++)
      if (a[i] == in[i] && a[i - 1] < in[i - 1]) { up = i; break; }
    for (uint32_t i = half; i < n; i++)
      if (a[i] < in[i] && a[i - 1] == in[i - 1]) { down = i; break; }
    const float lvl_up = omx_lin_to_db(in[up]), lvl_up_prev = omx_lin_to_db(in[up - 1]);
    const float lvl_dn = omx_lin_to_db(in[down]), lvl_dn_prev = omx_lin_to_db(in[down - 1]);
    ok(up < half && lvl_up >= T - 1e-4f && lvl_up_prev < T + 1e-4f, "the gate opens where the level reaches T", lvl_up, T);
    ok(down < n && lvl_dn < T - H + 1e-4f && lvl_dn_prev >= T - H - 1e-4f,
       H > 0.0f ? "it closes where the level falls below T - H" : "with no hysteresis it closes at T", lvl_dn, T - H);
    /* Between the two, on the way down, the open gate passes at unity. */
    uint32_t shut = 0;
    for (uint32_t i = half; i < down; i++) shut += a[i] != in[i];
    ok(shut == 0u, "while open (down to the close point) the gain is exactly one", shut, 0);
  }
  /* The close, with a release, is shaped: no (ratio - 1) * H dB step in one frame. */
  {
    struct omx_dyn d = gate_atom(T, 0.0f, 20.0f, sr);
    d.hyst_db = 6.0f;
    memcpy(a, in, n * sizeof(float));
    run(a, n, &d);
    float worst = 0.0f;
    for (uint32_t i = half + 1u; i < n; i++) {
      const float g0 = omx_lin_to_db(a[i - 1] / in[i - 1]), g1 = omx_lin_to_db(a[i] / in[i]);
      if (g0 - g1 > worst) worst = g0 - g1;
    }
    ok(worst < 0.5f, "a hysteresis close with a 20 ms release steps the gain by less than 0.5 dB per frame", worst, 0.5);
  }
}

/* A static comp (instant detector) on DC: the gain IS the curve at the input's level. */
static float static_gain_db(float level_db, float ratio, float knee_db, float sr) {
  static float x[64];
  const float v = omx_db_to_lin(level_db);
  for (int i = 0; i < 64; i++) x[i] = v;
  const struct omx_dyn d = comp_atom(-20.0f, ratio, knee_db, 0.0f, 0.0f, 0.0f, sr);
  run(x, 64, &d);
  return omx_lin_to_db(x[63] / v);
}

static void arm_knee(float sr) {
  g_arm = "knee";
  static const float knees[] = {6.0f, 12.0f, 24.0f};
  static const float ratios[] = {2.0f, 4.0f, 20.0f};
  for (int r = 0; r < 3; r++)
    for (int k = 0; k < 3; k++) {
      const float R = ratios[r], K = knees[k], slope = 1.0f / R - 1.0f;
      const float at_t = static_gain_db(-20.0f, R, K, sr);
      ok(fabsf(at_t - slope * K / 8.0f) < 2e-3f, "at the threshold the gain is (1/R - 1) * K / 8 dB", at_t, slope * K / 8.0f);
      ok(fabsf(static_gain_db(-20.0f, R, 0.0f, sr)) < 1e-4f, "where the hard knee gives 0 dB", static_gain_db(-20.0f, R, 0.0f, sr), 0);
      ok(fabsf(static_gain_db(-20.0f - K / 2.0f, R, K, sr)) < 2e-3f, "at T - K/2 the knee meets unity",
         static_gain_db(-20.0f - K / 2.0f, R, K, sr), 0);
      ok(fabsf(static_gain_db(-20.0f + K / 2.0f, R, K, sr) - slope * K / 2.0f) < 2e-3f, "at T + K/2 it meets the hard slope",
         static_gain_db(-20.0f + K / 2.0f, R, K, sr), slope * K / 2.0f);
      const float q = static_gain_db(-20.0f + K / 4.0f, R, K, sr);
      const float e = K / 4.0f + K / 2.0f;
      ok(fabsf(q - slope * e * e / (2.0f * K)) < 2e-3f, "inside the knee the gain is the quadratic", q, slope * e * e / (2.0f * K));
    }
}

static void arm_mix(float sr) {
  g_arm = "mix";
  static float in[N], wet[N], mixed[N], dry[N];
  const uint32_t n = (uint32_t)(0.25f * sr);
  uint32_t seed = 0x2468aceu;
  for (uint32_t i = 0; i < n; i++) {
    seed = seed * 1664525u + 1013904223u;
    const float amp = ((i / 2048u) % 2u == 0u) ? 0.7f : 0.01f;
    in[i] = amp * ((float)(int32_t)(seed >> 8) / 8388608.0f - 1.0f);
  }
  ok(omx_dyn_dry_share(100.0f) == 0.0f && omx_dyn_dry_share(50.0f) == 0.5f && omx_dyn_dry_share(0.0f) == 1.0f,
     "mix 100/50/0 % is a dry share of 0/0.5/1", omx_dyn_dry_share(50.0f), 0.5);
  for (int path = 0; path < 2; path++) { /* base path (5 ms) and the 4x path (0.25 ms on auto) */
    const struct omx_dyn full = comp_atom(-24.0f, 6.0f, 0.0f, path ? 0.25f : 5.0f, 80.0f, 6.0f, sr);
    const uint32_t lat = path ? OMX_OVS_LATENCY_4X : 0u;
    struct omx_dyn half = full, none = full;
    half.dry = omx_dyn_dry_share(50.0f);
    none.dry = omx_dyn_dry_share(0.0f);
    memcpy(wet, in, n * sizeof(float));
    memcpy(mixed, in, n * sizeof(float));
    memcpy(dry, in, n * sizeof(float));
    run(wet, n, &full);
    run(mixed, n, &half);
    run(dry, n, &none);
    float worst = 0.0f;
    uint32_t mis = 0;
    for (uint32_t i = lat; i < n; i++) {
      const float avg = 0.5f * (in[i - lat] + wet[i]);
      const float e = fabsf(mixed[i] - avg);
      if (e > worst) worst = e;
      mis += dry[i] != in[i - lat];
    }
    ok(worst < 2e-6f, path ? "4x path: mix 50 % is the average of the compensated dry and the compressed output"
                           : "base path: mix 50 % is the average of the dry and the compressed output", worst, 2e-6);
    ok(mis == 0u, path ? "4x path: mix 0 % is the input delayed by the compensation, bit for bit"
                       : "base path: mix 0 % is the input, bit for bit", mis, 0);
  }
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    arm_hold(sr);
    expect_clean(sr);
    arm_hysteresis(sr);
    expect_clean(sr);
    arm_knee(sr);
    expect_clean(sr);
    arm_mix(sr);
    expect_clean(sr);
  }
  printf("fx/dyn_controls: %d checks, %d failed, %d rates\n", g_checks, g_failed, (int)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
