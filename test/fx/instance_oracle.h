// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The harness every effect instance's oracle (test/fx/<k>_instance.test.c) shares: the check, the
 * contract ledger's drain, a deterministic programme signal and the byte comparisons. Built with
 * -DOMX_CONTRACTS against the contracts archive, so a POST the instance states and breaks is a
 * FAIL here, not a silent pass.
 */
#ifndef OMXDSP_TEST_FX_INSTANCE_ORACLE_H
#define OMXDSP_TEST_FX_INSTANCE_ORACLE_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/omx_contract.h>

#include "fx_rates.h"

#ifndef OMX_CONTRACTS
#error "an instance oracle needs -DOMX_CONTRACTS"
#endif

static int g_checks = 0, g_failed = 0;
static float g_sr = 48000.0f;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %.0f Hz: %s\n", (double)g_sr, what);
  }
}

/** Every contract the instance and its kernel stated in the arm just run held. */
static void drain_violations(const char *where) {
  uint32_t seen = omx_contract_log.count;
  uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++)
    printf("VIOLATION [%s] %s %s (frame %u)\n", omx_contract_log.rec[i].stage,
           omx_contract_log.rec[i].kind, omx_contract_log.rec[i].token,
           omx_contract_log.rec[i].frame);
  ok(seen == 0u, where);
  omx_contract_log.count = 0u;
}

/** A programme block: two detuned partials per leg, a different pair on each, phase `t0`. */
static void programme(float *l, float *r, uint32_t n, uint32_t t0) {
  for (uint32_t i = 0; i < n; i++) {
    const float t = (float)(t0 + i);
    l[i] = 0.4f * sinf(t * 0.0371f) + 0.2f * sinf(t * 0.211f);
    r[i] = 0.4f * cosf(t * 0.0293f) + 0.2f * sinf(t * 0.173f);
  }
}

static inline int same_bytes(const float *a, const float *b, uint32_t n) {
  return memcmp(a, b, (size_t)n * sizeof(float)) == 0;
}

static inline int all_finite(const float *a, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (a[i] - a[i] != 0.0f) return 0;
  return 1;
}

static inline int all_zero(const float *a, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != 0.0f) return 0;
  return 1;
}

/** The knob values a foreign host may write to any control port. */
static const float HOSTILE[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f, 0.0f};
#define HOSTILE_COUNT ((int)(sizeof(HOSTILE) / sizeof(HOSTILE[0])))

static int finish(const char *name) {
  printf("fx/%s: %d checks, %d failures (%d rates)\n", name, g_checks, g_failed,
         (int)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}

#endif
