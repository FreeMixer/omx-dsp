// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * room_oracle.h — what the room-analysis, sweep and alignment-fit oracles share: the checks, a
 * seeded noise source, and the allocation counter.
 *
 * The program links with malloc, calloc, realloc and free wrapped
 * (-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free). An arm allocates its buffers first,
 * then runs the kernels between room_count_begin() and room_count_end(), which requires that no
 * allocation happened in between: every buffer the kernels touch is the caller's.
 */
#ifndef OMXDSP_TEST_ROOM_ORACLE_H
#define OMXDSP_TEST_ROOM_ORACLE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t s);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

static int g_counting = 0;
static long g_allocs = 0;

void *__wrap_malloc(size_t n) {
  if (g_counting) g_allocs++;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t s) {
  if (g_counting) g_allocs++;
  return __real_calloc(n, s);
}
void *__wrap_realloc(void *p, size_t n) {
  if (g_counting) g_allocs++;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (g_counting) g_allocs++;
  __real_free(p);
}

static int g_fail = 0;
static int g_checks = 0;
static double g_rate = 0.0;
static const char *g_arm = "";

static void check(int cond, const char *what, double got, double want) {
  g_checks++;
  if (!cond) {
    g_fail++;
    if (g_fail <= 60) fprintf(stderr, "FAIL [%s] @%.0f Hz: %s (got %.12g, want %.12g)\n", g_arm, g_rate, what, got, want);
  }
}

static void room_count_begin(void) {
  g_allocs = 0;
  g_counting = 1;
}

static void room_count_end(void) {
  g_counting = 0;
  check(g_allocs == 0, "the kernels allocate nothing", (double)g_allocs, 0.0);
}

/* Zeroed memory for a test's own buffers, outside the counted span. */
static void *room_alloc(size_t n) {
  void *p = calloc(1, n ? n : 1);
  if (!p) {
    fprintf(stderr, "room oracle: out of memory\n");
    exit(2);
  }
  return p;
}

/* A seeded xorshift32: zero-mean noise in [−1, 1). */
typedef struct {
  uint32_t s;
} RoomRng;

static double room_noise(RoomRng *r) {
  uint32_t x = r->s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  r->s = x;
  return (double)x / 2147483648.0 - 1.0;
}

/* The larger of a running worst and a new error, as a comparison (BUILDING.md, "GCC 12.2 on
 * arm64"); a NaN error is carried. */
static inline double room_worse(double worst, double e) { return !(e <= worst) ? e : worst; }

static int room_report(const char *name) {
  printf("analysis/%s: %d checks, %d failed\n", name, g_checks, g_fail);
  return g_fail ? 1 : 0;
}

#endif
