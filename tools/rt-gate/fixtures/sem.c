/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* sem_wait and sem_timedwait in an LV2 run: red. sem_post is a wake (sem.clean.c). */
// abi: lv2
// expect: 1
// expect-red: RT-BLOCK sem_wait at fixtures/sem.c:16:3 via run[audio]
// expect-red: RT-BLOCK sem_timedwait at fixtures/sem.c:17:3 via run[audio]
#define _POSIX_C_SOURCE 200809L
#include <semaphore.h>
#include <time.h>
#include "lv2-mini.h"
struct plug { sem_t s; float *out; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void run(LV2_Handle h, uint32_t n) {
  struct plug *p = h; struct timespec ts = {0, 0}; (void)n;
  sem_wait(&p->s);
  sem_timedwait(&p->s, &ts);
}
FIXTURE_DESCRIPTOR(run, connect_port, 0)
