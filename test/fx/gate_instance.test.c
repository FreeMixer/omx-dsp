// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The strip gate instance's oracle (fx/omx_gate_instance.h), the shell over omx_gate.h
// (omx-dsp-dev#34). At every declared rate, contracts on, an empty ledger after each arm:
//   A  ready: a NULL instance, a zero, negative or NaN rate is refused; a refused instance is the
//      identity (separate and aliased buffers) and reports no latency; a ready one starts at the
//      declared defaults;
//   B  resolve: by value is omx_gate_resolve over controls pointing at the same values, field for
//      field, in travel, out of travel and NaN; bypass is `enabled` off;
//   C  latency: omx_gate_latency of the resolved atom — 4x at a 0.1 ms attack, none at 5 ms, none
//      bypassed;
//   D  run: the instance in uneven host blocks, resolved each block, is omx_gate_init +
//      omx_gate_resolve + omx_gate_run over the whole block, bit for bit — keyed, the key with
//      SELF chosen, no key, bypassed; bypassed is the identity;
//   E  aliasing: outputs on the inputs is the unaliased run, bit for bit.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_gate_instance.h>

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

static OmxGateInstance g_inst;
static struct omx_gate g_ref;

static int same_atom(const struct omx_dyn *a, const struct omx_dyn *b) {
  return a->enabled == b->enabled && a->gc.mode == b->gc.mode && a->gc.thresh_db == b->gc.thresh_db &&
         a->gc.ratio == b->gc.ratio && a->gc.knee_db == b->gc.knee_db && a->gc.range_db == b->gc.range_db &&
         a->gc.makeup_lin == b->gc.makeup_lin && a->detect == b->detect && a->attack_coeff == b->attack_coeff &&
         a->release_coeff == b->release_coeff && a->ovs_mode == b->ovs_mode && a->attack_ms == b->attack_ms;
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

static void arm_ready(float sr) {
  g_arm = "A ready";
  static float l[N], r[N], k[N], ol[N], or_[N], cl[N], cr[N];
  ok(omx_gate_instance_init(NULL, sr) == 0, "a NULL instance is refused", 1, 0);
  static const float bad[] = {0.0f, -48000.0f};
  for (int b = 0; b < 3; b++) {
    const float rate = b < 2 ? bad[b] : nanf("");
    ok(omx_gate_instance_init(&g_inst, rate) == 0 && !g_inst.ready, "a rate that is not positive is refused", rate, 0);
    ok(omx_gate_instance_latency(&g_inst) == 0.0f, "a refused instance reports no latency", omx_gate_instance_latency(&g_inst), 0);
    omx_gate_instance_resolve(&g_inst, 0, 1, -30.0f, 16.0f, -90.0f, 0.1f, 50.0f);
    fill(l, r, k);
    omx_gate_instance_run(&g_inst, k, l, r, ol, or_, N);
    memcpy(cl, l, sizeof l);
    memcpy(cr, r, sizeof r);
    omx_gate_instance_run(&g_inst, k, cl, cr, cl, cr, N);
    ok(!memcmp(ol, l, sizeof l) && !memcmp(or_, r, sizeof r) && !memcmp(cl, l, sizeof l) && !memcmp(cr, r, sizeof r),
       "a refused instance is the identity, separate and aliased", 1, 0);
  }
  omx_gate_instance_run(NULL, NULL, l, r, ol, or_, N);
  ok(!memcmp(ol, l, sizeof l) && !memcmp(or_, r, sizeof r), "a NULL instance is the identity", 1, 0);
  ok(omx_gate_instance_init(&g_inst, sr) == 1 && g_inst.ready, "a declared rate is accepted", sr, 0);
  struct omx_gate_controls c;
  memset(&c, 0, sizeof c);
  struct omx_dyn p;
  omx_gate_init(&g_ref, sr);
  omx_gate_resolve(&g_ref, &c, &p);
  ok(same_atom(&g_inst.atom, &p), "a ready instance starts at the declared defaults", 0, 0);
}

static void arm_resolve(float sr) {
  g_arm = "B resolve";
  omx_gate_instance_init(&g_inst, sr);
  omx_gate_init(&g_ref, sr);
  static const float v[][5] = {
      {-30.0f, 16.0f, -90.0f, 1.0f, 100.0f},        /* in travel */
      {-36.0f, 40.0f, -60.0f, 0.1f, 40.0f},         /* in travel, the 4x attack */
      {10.0f, 1000.0f, -200.0f, 9000.0f, 9000.0f},  /* above and below the travel */
      {-200.0f, 0.0f, 10.0f, -5.0f, -5.0f},         /* the other ends */
  };
  for (int bypass = 0; bypass < 2; bypass++)
    for (int ke = 0; ke < 2; ke++)
      for (int i = 0; i < 5; i++) {
        float t, ra, rg, a, rl;
        if (i < 4) { t = v[i][0]; ra = v[i][1]; rg = v[i][2]; a = v[i][3]; rl = v[i][4]; }
        else t = ra = rg = a = rl = nanf("");
        omx_gate_instance_resolve(&g_inst, bypass, ke, t, ra, rg, a, rl);
        const float en = bypass ? 0.0f : 1.0f, kx = ke ? 1.0f : 0.0f;
        const struct omx_gate_controls c = {&en, &kx, &t, &ra, &rg, &a, &rl};
        struct omx_dyn p;
        omx_gate_resolve(&g_ref, &c, &p);
        ok(same_atom(&g_inst.atom, &p), "resolve by value is omx_gate_resolve, field for field", i, 0);
        ok(g_inst.atom.enabled == !bypass, "bypass is enabled off", g_inst.atom.enabled, !bypass);
        ok(g_inst.key_external == kx, "the key source is the toggle", g_inst.key_external, kx);
      }
}

static void arm_latency(float sr) {
  g_arm = "C latency";
  omx_gate_instance_init(&g_inst, sr);
  omx_gate_instance_resolve(&g_inst, 0, 1, -40.0f, 16.0f, -90.0f, 0.1f, 100.0f);
  ok(omx_gate_instance_latency(&g_inst) == (float)OMX_OVS_LATENCY_4X, "a 0.1 ms attack reports the 4x latency",
     omx_gate_instance_latency(&g_inst), OMX_OVS_LATENCY_4X);
  ok(omx_gate_instance_latency(&g_inst) == omx_gate_latency(&g_inst.atom), "the latency is omx_gate_latency's", 0, 0);
  omx_gate_instance_resolve(&g_inst, 1, 1, -40.0f, 16.0f, -90.0f, 0.1f, 100.0f);
  ok(omx_gate_instance_latency(&g_inst) == 0.0f, "a bypassed gate reports none", omx_gate_instance_latency(&g_inst), 0);
  omx_gate_instance_resolve(&g_inst, 0, 1, -40.0f, 16.0f, -90.0f, 5.0f, 100.0f);
  ok(omx_gate_instance_latency(&g_inst) == 0.0f, "a 5 ms attack reports none", omx_gate_instance_latency(&g_inst), 0);
}

static void arm_run(float sr) {
  g_arm = "D run";
  static float l[N], r[N], k[N], ol[N], or_[N], el[N], er[N];
  static const float attacks[] = {0.1f, 2.0f};
  static const uint32_t blocks[] = {1u, 77u, 256u, 1000u, 513u};
  for (int ai = 0; ai < 2; ai++)
    for (int mode = 0; mode < 4; mode++) { /* keyed, key with SELF, no key, bypassed */
      fill(l, r, k);
      const int bypass = mode == 3, ke = mode != 1;
      const float *key = mode <= 1 ? k : NULL;
      omx_gate_instance_init(&g_inst, sr);
      for (uint32_t off = 0, b = 0; off < N; b++) {
        const uint32_t m = N - off < blocks[b % 5u] ? N - off : blocks[b % 5u];
        omx_gate_instance_resolve(&g_inst, bypass, ke, -30.0f, 16.0f, -90.0f, attacks[ai], 50.0f);
        omx_gate_instance_run(&g_inst, key ? key + off : NULL, l + off, r + off, ol + off, or_ + off, m);
        off += m;
      }
      const float en = bypass ? 0.0f : 1.0f, kx = ke ? 1.0f : 0.0f, t = -30.0f, ra = 16.0f, rg = -90.0f, rl = 50.0f;
      const struct omx_gate_controls c = {&en, &kx, &t, &ra, &rg, &attacks[ai], &rl};
      omx_gate_init(&g_ref, sr);
      struct omx_dyn p;
      omx_gate_resolve(&g_ref, &c, &p);
      omx_gate_run(&g_ref, &p, key, &kx, l, r, el, er, N);
      uint32_t mis = 0;
      for (uint32_t i = 0; i < N; i++) mis += ol[i] != el[i] || or_[i] != er[i];
      ok(mis == 0u, "the instance is omx_gate_run over the whole block, bit for bit", mis, 0);
      if (bypass) {
        uint32_t moved = 0;
        for (uint32_t i = 0; i < N; i++) moved += ol[i] != l[i] || or_[i] != r[i];
        ok(moved == 0u, "a bypassed gate is the identity", moved, 0);
      } else {
        uint32_t moved = 0;
        for (uint32_t i = 0; i < N; i++) moved += ol[i] != l[i];
        ok(moved > 0u, "an engaged gate moves the signal", moved, 0);
      }
    }
}

static void arm_alias(float sr) {
  g_arm = "E aliasing";
  static float l[N], r[N], k[N], ol[N], or_[N];
  fill(l, r, k);
  omx_gate_instance_init(&g_inst, sr);
  omx_gate_instance_resolve(&g_inst, 0, 1, -30.0f, 16.0f, -90.0f, 0.1f, 50.0f);
  omx_gate_instance_run(&g_inst, k, l, r, ol, or_, N);
  omx_gate_instance_init(&g_inst, sr);
  omx_gate_instance_resolve(&g_inst, 0, 1, -30.0f, 16.0f, -90.0f, 0.1f, 50.0f);
  omx_gate_instance_run(&g_inst, k, l, r, l, r, N);
  uint32_t mis = 0;
  for (uint32_t i = 0; i < N; i++) mis += l[i] != ol[i] || r[i] != or_[i];
  ok(mis == 0u, "outputs on the inputs is the unaliased run, bit for bit", mis, 0);
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    arm_ready(sr);
    expect_clean();
    arm_resolve(sr);
    expect_clean();
    arm_latency(sr);
    expect_clean();
    arm_run(sr);
    expect_clean();
    arm_alias(sr);
    expect_clean();
  }
  printf("fx/gate_instance: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
