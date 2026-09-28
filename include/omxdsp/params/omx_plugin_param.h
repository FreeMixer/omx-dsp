/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_plugin_param.h — the SHAPE of one generated plugin parameter, and nothing else: no number
 * lives here. The rows are emitted by tools/params-gen.mjs into generated/omx_<kernel>_params.h
 * from @freemixer/core's declaration (docs/design/specs/2026-09-25-omx-plugins-dpf.md §3); a DPF
 * plugin's initParameter reads them and types no range of its own.
 */
#ifndef OMX_PLUGIN_PARAM_H
#define OMX_PLUGIN_PARAM_H

/** A whole-number travel (declared `step` >= 1 over integer bounds). */
#define OMX_PLUGIN_PARAM_INTEGER 1u
/** A declared boolean: domain {0, 1}, the come-up value declared. */
#define OMX_PLUGIN_PARAM_TOGGLE 2u

typedef struct {
  const char *symbol; /* the declared field name — the LV2 symbol, the CLAP param's identity */
  const char *name;   /* derived from the symbol, never a label table */
  const char *unit;
  float min, max, def;
  unsigned flags;
} omx_plugin_param;

#endif /* OMX_PLUGIN_PARAM_H */
