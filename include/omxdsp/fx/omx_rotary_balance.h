// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_rotary_balance.h — the engine's mix_dsp.h balance law, the two functions omx_rotary.h calls
 * (omx_clamp_pan, omx_balance_law), excerpted here until a lane moves the pan law into omx-dsp as
 * a shared primitive. Unchanged.
 */
#ifndef OMX_ROTARY_BALANCE_H
#define OMX_ROTARY_BALANCE_H

#include <omxdsp/omx_contract_limits.h>

static inline float omx_clamp_pan(float pan) {
  if (pan < OMX_PAN_PAN_MIN) return OMX_PAN_PAN_MIN;
  if (pan > OMX_PAN_PAN_MAX) return OMX_PAN_PAN_MAX;
  return pan;
}

static inline void omx_balance_law(float pan, float *bL, float *bR) {
  float p = omx_clamp_pan(pan);
  *bL = p <= 0.0f ? 1.0f : 1.0f - p;
  *bR = p >= 0.0f ? 1.0f : 1.0f + p;
}

#endif /* OMX_ROTARY_BALANCE_H */
