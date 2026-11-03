/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/**
 * @file fx_harness.h
 * @brief The allocation counter of the instance oracles that prove a face allocates nothing: the one copy every
 *        `*_instance.test.c` linked with `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free`
 *        (FX_NO_ALLOC_LDFLAGS) includes. While `g_counting` is set, every allocator call counts in `g_allocs`.
 */
#ifndef OMXDSP_TEST_FX_HARNESS_H
#define OMXDSP_TEST_FX_HARNESS_H
#include <stddef.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

static int g_counting = 0;
static long g_allocs = 0;

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

#endif /* OMXDSP_TEST_FX_HARNESS_H */
