// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The property battery's harness, shared by every kernel's <kernel>_math.test.c: the stages' own
 * contracts (omx_contract.h) driven by six PATHOLOGICAL blocks (silence, DC, full-scale square,
 * impulse, a 30 s decaying tail, band-limited noise) and random parameter vectors drawn INSIDE the
 * declared travels, at every rate in OMX_DECLARED_RATES. Deterministic: one xorshift, one seed, so
 * a violation is reproducible from the printed seed alone. A violation prints the stage, the token
 * and the input shape, and saves that input as build/contract-<stage>-<token>.f32.
 *
 * These are the arms of the engine's rt_contracts.test.c, moved with each kernel. The including
 * file defines OMX_CONTRACT_STORAGE before including omx_contract.h, and is built with
 * -DOMX_CONTRACTS.
 */
#ifndef OMXDSP_TEST_FX_CONTRACTS_BATTERY_H
#define OMXDSP_TEST_FX_CONTRACTS_BATTERY_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_denormal.h>

#include "fx_rates.h"

#ifndef OMX_CONTRACTS
#error "the contracts battery needs -DOMX_CONTRACTS"
#endif

static int g_checks = 0;
static int g_failed = 0;
static const char *g_stage = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_stage, what, measured, limit);
  }
}

#define N 512u
#define RATES ((int)OMX_DECLARED_RATE_COUNT)
#define RATE OMX_DECLARED_RATES

/** One xorshift, one seed — a violation is reproducible from the seed alone. */
static uint32_t g_seed = 0xd59a17e3u;
static float rnd(float lo, float hi) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return lo + (hi - lo) * ((float)(g_seed >> 8) / 16777216.0f);
}

enum shape { SH_SILENCE, SH_DC, SH_SQUARE, SH_IMPULSE, SH_TAIL, SH_NOISE, SH_COUNT };
static const char *SHAPE_NAME[SH_COUNT] = {"silence", "dc",   "full-scale-square",
                                           "impulse", "tail", "noise"};

/** The shape most recently filled — named in a violation report so it is reproducible. */
static enum shape g_shape_now = SH_NOISE;

static void fill(float *b, uint32_t n, enum shape s, float sr) {
  g_shape_now = s;
  for (uint32_t i = 0; i < n; i++) {
    switch (s) {
      case SH_SILENCE: b[i] = 0.0f; break;
      case SH_DC: b[i] = 0.5f; break;
      /* FULL SCALE, alternating every 16 samples: the worst case for any nonlinearity and for
       * any recursive filter's transient. */
      case SH_SQUARE: b[i] = ((i / 16u) & 1u) ? 1.0f : -1.0f; break;
      case SH_IMPULSE: b[i] = (i == 0u) ? 1.0f : 0.0f; break;
      /* The DENORMAL APPROACH: a tail that has been decaying for a long time. exp(-t/tau) with
       * tau = 0.1 s, started 30 s ago, is ~1e-130 — well inside the denormal range at float. */
      case SH_TAIL: b[i] = (float)(1e-30 * exp(-(double)i / (0.01 * (double)sr))); break;
      default: b[i] = rnd(-0.7f, 0.7f); break;
    }
  }
}

/** Save the input that broke a contract, so the next run has it as a named oracle. */
static void save_fixture(const char *stage, const char *token, const float *b, uint32_t n) {
  char path[512];
  snprintf(path, sizeof path, "build/contract-%s-%s.f32", stage, token);
  for (char *c = path + 6; *c; c++) if (*c == '/') *c = '_';
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(b, sizeof(float), n, f);
  fclose(f);
  printf("       saved the offending input: %s\n", path);
}

/** Read the ledger, report every violation, and reset it for the next stage. */
static void drain_violations(const float *input, uint32_t n) {
  uint32_t seen = omx_contract_log.count;
  uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    printf("VIOLATION [%s] %s %s (frame %u) on input shape '%s'\n", r->stage, r->kind, r->token,
           r->frame, SHAPE_NAME[g_shape_now]);
    save_fixture(r->stage, r->token, input, n);
  }
  ok(seen == 0u, "no contract violation", (double)seen, 0.0);
  omx_contract_log.count = 0u;
}

__attribute__((unused)) static float peak(const float *b, uint32_t n) { return omx_block_absmax(b, n); }

/** Close the battery: it must have EVALUATED contracts (a battery that evaluated none measured
 * nothing) and every arm must have passed. */
static int omx_fx_battery_report(const char *name) {
  printf("fx/%s_math: seed 0x%08x, %u contracts evaluated, %d checks, %d failures (%d rates)\n", name,
         0xd59a17e3u, omx_contract_log.checks, g_checks, g_failed, RATES);
  if (omx_contract_log.checks == 0u) {
    printf("FAIL: the battery evaluated no contract\n");
    return 1;
  }
  return g_failed == 0 ? 0 : 1;
}

#endif
