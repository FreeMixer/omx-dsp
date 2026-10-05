// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_balance_law.h
 * @brief THE stereo balance law: pan attenuates the FAR leg linearly and never boosts, so a stereo
 *        source stays unity per leg at centre — MAIN's one stereo law, shared by the engine's
 *        strips and master, the rotary's horn/drum split and the tremolo's pan mode.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/mix_dsp.h (omx-dsp-dev#32): the
 * rotary and the tremolo read this law and never a second one. Stateless, pure.
 */
#ifndef OMX_BALANCE_LAW_H
#define OMX_BALANCE_LAW_H

#include "omx_contract_limits.h"

/**
 * @brief Clamp a pan position to the declared [-1, +1] range.
 *
 * A UI or model bug must not drive a pan law outside a quarter turn and flip a leg's sign. The
 * bounds are the contract row's (OMX_PAN_PAN_MIN/MAX), not restated here.
 * @param pan The pan position; any value.
 * @return `pan` clamped to [OMX_PAN_PAN_MIN, OMX_PAN_PAN_MAX].
 * @note RT-safe: two compares. Thread-safe: pure.
 */
static inline float omx_clamp_pan(float pan) {
  if (pan < OMX_PAN_PAN_MIN) return OMX_PAN_PAN_MIN;
  if (pan > OMX_PAN_PAN_MAX) return OMX_PAN_PAN_MAX;
  return pan;
}

/**
 * @brief Linear balance for a STEREO source: pan attenuates the FAR leg, never boosts.
 *
 *   pan=0 → (1, 1)      pan=+1 (right) → (0, 1)      pan=-1 (left) → (1, 0)
 *
 * L-in feeds only the L bus (scaled bL), R-in only the R bus (scaled bR): no −3 dB dip at centre,
 * no clip risk.
 * @param pan The balance position; clamped by omx_clamp_pan(), so any value.
 * @param bL Out: the left leg's gain, in [0, 1].
 * @param bR Out: the right leg's gain, in [0, 1].
 * @note RT-safe: compares and one subtraction. Thread-safe: pure.
 */
static inline void omx_balance_law(float pan, float *bL, float *bR) {
  float p = omx_clamp_pan(pan);
  *bL = p <= 0.0f ? 1.0f : 1.0f - p;
  *bR = p >= 0.0f ? 1.0f : 1.0f + p;
}

#endif /* OMX_BALANCE_LAW_H */
