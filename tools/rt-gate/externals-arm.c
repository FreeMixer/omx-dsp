// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * externals-arm.c — the arm that measured every row of omx-rt-gate's class table (classes.tbl).
 *
 * Each case calls its symbol over a spread of arguments, through the real symbol (built -O0
 * -fno-builtin, so no call is folded or inlined away), with the program's allocator interposed: the
 * process's malloc, calloc, realloc, free and the aligned allocators count every call made while a
 * case runs, from this program or from inside the library. A case passes when the symbol ran and
 * nothing was allocated or freed. It prints `measured <case> calls=<n> allocations=0`.
 *
 *   externals-arm            every case
 *   externals-arm <case>...  the cases named
 *   externals-arm --sabotage the arm's own check: a case that allocates (strdup) must be read red
 *
 * What it cannot see: a lock taken inside the library (glibc's internal calls do not go through the
 * interposable symbols), and time. The class table says so beside the classes this matters for.
 */
#define _GNU_SOURCE
#include <math.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);
extern void *__libc_memalign(size_t, size_t);

static volatile int armed;
static volatile long allocations;
void *malloc(size_t n) { if (armed) allocations++; return __libc_malloc(n); }
void *calloc(size_t a, size_t b) { if (armed) allocations++; return __libc_calloc(a, b); }
void *realloc(void *p, size_t n) { if (armed) allocations++; return __libc_realloc(p, n); }
void free(void *p) { if (armed && p) allocations++; __libc_free(p); }
void *memalign(size_t a, size_t n) { if (armed) allocations++; return __libc_memalign(a, n); }
void *aligned_alloc(size_t a, size_t n) { if (armed) allocations++; return __libc_memalign(a, n); }
int posix_memalign(void **p, size_t a, size_t n) {
  if (armed) allocations++;
  *p = __libc_memalign(a, n);
  return *p ? 0 : 12;
}

#define N 4096
static volatile float sinkf;
static volatile double sinkd;
static volatile long sinkl;
static float xf(int i) { return -20.0f + 40.0f * (float)i / N; }
static double xd(int i) { return -20.0 + 40.0 * (double)i / N; }
static float pf(int i) { return 1e-6f + 100.0f * (float)i / N; }
static double pd(int i) { return 1e-9 + 100.0 * (double)i / N; }

#define F1(f, arg) static long case_##f(void) { for (int i = 0; i < N; i++) sinkf = f(arg(i)); return N; }
#define D1(f, arg) static long case_##f(void) { for (int i = 0; i < N; i++) sinkd = f(arg(i)); return N; }
#define F2(f, a, b) static long case_##f(void) { for (int i = 0; i < N; i++) sinkf = f(a(i), b(N - 1 - i)); return N; }
#define D2(f, a, b) static long case_##f(void) { for (int i = 0; i < N; i++) sinkd = f(a(i), b(N - 1 - i)); return N; }
F1(sinf, xf) F1(cosf, xf) F1(tanf, xf) F1(tanhf, xf) F1(expf, xf) F1(exp2f, xf) F1(logf, pf) F1(log2f, pf)
F1(log10f, pf) F1(sqrtf, pf) F1(floorf, xf) F1(ceilf, xf) F2(powf, pf, xf) F2(fminf, xf, xf) F2(fmaxf, xf, xf)
F2(nextafterf, xf, xf)
D1(sin, xd) D1(cos, xd) D1(tan, xd) D1(sinh, xd) D1(exp, xd) D1(log, pd) D1(log2, pd) D1(log10, pd) D1(sqrt, pd)
D1(floor, xd) D1(ceil, xd) D2(pow, pd, xd) D2(hypot, xd, xd) D2(atan2, xd, xd) D2(fmin, xd, xd) D2(fmax, xd, xd)
D2(copysign, xd, xd)
static long case_lrintf(void) { for (int i = 0; i < N; i++) sinkl = lrintf(xf(i)); return N; }
static long case_lroundf(void) { for (int i = 0; i < N; i++) sinkl = lroundf(xf(i)); return N; }
static long case_lrint(void) { for (int i = 0; i < N; i++) sinkl = lrint(xd(i)); return N; }
static long case_llround(void) { for (int i = 0; i < N; i++) sinkl = llround(xd(i)); return N; }
static long case_lround(void) { for (int i = 0; i < N; i++) sinkl = lround(xd(i)); return N; }
static long case_ldexp(void) { for (int i = 0; i < N; i++) sinkd = ldexp(xd(i), i % 64 - 32); return N; }

