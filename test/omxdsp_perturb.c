// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_perturb — the C side of R-094: built by test/perturb.sh against a PERTURBED
 * omx_contract_limits.h (one declared rate dropped, the fdelay L1 bound moved) ahead of the real
 * one, this arm predicts every violation from the header it sees and requires the contracts to
 * record exactly those (docs/design/specs/2026-09-26-dsp-primitives.md §7). Against the
 * script's literal copies of the same words the predictions miss and the arm is red.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef OMX_PERTURB_DROPPED_RATE
#error "omxdsp_perturb.c is built by test/perturb.sh, which names the dropped rate"
#endif

static int g_checks = 0, g_failed = 0;

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) { g_failed++; printf("FAIL %s — measured %.9g, limit %.9g\n", what, measured, limit); }
}

static void print_ledger(void) {
  const uint32_t n = omx_contract_log.count;
  for (uint32_t i = 0; i < n && i < OMX_CONTRACT_MAX; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (omx_contract_record_ready(r)) printf(" {%s %s %s}", r->stage, r->kind, r->token);
  }
}

/* The ledger holds exactly `want` records (0 or 1) and, when one, it is this one. */
static void expect(uint32_t want, const char *stage, const char *kind, const char *token) {
  g_checks++;
  const uint32_t n = omx_contract_log.count;
  const struct omx_contract_record *r = &omx_contract_log.rec[0];
  const int same = n == want && (want == 0u || (omx_contract_record_ready(r) && strcmp(r->stage, stage) == 0 &&
                                                strcmp(r->kind, kind) == 0 && strcmp(r->token, token) == 0));
  if (!same) {
    g_failed++;
    printf("FAIL wanted %u x {%s %s %s}, the ledger holds %u:", want, stage, kind, token, n);
    print_ledger();
    printf("\n");
  }
  omx_contract_reset();
}

int main(void) {
  const float dropped = OMX_PERTURB_DROPPED_RATE;
  omx_contract_reset();
  /* the rate list: the dropped rate is gone from the header, so it is gone from the C */
  int listed = 0;
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++) if (OMX_DECLARED_RATES[i] == dropped) listed = 1;
  ok(!listed, "the perturbed header lists the dropped rate no more", dropped, 0.0);
  ok(!omx_rate_is_declared(dropped), "the dropped rate is refused", dropped, 0.0);
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++)
    ok(omx_rate_is_declared(OMX_DECLARED_RATES[i]), "a rate the header keeps is declared", OMX_DECLARED_RATES[i], 1.0);
  expect(0u, "", "", "");
  /* the one-pole design at the dropped rate: exactly its rate PRE */
  (void)omx_pole_from_cutoff_hz(1000.0f, dropped);
  expect(1u, "onepole/from-cutoff-hz", "pre", "rate-is-declared");
  (void)omx_pole_from_time_ms(10.0f, dropped);
  expect(1u, "onepole/from-time-ms", "pre", "rate-is-declared");
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++) {
    (void)omx_pole_from_cutoff_hz(1000.0f, OMX_DECLARED_RATES[i]);
    expect(0u, "", "", "");
  }
  /* the fdelay L1 bound: the order-3 kernel at its worst fraction, the header's bound its judge */
  float c[OMX_FDELAY_MAX_TAPS];
  omx_fdelay_lagrange(3, 0.5f, c);
  double l1 = 0.0;
  for (int k = 0; k <= 3; k++) l1 += fabs((double)c[k]);
  const int predicted = l1 > (double)OMX_FDELAY_READ_L1_NORM + 1e-6;
  ok(predicted, "the perturbed bound sits below the kernel's norm, so a violation is predicted", l1, OMX_FDELAY_READ_L1_NORM);
  expect(predicted ? 1u : 0u, "fdelay/kernel", "post", "an-order-3-kernel-is-inside-the-declared-l1-norm");
  printf("omxdsp_perturb: %d checks, %d failed (dropped rate %g, L1 bound %g, kernel L1 %.6f)\n",
         g_checks, g_failed, (double)dropped, (double)OMX_FDELAY_READ_L1_NORM, l1);
  return g_failed == 0 ? 0 : 1;
}
