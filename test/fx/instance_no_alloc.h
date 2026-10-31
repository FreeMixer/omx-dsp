// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * What an instance oracle needs to prove a run allocates nothing: the allocators wrapped and counted
 * (the Makefile's FX_NO_ALLOC_LDFLAGS links them), and the contract ledger drained at a rate of the
 * oracle set. Include after instance_oracle.h. g_counting on, run, off; g_allocs must stay 0.
 */
#ifndef OMXDSP_TEST_FX_INSTANCE_NO_ALLOC_H
#define OMXDSP_TEST_FX_INSTANCE_NO_ALLOC_H

#include <stdlib.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
static int g_counting = 0;
static long g_allocs = 0;
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t sz);
void *__wrap_realloc(void *p, size_t n);
void __wrap_free(void *p);
void *__wrap_malloc(size_t n) {
  if (g_counting) g_allocs++;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t sz) {
  if (g_counting) g_allocs++;
  return __real_calloc(n, sz);
}
void *__wrap_realloc(void *p, size_t n) {
  if (g_counting) g_allocs++;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (g_counting) g_allocs++;
  __real_free(p);
}

/* Every contract record since the last call is a failure of the arm. */
static void clean(void) {
  const uint32_t unexplained = omx_fx_drain_ledger(g_sr);
  ok(unexplained == 0u, "no contract violation in this arm", (double)unexplained, 0.0);
}

/* The oracle's frame: the rate floor, then each arm list at each of the nine rates. */
#define OMX_INSTANCE_ORACLE_MAIN(name, ...)                              \
  int main(void) {                                                       \
    (void)sine;                                                          \
    (void)rms;                                                           \
    (void)expect_clean;                                                  \
    (void)stimulus;                                                      \
    omx_fx_require_rate_floor();                                         \
    omx_fx_require_full_rates_cover_declared();                          \
    for (unsigned ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {         \
      g_sr = OMX_FX_ORACLE_RATES[ri];                                    \
      __VA_ARGS__                                                        \
    }                                                                    \
    return instance_oracle_end(name);                                    \
  }

#endif