static char a[4096], b[4096];
static long case_memcpy(void) { for (int i = 0; i < N; i++) memcpy(a, b, (size_t)(i % 4096)); return N; }
static long case_memmove(void) { for (int i = 0; i < N; i++) memmove(a + 1, a, (size_t)(i % 4095)); return N; }
static long case_memset(void) { for (int i = 0; i < N; i++) memset(a, i, (size_t)(i % 4096)); return N; }
static long case_memcmp(void) { for (int i = 0; i < N; i++) sinkl = memcmp(a, b, (size_t)(i % 4096)); return N; }

static sem_t sem;
static long case_sem_post(void) {
  /* no waiter: an atomic increment and no system call */
  for (int i = 0; i < N; i++) sem_post(&sem);
  return N;
}
static long case_clock_gettime(void) {
  struct timespec ts;
  for (int i = 0; i < N; i++) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sinkl = ts.tv_nsec;
  }
  return N;
}
static long case_strdup_sabotage(void) {
  for (int i = 0; i < 4; i++) {
    char *s = strdup("x");
    sinkl = s[0];
    free(s);
  }
  return 4;
}

struct arm_case { const char *name; long (*fn)(void); };
#define C(f) {#f, case_##f}
static const struct arm_case CASES[] = {
    C(sinf), C(cosf), C(tanf), C(tanhf), C(expf), C(exp2f), C(logf), C(log2f), C(log10f), C(sqrtf), C(floorf),
    C(ceilf), C(powf), C(fminf), C(fmaxf), C(nextafterf), C(lrintf), C(lroundf),
    C(sin), C(cos), C(tan), C(sinh), C(exp), C(log), C(log2), C(log10), C(sqrt), C(floor), C(ceil), C(pow),
    C(hypot), C(atan2), C(fmin), C(fmax), C(copysign), C(lrint), C(lround), C(llround), C(ldexp),
    C(memcpy), C(memmove), C(memset), C(memcmp), C(sem_post), C(clock_gettime),
};

static int measure(const struct arm_case *c) {
  c->fn(); /* warm: lazy binding and first-touch happen outside the window */
  allocations = 0;
  armed = 1;
  long calls = c->fn();
  armed = 0;
  long got = allocations;
  printf("%s %s calls=%ld allocations=%ld\n", got ? "ALLOCATES" : "measured", c->name, calls, got);
  return got == 0;
}

int main(int argc, char **argv) {
  sem_init(&sem, 0, 0);
  memset(b, 1, sizeof b);
  if (argc == 2 && !strcmp(argv[1], "--sabotage")) {
    struct arm_case s = {"strdup", case_strdup_sabotage};
    if (measure(&s)) {
      printf("externals-arm --sabotage: FAIL, a case that allocates was read clean\n");
      return 1;
    }
    printf("externals-arm --sabotage: ok, an allocating case is read red\n");
    return 0;
  }
  int ok = 1, n = (int)(sizeof CASES / sizeof CASES[0]);
  for (int i = 0; i < n; i++) {
    int want = argc == 1;
    for (int k = 1; k < argc; k++)
      if (!strcmp(argv[k], CASES[i].name)) want = 1;
    if (want) ok &= measure(&CASES[i]);
  }
  for (int k = 1; k < argc; k++) {
    int found = 0;
    for (int i = 0; i < n; i++)
      if (!strcmp(argv[k], CASES[i].name)) found = 1;
    if (!found) {
      printf("NO-CASE %s\n", argv[k]);
      ok = 0;
    }
  }
  return ok ? 0 : 1;
}
