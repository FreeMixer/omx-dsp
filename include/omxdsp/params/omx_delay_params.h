#ifndef OMX_DELAY_PARAMS_H
#define OMX_DELAY_PARAMS_H
/*
 * GENERATED — DO NOT EDIT BY HAND.
 * Produced by packages/omx-plugins/tools/params-gen.mjs from @freemixer/core:
 * CONSOLE_TRAVEL_DECLS['/channel/{kind}/{index}/delay'], limitForKind(DELAY_MIX_RANGE, 'input'), FX_DELAY_PINGPONG_DEFAULT.
 * Regenerate: `node packages/omx-plugins/tools/params-gen.mjs`, then commit the result
 * (docs/design/specs/2026-09-25-omx-plugins-dpf.md §3c). Order is append-only (§3b).
 */
#include "omx_plugin_param.h"

enum {
  OMX_DELAY_PARAM_TIME_MS = 0,
  OMX_DELAY_PARAM_FEEDBACK = 1,
  OMX_DELAY_PARAM_MIX = 2,
  OMX_DELAY_PARAM_TONE = 3,
  OMX_DELAY_PARAM_PINGPONG = 4,
  OMX_DELAY_PARAM_COUNT = 5
};

static const omx_plugin_param OMX_DELAY_PARAMS[OMX_DELAY_PARAM_COUNT] = {
  { "timeMs", "Time", "ms", 0.0f, 2000.0f, 300.0f, OMX_PLUGIN_PARAM_INTEGER },
  { "feedback", "Feedback", "", 0.0f, 0.99f, 0.3f, 0u },
  { "mix", "Mix", "", 0.0f, 1.0f, 0.3f, 0u },
  { "tone", "Tone", "", 0.0f, 1.0f, 0.3f, 0u },
  { "pingpong", "Pingpong", "", 0.0f, 1.0f, 0.0f, OMX_PLUGIN_PARAM_TOGGLE },
};

#endif /* OMX_DELAY_PARAMS_H */
