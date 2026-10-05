// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The shared scaffolding of the instance oracles (<k>_instance.test.c): the check counter, the
 * contract-ledger reader and the deterministic stimulus. Contracts are compiled in: an instance's
 * clamps are what keep a foreign host's ports off the kernel's PRE, so a clean ledger is a check.
 */
#ifndef OMXDSP_TEST_FX_INSTANCE_ORACLE_H
#define OMXDSP_TEST_FX_INSTANCE_ORACLE_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fx_rates.h"

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";
static float g_sr = 0.0f;

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s @ %.0f Hz] %s — measured %.9g, limit %.9g\n", g_arm, (double)g_sr, what, measured, limit);
  }
}

/** Every contract record since the last call is a failure of the current arm. */
static void expect_clean(void) {
  const uint32_t seen = omx_contract_log.count;
  const uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (omx_contract_record_ready(r)) printf("VIOLATION [%s] %s %s %s\n", g_arm, r->stage, r->kind, r->token);
  }
  ok(seen == 0u, "no contract violation in this arm", (double)seen, 0.0);
  omx_contract_reset();
}

/** The stimulus: integer-generated noise, L at half scale, R inverted at quarter scale, with a
 * burst envelope every `period` frames so a dynamics kernel has onsets to act on. */
static void stimulus(float *l, float *r, uint32_t n, uint32_t seed, uint32_t period) {
  uint32_t lcg = seed;
  for (uint32_t i = 0; i < n; i++) {
    lcg = lcg * 1664525u + 1013904223u;
    const float noise = (float)(int32_t)(lcg >> 8) / 16777216.0f - 0.25f;
    const float env = period ? ((i % period) < period / 4 ? 1.0f : 0.05f) : 1.0f;
    l[i] = noise * 0.5f * env;
    r[i] = -noise * 0.25f * env;
  }
}

/** A sine of `amp` at `hz`, in double then rounded. */
static void sine(float *x, uint32_t n, double hz, double sr, double amp) {
  for (uint32_t i = 0; i < n; i++) x[i] = (float)(amp * sin(2.0 * M_PI * hz * (double)i / sr));
}

/** RMS of x[from..n), in double. */
static double rms(const float *x, uint32_t from, uint32_t n) {
  double s = 0.0;
  for (uint32_t i = from; i < n; i++) s += (double)x[i] * (double)x[i];
  return sqrt(s / (double)(n - from));
}

static int instance_oracle_end(const char *name) {
  printf("fx/%s: %d checks, %d failed, %u declared rates\n", name, g_checks, g_failed, OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}

#endif
