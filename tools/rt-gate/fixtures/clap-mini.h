/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* clap-mini.h — the CLAP types the fixtures bind, spelled as clap/plugin.h, clap/events.h and
 * clap/ext/params.h spell them (CLAP_ABI expands to nothing here, as on Linux). */
#ifndef OMX_RT_GATE_CLAP_MINI_H
#define OMX_RT_GATE_CLAP_MINI_H
#include <stdbool.h>
#include <stdint.h>
#define CLAP_ABI
typedef struct clap_event_header { uint32_t size, time; uint16_t space_id, type; uint32_t flags; } clap_event_header_t;
typedef struct clap_input_events {
  void *ctx;
  uint32_t(CLAP_ABI *size)(const struct clap_input_events *list);
  const clap_event_header_t *(CLAP_ABI *get)(const struct clap_input_events *list, uint32_t index);
} clap_input_events_t;
typedef struct clap_output_events {
  void *ctx;
  bool(CLAP_ABI *try_push)(const struct clap_output_events *list, const clap_event_header_t *event);
} clap_output_events_t;
typedef struct clap_process {
  int64_t steady_time;
  uint32_t frames_count;
  const clap_input_events_t *in_events;
  const clap_output_events_t *out_events;
} clap_process_t;
typedef int32_t clap_process_status;
typedef struct clap_plugin {
  const void *desc;
  void *plugin_data;
  bool(CLAP_ABI *init)(const struct clap_plugin *plugin);
  void(CLAP_ABI *destroy)(const struct clap_plugin *plugin);
  bool(CLAP_ABI *activate)(const struct clap_plugin *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames);
  void(CLAP_ABI *deactivate)(const struct clap_plugin *plugin);
  bool(CLAP_ABI *start_processing)(const struct clap_plugin *plugin);
  void(CLAP_ABI *stop_processing)(const struct clap_plugin *plugin);
  void(CLAP_ABI *reset)(const struct clap_plugin *plugin);
  clap_process_status(CLAP_ABI *process)(const struct clap_plugin *plugin, const clap_process_t *process);
  const void *(CLAP_ABI *get_extension)(const struct clap_plugin *plugin, const char *id);
  void(CLAP_ABI *on_main_thread)(const struct clap_plugin *plugin);
} clap_plugin_t;
typedef struct clap_plugin_params {
  uint32_t(CLAP_ABI *count)(const clap_plugin_t *plugin);
  void(CLAP_ABI *flush)(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out);
} clap_plugin_params_t;
#endif
