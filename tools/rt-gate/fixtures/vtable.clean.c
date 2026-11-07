/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* The clean twin of indirect.c: the indirect calls are a vtable whose targets an initializer stores,
 * walked like direct calls, and the host's declared audio fields (in_events.size/get, try_push,
 * params.flush). */
// abi: clap
// expect: 0
#include "clap-mini.h"
struct stage_ops { void (*on_block)(float *x, uint32_t n); };
static void stage_gain(float *x, uint32_t n) { for (uint32_t i = 0; i < n; i++) x[i] *= 0.5f; }
static void stage_mute(float *x, uint32_t n) { for (uint32_t i = 0; i < n; i++) x[i] = 0.0f; }
static const struct stage_ops OPS[] = {{stage_gain}, {.on_block = stage_mute}};
struct plug { const struct stage_ops *ops; float buf[64]; };
static void events(const clap_input_events_t *in, const clap_output_events_t *out) {
  uint32_t n = in->size(in);
  for (uint32_t i = 0; i < n; i++) out->try_push(out, in->get(in, i));
}
static clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *pr) {
  struct plug *p = plugin->plugin_data;
  p->ops->on_block(p->buf, pr->frames_count);
  events(pr->in_events, pr->out_events);
  return 0;
}
static void flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
  (void)plugin;
  events(in, out);
}
const clap_plugin_t fixture_plugin = {.process = process};
const clap_plugin_params_t fixture_params = {0, flush};
const struct stage_ops *fixture_ops = OPS;
