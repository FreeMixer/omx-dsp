/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* usleep and nanosleep in a CLAP process: red. */
// abi: clap
// expect: 1
// expect-red: RT-BLOCK usleep at fixtures/sleep.c:13:3 via process[audio] -> settle
// expect-red: RT-BLOCK nanosleep at fixtures/sleep.c:15:3 via process[audio] -> settle
#define _GNU_SOURCE
#include <time.h>
#include <unistd.h>
#include "clap-mini.h"
static void settle(void) {
  usleep(10);
  struct timespec ts = {0, 1000};
  nanosleep(&ts, 0);
}
static clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *pr) {
  (void)plugin; (void)pr;
  settle();
  return 0;
}
const clap_plugin_t fixture_plugin = {.process = process};
