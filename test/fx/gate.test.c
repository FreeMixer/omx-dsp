// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The keyed gate's oracle (omx_gate.h), its DSP moved unchanged from openmixer's keyed_gate_lv2.c
// (omx-dsp-dev#34). At every declared rate, contracts on, an empty ledger after each arm:
//   A  resolve: an unconnected control is its declared default, a value outside the travel is
//      clamped into it, a NaN reads as the floor; the atom is ACT-BELOW, peak, hard knee, unity
//      make-up, oversampling on `auto`, its poles the attack and release times' at the rate;
//   B  latency: OMX_OVS_LATENCY_4X while the attack engages 4x, else 0, and 0 when disabled;
//   C  run: the host's block through the scratch is omx_dynamics_keyed over the whole block, bit
//      for bit, keyed and self; a key with SELF chosen is no key; disabled is the identity;
//   D  aliasing: outputs on the inputs, and the key on an output, change nothing.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_gate.h>

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

#define N 3000u /* not a multiple of OMX_GATE_CHUNK */

static struct omx_gate g_gate;

static void arm_resolve(float sr) {
  g_arm = "A resolve";
  omx_gate_init(&g_gate, sr);
  struct omx_gate_controls c;
  memset(&c, 0, sizeof c);
  struct omx_dyn p;
  omx_gate_resolve(&g_gate, &c, &p);
  ok(p.enabled == 1, "unconnected enabled is on", p.enabled, 1);
  ok(p.gc.thresh_db == OMX_GATE_THRESHOLD_DB_DEFAULT, "unconnected threshold is the default", p.gc.thresh_db, OMX_GATE_THRESHOLD_DB_DEFAULT);
  ok(p.gc.ratio == OMX_GATE_RATIO_DEFAULT, "unconnected ratio is the default", p.gc.ratio, OMX_GATE_RATIO_DEFAULT);
  ok(p.gc.range_db == OMX_GATE_RANGE_DB_DEFAULT, "unconnected range is the default", p.gc.range_db, OMX_GATE_RANGE_DB_DEFAULT);
  ok(p.attack_ms == OMX_GATE_ATTACK_MS_DEFAULT, "unconnected attack is the default", p.attack_ms, OMX_GATE_ATTACK_MS_DEFAULT);
  ok(p.attack_coeff == omx_pole_from_time_ms(OMX_GATE_ATTACK_MS_DEFAULT, sr), "the attack pole is the default's at the rate", p.attack_coeff, 0);
  ok(p.release_coeff == omx_pole_from_time_ms(OMX_GATE_RELEASE_MS_DEFAULT, sr), "the release pole is the default's at the rate", p.release_coeff, 0);
  ok(p.gc.mode == OMX_DYN_BELOW && p.detect == OMX_DETECT_PEAK && p.gc.knee_db == 0.0f && p.gc.makeup_lin == 1.0f &&
         p.ovs_mode == OMX_DYN_OVS_AUTO,
     "ACT-BELOW, peak, hard knee, unity make-up, auto", 0, 0);
  const float hi_t = 10.0f, hi_r = 1000.0f, lo_g = -200.0f, nan_a = nanf(""), hi_rel = 9000.0f, off = 0.2f;
  c.threshold = &hi_t;
  c.ratio = &hi_r;
  c.range = &lo_g;
  c.attack = &nan_a;
  c.release = &hi_rel;
  c.enabled = &off;
  omx_gate_resolve(&g_gate, &c, &p);
  ok(p.enabled == 0, "enabled below 0.5 is off", p.enabled, 0);
  ok(p.gc.thresh_db == OMX_GATE_THRESHOLD_DB_MAX, "threshold clamps to its roof", p.gc.thresh_db, OMX_GATE_THRESHOLD_DB_MAX);
  ok(p.gc.ratio == OMX_GATE_RATIO_MAX, "ratio clamps to its roof", p.gc.ratio, OMX_GATE_RATIO_MAX);
  ok(p.gc.range_db == OMX_GATE_RANGE_DB_MIN, "range clamps to its floor", p.gc.range_db, OMX_GATE_RANGE_DB_MIN);
  ok(p.attack_ms == OMX_GATE_ATTACK_MS_MIN, "a NaN attack reads as the floor", p.attack_ms, OMX_GATE_ATTACK_MS_MIN);
  ok(p.release_coeff == omx_pole_from_time_ms(OMX_GATE_RELEASE_MS_MAX, sr), "release clamps to its roof", p.release_coeff, 0);
}

