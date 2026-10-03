// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The transient designer against its declared contracts, the transient arm of the engine's
 * rt_contracts.test.c moved with the kernel: every declared rate, every input shape, random knobs
 * across the declared travels. The stage's declared postconditions — the applied gain inside L4's
 * interval and a finite block — and the identity a bypassed stage owes
 * (docs/design/specs/2026-09-26-transient-designer.md §4 L4, L5). The positive control: an attack
 * time below its travel IS recorded, so the silence above is a ledger that could have spoken.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <omxdsp/fx/omx_transient.h>

#include "contracts_battery.h"

static void test_transient(void) {
  g_stage = "transient";
  for (int ri = 0; ri < RATES; ri++) {
    for (int sh = 0; sh < SH_COUNT; sh++) {
      float in[N], l[N], r[N];
      fill(in, N, (enum shape)sh, RATE[ri]);
      memcpy(l, in, sizeof l);
      memcpy(r, in, sizeof r);
      struct omx_transient p;
      omx_transient_resolve(&p, 0, rnd(OMX_TRANSIENT_ATTACK_DB_MIN, OMX_TRANSIENT_ATTACK_DB_MAX),
                            rnd(OMX_TRANSIENT_SUSTAIN_DB_MIN, OMX_TRANSIENT_SUSTAIN_DB_MAX),
                            rnd(OMX_TRANSIENT_ATTACK_TIME_MS_MIN, OMX_TRANSIENT_ATTACK_TIME_MS_MAX),
                            rnd(OMX_TRANSIENT_SUSTAIN_TIME_MS_MIN, OMX_TRANSIENT_SUSTAIN_TIME_MS_MAX),
                            rnd(OMX_TRANSIENT_OUTPUT_DB_MIN, OMX_TRANSIENT_OUTPUT_DB_MAX), RATE[ri]);
      struct omx_transient_state st;
      omx_transient_state_init(&st);
      omx_transient_process(l, r, N, &p, &st);
      ok(omx_block_finite(l, N) && omx_block_finite(r, N), "an engaged stage is finite at any setting",
         (double)(omx_block_finite(l, N) && omx_block_finite(r, N)), 1.0);
      drain_violations(in, N);

      /* BYPASS IS THE IDENTITY — a disengaged stage touches no sample. */
      memcpy(l, in, sizeof l);
      memcpy(r, in, sizeof r);
      p.enabled = 0;
      omx_transient_process(l, r, N, &p, &st);
      ok(memcmp(l, in, sizeof l) == 0 && memcmp(r, in, sizeof r) == 0, "bypass is the IDENTITY", 0.0, 0.0);
      drain_violations(in, N);
    }
  }
}

static void test_transient_positive_control(void) {
  g_stage = "transient-positive-control";
  struct omx_transient p;
  omx_contract_log.count = 0u;
  omx_transient_resolve(&p, 0, 0.0f, 0.0f, OMX_TRANSIENT_ATTACK_TIME_MS_MIN * 0.5f, OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT,
                        0.0f, 48000.0f);
  ok(omx_contract_log.count >= 1u, "an attack time below its travel IS recorded", (double)omx_contract_log.count, 1.0);
  omx_contract_log.count = 0u;
}

int main(void) {
  omx_fx_require_rate_floor();
  test_transient_positive_control();
  test_transient();
  return omx_fx_battery_report("transient");
}
