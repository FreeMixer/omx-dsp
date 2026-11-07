/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* A mutex taken in an LV2 run: red, both halves. */
// abi: lv2
// expect: 1
// expect-red: RT-LOCK pthread_mutex_lock at fixtures/lock.c:15:3 via run[audio]
// expect-red: RT-LOCK pthread_mutex_unlock at fixtures/lock.c:17:3 via run[audio]
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include "lv2-mini.h"
struct plug { pthread_mutex_t m; float *out; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void run(LV2_Handle h, uint32_t n) {
  struct plug *p = h;
  pthread_mutex_lock(&p->m);
  p->out[0] = (float)n;
  pthread_mutex_unlock(&p->m);
}
FIXTURE_DESCRIPTOR(run, connect_port, 0)
