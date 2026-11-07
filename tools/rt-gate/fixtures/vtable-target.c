/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* A vtable is walked, not trusted: one of the targets its initializer stores allocates. Red. */
// abi: clap
// expect: 1
// expect-red: RT-ALLOC free at fixtures/vtable-target.c:12:78 via process[audio] -> stage_grow
// expect-red: RT-ALLOC malloc at fixtures/vtable-target.c:12:67 via process[audio] -> stage_grow
#include <stdlib.h>
#include "clap-mini.h"
struct stage_ops { void (*on_block)(float *x, uint32_t n); };
static void stage_gain(float *x, uint32_t n) { for (uint32_t i = 0; i < n; i++) x[i] *= 0.5f; }
static void stage_grow(float *x, uint32_t n) { (void)x; void *b = malloc(n); free(b); }
static const struct stage_ops OPS[] = {{stage_gain}, {stage_grow}};
struct plug { const struct stage_ops *ops; float buf[64]; };
static clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *pr);
const clap_plugin_t fixture_plugin = {.process = process};
const struct stage_ops *fixture_ops = OPS;
static clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *pr) {
  struct plug *p = plugin->plugin_data;
  p->ops->on_block(p->buf, pr->frames_count);
  return 0;
}
