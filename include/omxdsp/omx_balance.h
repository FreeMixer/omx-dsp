// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_balance.h
 * @brief The linear stereo balance law: one leg holds at 1, the other falls off toward 0.
 *
 * MAIN's one stereo law (the engine's `mix_dsp.h`), moved here so a kernel that needs it — a
 * tremolo's PAN mode, a bus's balance fold — reads the same function rather than a second one.
 */
#ifndef OMX_BALANCE_H
#define OMX_BALANCE_H

#include "omx_contract_limits.h"

/**
 * @brief Clamp a pan/balance position to the declared [-1, +1] range.
 * @param pan The position to clamp.
 * @return `pan`, clamped to [`OMX_PAN_PAN_MIN`, `OMX_PAN_PAN_MAX`].
 * @note RT-safe and thread-safe: two comparisons, no state.
 */
static inline float omx_clamp_pan(float pan) {
  if (pan < OMX_PAN_PAN_MIN) return OMX_PAN_PAN_MIN;
  if (pan > OMX_PAN_PAN_MAX) return OMX_PAN_PAN_MAX;
  return pan;
}

/**
 * @brief The linear balance law: left falls off toward the right, right toward the left.
 * @param pan The position, clamped to [-1, +1] before use.
 * @param bL Left gain, out; 1 at centre and hard left, falling to 0 at hard right.
 * @param bR Right gain, out; 1 at centre and hard right, falling to 0 at hard left.
 * @note RT-safe: one clamp, two branches. Reentrant.
 */
static inline void omx_balance_law(float pan, float *bL, float *bR) {
  float p = omx_clamp_pan(pan);
  *bL = p <= 0.0f ? 1.0f : 1.0f - p;
  *bR = p >= 0.0f ? 1.0f : 1.0f + p;
}

#endif /* OMX_BALANCE_H */
