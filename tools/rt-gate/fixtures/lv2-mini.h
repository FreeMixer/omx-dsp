/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* lv2-mini.h — the LV2 core and worker types the fixtures bind, spelled as lv2/core/lv2.h and
 * lv2/worker/worker.h spell them, so the fixtures build where no LV2 headers are installed. */
#ifndef OMX_RT_GATE_LV2_MINI_H
#define OMX_RT_GATE_LV2_MINI_H
#include <stdint.h>
typedef void *LV2_Handle;
typedef struct LV2_Descriptor {
  const char *URI;
  LV2_Handle (*instantiate)(const struct LV2_Descriptor *d, double rate, const char *path, const void *const *features);
  void (*connect_port)(LV2_Handle instance, uint32_t port, void *data);
  void (*activate)(LV2_Handle instance);
  void (*run)(LV2_Handle instance, uint32_t n);
  void (*deactivate)(LV2_Handle instance);
  void (*cleanup)(LV2_Handle instance);
  const void *(*extension_data)(const char *uri);
} LV2_Descriptor;
typedef enum { LV2_WORKER_SUCCESS = 0, LV2_WORKER_ERR_UNKNOWN = 1 } LV2_Worker_Status;
typedef void *LV2_Worker_Respond_Handle;
typedef LV2_Worker_Status (*LV2_Worker_Respond_Function)(LV2_Worker_Respond_Handle handle, uint32_t size, const void *data);
typedef struct {
  LV2_Worker_Status (*work)(LV2_Handle instance, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle, uint32_t size, const void *data);
  LV2_Worker_Status (*work_response)(LV2_Handle instance, uint32_t size, const void *body);
  LV2_Worker_Status (*end_run)(LV2_Handle instance);
} LV2_Worker_Interface;
typedef void *LV2_Worker_Schedule_Handle;
typedef struct {
  LV2_Worker_Schedule_Handle handle;
  LV2_Worker_Status (*schedule_work)(LV2_Worker_Schedule_Handle handle, uint32_t size, const void *data);
} LV2_Worker_Schedule;
typedef struct {
  void *handle;
  int (*printf)(void *handle, uint32_t type, const char *fmt, ...);
} LV2_Log_Log;
/* the descriptor every fixture exports */
#define FIXTURE_DESCRIPTOR(run_fn, connect_fn, ext_fn)                                              \
  static const LV2_Descriptor fixture_descriptor = {"urn:omx-rt-gate:fixture", 0, connect_fn, 0, run_fn, \
                                                     0, 0, ext_fn};                                    \
  const LV2_Descriptor *lv2_descriptor(uint32_t index) { return index ? 0 : &fixture_descriptor; }
#endif
