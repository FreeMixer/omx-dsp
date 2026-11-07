/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* sem_post from an LV2 run wakes a worker thread: class `wake`, green. */
// abi: lv2
// expect: 0
#define _POSIX_C_SOURCE 200809L
#include <semaphore.h>
#include "lv2-mini.h"
struct plug { sem_t s; float *out; };
static void connect_port(LV2_Handle h, uint32_t port, void *d) { (void)port; ((struct plug *)h)->out = d; }
static void run(LV2_Handle h, uint32_t n) { struct plug *p = h; (void)n; sem_post(&p->s); }
FIXTURE_DESCRIPTOR(run, connect_port, 0)
