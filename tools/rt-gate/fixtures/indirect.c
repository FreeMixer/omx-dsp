/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* Indirect calls from a CLAP process that no table classifies: a callback field nothing in the
 * unit stores into, and a bare function pointer. Red, each. */
// abi: clap
// expect: 1
// expect-red: RT-INDIRECT p->on_block at fixtures/indirect.c:15:3 via process[audio]
// expect-red: RT-INDIRECT hook at fixtures/indirect.c:16:4 via process[audio]
#include "clap-mini.h"
struct plug { void (*on_block)(struct plug *p, uint32_t n); };
static void (*hook)(uint32_t n);
static clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *pr) {
  struct plug *p = plugin->plugin_data;
  /* the callback is installed at run time from elsewhere: no initializer names its targets */
  p->on_block(p, pr->frames_count);
  (*hook)(pr->frames_count);
  return 0;
}
const clap_plugin_t fixture_plugin = {.process = process};
