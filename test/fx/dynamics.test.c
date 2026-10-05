// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The self-detecting dynamics slot's oracle (omx_dyn.h, omx_dynamics), moved unchanged from
// openmixer's mix_dsp.h (omx-dsp-dev#34). At every declared rate, contracts on, an empty ledger
// after each arm: the slot every comp and every un-keyed gate takes IS omx_dynamics_keyed with no
// key, bit for bit, stereo and mono, a gate and a comp, on the base path, the 4x control path and
// across an `auto` handover, in uneven blocks.
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

static struct omx_dyn atom(int mode, float att_ms, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = mode;
  d.gc.thresh_db = mode == OMX_DYN_BELOW ? -30.0f : -24.0f;
  d.gc.ratio = mode == OMX_DYN_BELOW ? 40.0f : 4.0f;
  d.gc.range_db = mode == OMX_DYN_BELOW ? -80.0f : 0.0f;
  d.gc.makeup_lin = 1.0f;
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = OMX_DYN_OVS_AUTO;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(60.0f, sr);
  return d;
}

static void arm_unkeyed(float sr) {
  g_arm = "omx_dynamics is the keyed kernel with no key";
  static const uint32_t sizes[] = {1u, 13u, 64u, 65u, 200u, 1000u};
  static float la[N], ra[N], lb[N], rb[N];
  for (int mode = OMX_DYN_ABOVE; mode <= OMX_DYN_BELOW; mode++)
    for (int stereo = 0; stereo <= 1; stereo++)
      for (int c = 0; c < 3; c++) { /* 5 ms (factor 1), 0.2 ms (4x), 5 ms → 0.2 ms halfway */
        g_seed = 71u + (uint32_t)(mode * 6 + stereo * 3 + c);
        for (uint32_t i = 0; i < N; i++) {
          const float amp = ((i / 1024u) % 2u == 0u) ? 0.5f : 0.001f;
          la[i] = amp * rnd();
          ra[i] = amp * rnd();
        }
        memcpy(lb, la, sizeof la);
        memcpy(rb, ra, sizeof ra);
        const struct omx_dyn first = atom(mode, c == 1 ? 0.2f : 5.0f, sr);
        const struct omx_dyn second = atom(mode, c == 0 ? 5.0f : 0.2f, sr);
        struct omx_dyn_state sa, sb;
        omx_dyn_state_init(&sa, 1u);
        omx_dyn_state_init(&sb, 1u);
        for (uint32_t off = 0, k = 0; off < N; k++) {
          uint32_t m = sizes[k % 6u];
          if (off < N / 2u && off + m > N / 2u) m = N / 2u - off;
          if (off + m > N) m = N - off;
          const struct omx_dyn *d = off < N / 2u ? &first : &second;
          omx_dynamics(la + off, stereo ? ra + off : NULL, m, d, &sa);
          omx_dynamics_keyed(lb + off, stereo ? rb + off : NULL, NULL, m, d, &sb);
          off += m;
        }
        uint32_t mis = 0;
        for (uint32_t i = 0; i < N; i++) mis += la[i] != lb[i] || ra[i] != rb[i];
        ok(mis == 0u, "omx_dynamics is omx_dynamics_keyed(key = NULL), bit for bit", mis, 0);
        ok(memcmp(&sa, &sb, sizeof sa) == 0, "and leaves the same state", 0, 0);
      }
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    arm_unkeyed(OMX_DECLARED_RATES[ri]);
    expect_clean();
  }
  printf("fx/dynamics: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
