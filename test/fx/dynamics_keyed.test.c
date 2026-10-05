// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The dynamics slot's oracle (omx_dyn.h, omx_dynamics_keyed), moved unchanged from openmixer's
// mix_dsp.h (omx-dsp-dev#34). At every declared rate, contracts on, an empty ledger after each arm:
//   A  a disabled slot is the identity on the audio AND the state, bit for bit;
//   B  a mono slot keyed on its own leg is the self-detecting slot, bit for bit, at factor 1 and 4;
//   C  a gate never adds gain: |out| <= |in| sample for sample on the base path;
//   D  the block size changes nothing: one call against uneven blocks, bit for bit, at factor 1,
//      at 4 and across an `auto` switch (the handover);
//   E  the key decides: a silent key holds the gate at its range floor, a loud key opens it to
//      unity on a quiet leg;
//   F  the 4x control path delays the audio by OMX_OVS_LATENCY_4X, the latency it reports.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_dyn.h>

#include "fx_rates.h"

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

static void expect_clean(void) {
  const uint32_t seen = omx_contract_log.count;
  const uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (omx_contract_record_ready(r)) printf("VIOLATION [%s] %s %s\n", r->stage, r->kind, r->token);
  }
  ok(seen == 0u, "no contract violation in this arm", (double)seen, 0.0);
  omx_contract_reset();
}

static uint32_t g_seed;
static float rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

#define N 8192u

/* A gate (act BELOW) or a comp (act ABOVE) at `sr`, attack/release in ms, ovs mode as given. */
static struct omx_dyn atom(int mode, float thresh_db, float att_ms, float rel_ms, int ovs, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = mode;
  d.gc.thresh_db = thresh_db;
  d.gc.ratio = mode == OMX_DYN_BELOW ? 40.0f : 4.0f;
  d.gc.knee_db = 0.0f;
  d.gc.range_db = mode == OMX_DYN_BELOW ? -80.0f : 0.0f;
  d.gc.makeup_lin = 1.0f;
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = ovs;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(rel_ms, sr);
  return d;
}

/* Bursts of noise at `amp`, silent between them, so a gate opens and closes. */
static void bursts(float *x, uint32_t n, float amp, uint32_t seed) {
  g_seed = seed;
  for (uint32_t i = 0; i < n; i++) x[i] = ((i / 1024u) % 2u == 0u) ? amp * rnd() : 0.001f * rnd();
}

static uint32_t mismatches(const float *a, const float *b, uint32_t n) {
  uint32_t m = 0;
  for (uint32_t i = 0; i < n; i++) m += a[i] != b[i];
  return m;
}

static void arm_bypass(float sr) {
  g_arm = "A bypass";
  static float l[N], r[N], l0[N], r0[N];
  bursts(l, N, 0.5f, 11u);
  bursts(r, N, 0.3f, 12u);
  memcpy(l0, l, sizeof l);
  memcpy(r0, r, sizeof r);
  struct omx_dyn d = atom(OMX_DYN_BELOW, -30.0f, 1.0f, 50.0f, OMX_DYN_OVS_AUTO, sr);
  d.enabled = 0;
  struct omx_dyn_state st, st0;
  omx_dyn_state_init(&st, 1u);
  st0 = st;
  omx_dynamics_keyed(l, r, NULL, N, &d, &st);
  ok(mismatches(l, l0, N) + mismatches(r, r0, N) == 0u, "a disabled slot leaves the audio untouched", 0, 0);
  ok(memcmp(&st, &st0, sizeof st) == 0, "a disabled slot leaves the state untouched", 0, 0);
}

static void arm_self_key(float sr) {
  g_arm = "B self key";
  static float a[N], b[N];
  for (int ovs = OMX_DYN_OVS_OFF; ovs <= OMX_DYN_OVS_X4; ovs++) {
    bursts(a, N, 0.5f, 21u);
    memcpy(b, a, sizeof a);
    static float key[N];
    memcpy(key, a, sizeof a);
    const struct omx_dyn d = atom(OMX_DYN_BELOW, -30.0f, 0.3f, 40.0f, ovs, sr);
    struct omx_dyn_state sa, sb;
    omx_dyn_state_init(&sa, 1u);
    omx_dyn_state_init(&sb, 1u);
    omx_dynamics_keyed(a, NULL, NULL, N, &d, &sa);
    omx_dynamics_keyed(b, NULL, key, N, &d, &sb);
    ok(mismatches(a, b, N) == 0u, "a mono slot keyed on its own leg is the self-detecting slot", mismatches(a, b, N), 0);
  }
}

static void arm_no_gain(float sr) {
  g_arm = "C no gain added";
  static float l[N], r[N], l0[N], r0[N];
  bursts(l, N, 0.5f, 31u);
  bursts(r, N, 0.4f, 32u);
  memcpy(l0, l, sizeof l);
  memcpy(r0, r, sizeof r);
  const struct omx_dyn d = atom(OMX_DYN_BELOW, -30.0f, 1.0f, 30.0f, OMX_DYN_OVS_OFF, sr);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  omx_dynamics_keyed(l, r, NULL, N, &d, &st);
  uint32_t louder = 0;
  for (uint32_t i = 0; i < N; i++) louder += fabsf(l[i]) > fabsf(l0[i]) || fabsf(r[i]) > fabsf(r0[i]);
  ok(louder == 0u, "a gate never makes a sample louder", louder, 0);
}