static void arm_latency(float sr) {
  g_arm = "B latency";
  omx_gate_init(&g_gate, sr);
  struct omx_gate_controls c;
  memset(&c, 0, sizeof c);
  const float fast = 0.1f, slow = 5.0f, off = 0.0f;
  struct omx_dyn p;
  c.attack = &fast;
  omx_gate_resolve(&g_gate, &c, &p);
  ok(omx_gate_latency(&p) == (float)OMX_OVS_LATENCY_4X, "a 0.1 ms attack reports the 4x latency", omx_gate_latency(&p), OMX_OVS_LATENCY_4X);
  c.enabled = &off;
  omx_gate_resolve(&g_gate, &c, &p);
  ok(omx_gate_latency(&p) == 0.0f, "a disabled gate reports none", omx_gate_latency(&p), 0);
  c.enabled = NULL;
  c.attack = &slow;
  omx_gate_resolve(&g_gate, &c, &p);
  ok(omx_gate_latency(&p) == 0.0f, "a 5 ms attack reports none", omx_gate_latency(&p), 0);
}

static void fill(float *l, float *r, float *k) {
  g_seed = 81u;
  for (uint32_t i = 0; i < N; i++) {
    const float amp = ((i / 700u) % 2u == 0u) ? 0.5f : 0.002f;
    l[i] = amp * rnd();
    r[i] = amp * rnd();
    k[i] = ((i / 500u) % 2u == 0u) ? 0.3f * rnd() : 0.0f;
  }
}

static void arm_run(float sr) {
  g_arm = "C run";
  static float l[N], r[N], k[N], ol[N], or_[N], el[N], er[N];
  static const float attacks[] = {0.1f, 2.0f};
  const float one = 1.0f, zero = 0.0f, t = -30.0f;
  for (int ai = 0; ai < 2; ai++)
    for (int mode = 0; mode < 4; mode++) { /* keyed, key with SELF, no key, disabled */
      fill(l, r, k);
      struct omx_gate_controls c;
      memset(&c, 0, sizeof c);
      c.threshold = &t;
      c.attack = &attacks[ai];
      c.key_external = mode == 1 ? &zero : &one;
      c.enabled = mode == 3 ? &zero : &one;
      const float *key = mode <= 1 ? k : NULL;
      omx_gate_init(&g_gate, sr);
      struct omx_dyn p;
      omx_gate_resolve(&g_gate, &c, &p);
      omx_gate_run(&g_gate, &p, key, c.key_external, l, r, ol, or_, N);
      memcpy(el, l, sizeof l);
      memcpy(er, r, sizeof r);
      struct omx_dyn_state st;
      omx_dyn_state_init(&st, 1u);
      omx_dynamics_keyed(el, er, mode == 0 ? k : NULL, N, &p, &st);
      uint32_t mis = 0;
      for (uint32_t i = 0; i < N; i++) mis += ol[i] != el[i] || or_[i] != er[i];
      ok(mis == 0u, "the gate is omx_dynamics_keyed over the whole block, bit for bit", mis, 0);
      if (mode == 3) {
        uint32_t moved = 0;
        for (uint32_t i = 0; i < N; i++) moved += ol[i] != l[i] || or_[i] != r[i];
        ok(moved == 0u, "a disabled gate is the identity", moved, 0);
      }
    }
}

static void arm_alias(float sr) {
  g_arm = "D aliasing";
  static float l[N], r[N], k[N], ol[N], or_[N];
  const float t = -30.0f, a = 0.1f;
  struct omx_gate_controls c;
  memset(&c, 0, sizeof c);
  c.threshold = &t;
  c.attack = &a;
  for (int shape = 0; shape < 2; shape++) {
    fill(l, r, k);
    omx_gate_init(&g_gate, sr);
    struct omx_dyn p;
    omx_gate_resolve(&g_gate, &c, &p);
    omx_gate_run(&g_gate, &p, k, NULL, l, r, ol, or_, N); /* the reference, nothing aliased */
    fill(l, r, k);
    omx_gate_init(&g_gate, sr);
    uint32_t mis = 0;
    if (shape == 0) { /* outputs on the inputs */
      omx_gate_run(&g_gate, &p, k, NULL, l, r, l, r, N);
      for (uint32_t i = 0; i < N; i++) mis += l[i] != ol[i] || r[i] != or_[i];
    } else { /* the key on the left output */
      static float kl[N];
      memcpy(kl, k, sizeof k);
      static float rr[N];
      omx_gate_run(&g_gate, &p, kl, NULL, l, r, kl, rr, N);
      for (uint32_t i = 0; i < N; i++) mis += kl[i] != ol[i] || rr[i] != or_[i];
    }
    ok(mis == 0u, "an aliased block is the unaliased one, bit for bit", mis, 0);
  }
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    arm_resolve(sr);
    expect_clean();
    arm_latency(sr);
    expect_clean();
    arm_run(sr);
    expect_clean();
    arm_alias(sr);
    expect_clean();
  }
  printf("fx/gate: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
