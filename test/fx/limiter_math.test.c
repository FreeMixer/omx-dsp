// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The precision limiter against its declared contracts (2026-09-27-precision-limiter.md §1), the
 * limiter arm of the engine's rt_contracts.test.c moved with the kernel: random ceiling, look-ahead
 * and release inside LIMITER_LIMITS, every shape, every declared rate, four blocks in a row,
 * stereo-linked and mono. Engaged: finite, no contract violation, no sample past the ceiling.
 * Bypassed: the identity, bit for bit. Then the stage's own positive control: a ceiling above its
 * travel IS recorded under `ceiling-in-travel`, so the silence above is a ledger that could have
 * spoken.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <omxdsp/fx/omx_limiter.h>

#include "contracts_battery.h"

#define LIM_CAP 2048u
static float g_lim_mem[4u * LIM_CAP];
static uint32_t g_lim_idx[LIM_CAP];

static void test_limiter(void) {
  g_stage = "limiter";
  for (int ri = 0; ri < RATES; ri++) {
    const uint32_t cap = omx_limiter_cap(RATE[ri]);
    ok(cap <= LIM_CAP, "the battery's ring holds the declared look-ahead", (double)cap, (double)LIM_CAP);
    for (int sh = 0; sh < SH_COUNT; sh++) {
      for (int mono = 0; mono < 2; mono++) {
        struct omx_limiter p;
        memset(&p, 0, sizeof p);
        p.enabled = 1;
        p.ceiling_db = rnd(OMX_LIMITER_CEILING_DB_MIN, OMX_LIMITER_CEILING_DB_MAX);
        p.release_ms = rnd(OMX_LIMITER_RELEASE_MS_MIN, OMX_LIMITER_RELEASE_MS_MAX);
        struct omx_limiter_state st;
        omx_limiter_init(&st, rnd(OMX_LIMITER_LOOKAHEAD_MS_MIN, OMX_LIMITER_LOOKAHEAD_MS_MAX),
                         RATE[ri], g_lim_mem, g_lim_idx, cap);
        const float c = omx_db_to_lin(p.ceiling_db);
        float in[N], l[N], r[N];
        for (int b = 0; b < 4; b++) {
          fill(in, N, (enum shape)sh, RATE[ri]);
          memcpy(l, in, sizeof l);
          memcpy(r, in, sizeof r);
          omx_limiter_process(l, mono ? NULL : r, N, &p, &st);
          const float pk = mono ? peak(l, N) : fmaxf(peak(l, N), peak(r, N));
          ok(omx_block_finite(l, N) && omx_block_finite(r, N) && pk <= c,
             "an engaged limiter is finite and never passes the ceiling", (double)pk, (double)c);
          drain_violations(in, N);
        }
        memcpy(l, in, sizeof l);
        memcpy(r, in, sizeof r);
        p.enabled = 0;
        omx_limiter_process(l, mono ? NULL : r, N, &p, &st);
        ok(memcmp(l, in, sizeof l) == 0 && memcmp(r, in, sizeof r) == 0, "bypass is the IDENTITY",
           0.0, 0.0);
        drain_violations(in, N);
      }
    }
  }
  struct omx_limiter p = {1, OMX_LIMITER_CEILING_DB_MAX + 1.0f, OMX_LIMITER_RELEASE_MS_DEFAULT};
  struct omx_limiter_state st;
  omx_limiter_init(&st, OMX_LIMITER_LOOKAHEAD_MS_DEFAULT, 48000.0f, g_lim_mem, g_lim_idx,
                   omx_limiter_cap(48000.0f));
  float l[N], r[N];
  fill(l, N, SH_NOISE, 48000.0f);
  memcpy(r, l, sizeof r);
  omx_contract_log.count = 0u;
  omx_limiter_process(l, r, N, &p, &st);
  int found = 0;
  for (uint32_t i = 0; i < omx_contract_log.count && i < OMX_CONTRACT_MAX; i++)
    if (strcmp(omx_contract_log.rec[i].token, "ceiling-in-travel") == 0) found = 1;
  ok(found, "a ceiling above its travel IS recorded as ceiling-in-travel", (double)found, 1.0);
  omx_contract_log.count = 0u;
}

int main(void) {
  omx_fx_require_rate_floor();
  test_limiter();
  return omx_fx_battery_report("limiter");
}
