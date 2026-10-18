// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The harness every effect instance's oracle (test/fx/<k>_instance.test.c) shares: the check, the
 * contract ledger's drain, a deterministic programme signal and the byte comparisons. Built with
 * -DOMX_CONTRACTS against the contracts archive, so a POST the instance states and breaks is a
 * FAIL here, not a silent pass.
 */
#ifndef OMXDSP_TEST_FX_INSTANCE_HARNESS_H
#define OMXDSP_TEST_FX_INSTANCE_HARNESS_H

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

/** Violations the rate does not explain, taken off the ledger since the last drain_violations. */
static uint32_t g_unexplained = 0;

/** Take the ledger's violations after one call. At 32, 64 and 128 kHz (outside the console's
 * declaration) every call records its kernels' `rate-is-declared` precondition, which the rate
 * explains and omx_fx_drain_ledger does not count; but the ledger keeps OMX_CONTRACT_MAX records and
 * counts the lost ones as unexplained, so an arm of many blocks calls this after each. */
static inline void ledger_checkpoint(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }

/** Every contract the instance and its kernel stated in the arm just run held, but for the
 * `rate-is-declared` preconditions an undeclared rate explains (omx_fx_drain_ledger). */
static void drain_violations(const char *where) {
  const uint32_t unexplained = g_unexplained + omx_fx_drain_ledger(g_sr);
  g_unexplained = 0u;
  ok(unexplained == 0u, where);
}

/** The same, passing an init's result through. */
static inline int ledger_checkpoint_int(int v) { ledger_checkpoint(); return v; }

/** Wrap a void call so the ledger is taken after it:
 * `#define omx_x_instance_run(...) OMX_FX_CHECKPOINTED(omx_x_instance_run, __VA_ARGS__)`; an init
 * that returns an int is wrapped with OMX_FX_CHECKPOINTED_INT. */
#define OMX_FX_CHECKPOINTED(fn, ...) ((void)(fn)(__VA_ARGS__), ledger_checkpoint())
#define OMX_FX_CHECKPOINTED_INT(fn, ...) ledger_checkpoint_int((fn)(__VA_ARGS__))

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
  printf("fx/%s: %d checks, %d failures\n", name, g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

#endif
