// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * log10f-check — omx_log10f() against the libm's log10f on every non-negative float (0 to +inf),
 * one thread per slice. Only meaningful against a correctly rounded log10f (glibc 2.43's): there,
 * zero mismatches means omx_log10f() is correctly rounded everywhere (FreeMixer/omx-dsp#21).
 *   make check-log10f
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../test/support/log10f_cr.h"

#define SLICES 32u

static unsigned long bad[SLICES];

static void *slice(void *arg) {
  const uint32_t s = (uint32_t)(uintptr_t)arg, step = 0x7f800001u / SLICES + 1u;
  const uint32_t lo = s * step, hi = s == SLICES - 1u ? 0x7f800001u : lo + step;
  for (uint32_t u = lo; u < hi; u++) {
    float x, y, r;
    memcpy(&x, &u, sizeof x);
    y = omx_log10f(x);
    r = log10f(x);
    if (memcmp(&y, &r, sizeof y) != 0 && bad[s]++ < 4) printf("log10f-check: %08x omx %a libm %a\n", u, y, r);
  }
  return NULL;
}

int main(void) {
  pthread_t th[SLICES];
  unsigned long total = 0;
  for (uint32_t s = 0; s < SLICES; s++) pthread_create(&th[s], NULL, slice, (void *)(uintptr_t)s);
  for (uint32_t s = 0; s < SLICES; s++) {
    pthread_join(th[s], NULL);
    total += bad[s];
  }
  printf("log10f-check: %lu of 2139095041 non-negative floats differ from the libm's log10f\n", total);
  return total != 0;
}