static void arm_blocks(float sr) {
  g_arm = "D block size";
  static const uint32_t sizes[] = {1u, 13u, 64u, 65u, 200u, 1000u};
  static float la[N], ra[N], lb[N], rb[N];
  for (int c = 0; c < 3; c++) {
    bursts(la, N, 0.5f, 41u);
    bursts(ra, N, 0.25f, 42u);
    memcpy(lb, la, sizeof la);
    memcpy(rb, ra, sizeof ra);
    struct omx_dyn d = atom(OMX_DYN_ABOVE, -24.0f, c == 0 ? 5.0f : 0.2f, 80.0f, OMX_DYN_OVS_AUTO, sr);
    struct omx_dyn_state sa, sb;
    omx_dyn_state_init(&sa, 1u);
    omx_dyn_state_init(&sb, 1u);
    /* c == 2: the attack crosses OMX_DYN_OVS_AUTO_MS halfway, so a handover runs in both. */
    const struct omx_dyn d_late = atom(OMX_DYN_ABOVE, -24.0f, 5.0f, 80.0f, OMX_DYN_OVS_AUTO, sr);
    omx_dynamics_keyed(la, ra, NULL, N / 2u, &d, &sa);
    omx_dynamics_keyed(la + N / 2u, ra + N / 2u, NULL, N / 2u, c == 2 ? &d_late : &d, &sa);
    for (uint32_t off = 0, k = 0; off < N; k++) {
      uint32_t m = sizes[k % 6u];
      if (off < N / 2u && off + m > N / 2u) m = N / 2u - off;
      if (off + m > N) m = N - off;
      omx_dynamics_keyed(lb + off, rb + off, NULL, m, (c == 2 && off >= N / 2u) ? &d_late : &d, &sb);
      off += m;
    }
    ok(mismatches(la, lb, N) + mismatches(ra, rb, N) == 0u, "uneven blocks are one block, bit for bit",
       mismatches(la, lb, N) + mismatches(ra, rb, N), 0);
  }
}

static void arm_key(float sr) {
  g_arm = "E the key decides";
  static float l[N], r[N], key[N];
  const uint32_t settle = (uint32_t)(0.05f * sr) < N / 2u ? (uint32_t)(0.05f * sr) : N / 2u;
  for (int loud = 0; loud <= 1; loud++) {
    g_seed = 51u;
    for (uint32_t i = 0; i < N; i++) {
      l[i] = 0.01f * rnd(); /* -40 dB: below the threshold, the gate would close on itself */
      r[i] = -l[i];
      key[i] = loud ? 0.5f * rnd() : 0.0f;
    }
    const struct omx_dyn d = atom(OMX_DYN_BELOW, -30.0f, 1.0f, 30.0f, OMX_DYN_OVS_OFF, sr);
    struct omx_dyn_state st;
    omx_dyn_state_init(&st, 1u);
    static float l0[N];
    memcpy(l0, l, sizeof l);
    omx_dynamics_keyed(l, r, key, N, &d, &st);
    double worst = 0.0;
    for (uint32_t i = settle; i < N; i++) {
      if (l0[i] == 0.0f) continue;
      const double g = fabs((double)l[i] / (double)l0[i]);
      const double e = loud ? fabs(g - 1.0) : g;
      if (e > worst) worst = e;
    }
    if (loud) ok(worst < 1e-6, "a loud key holds the gate open on a quiet leg (|gain - 1|)", worst, 1e-6);
    else ok(worst <= 1e-4 * 1.0001, "a silent key holds the gate at its -80 dB floor", worst, 1e-4);
  }
}

static void arm_latency(float sr) {
  g_arm = "F 4x latency";
  static float l[N], l0[N];
  g_seed = 61u;
  for (uint32_t i = 0; i < N; i++) l[i] = 0.01f * rnd();
  memcpy(l0, l, sizeof l);
  /* A comp whose threshold the signal never reaches: unity gain, so the output is the delayed input. */
  const struct omx_dyn d = atom(OMX_DYN_ABOVE, 0.0f, 0.1f, 50.0f, OMX_DYN_OVS_AUTO, sr);
  ok(omx_dyn_oversample_factor(&d) == 4u, "an attack under OMX_DYN_OVS_AUTO_MS engages 4x", omx_dyn_oversample_factor(&d), 4);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 4u);
  omx_dynamics_keyed(l, NULL, NULL, N, &d, &st);
  double worst = 0.0;
  for (uint32_t i = 2048u; i < N; i++) {
    const double e = fabs((double)l[i] - (double)l0[i - OMX_OVS_LATENCY_4X]);
    if (e > worst) worst = e;
  }
  ok(worst < 1e-6, "the audio arrives OMX_OVS_LATENCY_4X late on the 4x path", worst, 1e-6);
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    arm_bypass(sr);
    expect_clean();
    arm_self_key(sr);
    expect_clean();
    arm_no_gain(sr);
    expect_clean();
    arm_blocks(sr);
    expect_clean();
    arm_key(sr);
    expect_clean();
    arm_latency(sr);
    expect_clean();
  }
  printf("fx/dynamics_keyed: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
