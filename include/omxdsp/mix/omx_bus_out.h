// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_bus_out.h
 * @brief A bus's out stage: its fader × mute split across the legs by the balance law, applied in
 *        place with a click-free linear ramp, and the post-fader per-leg peaks measured.
 *
 * Every bus runs it, MAIN, the cue bus and each stereo matrix alike (openmixer
 * docs/design/specs/2026-10-14-lane-output-path.md §2, §4). The engine keeps the ramp cursors and
 * the published meter slots: it passes the cursors in and holds the measured peaks into its slots
 * itself (the publish half). Moved from the engine's `omx_bus_out_stage` and `omx_apply_master`
 * (openmixer packages/pipewire-native/src/mix_dsp.h), bit for bit.
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_bus_out.h>.
 */
#ifndef OMX_BUS_OUT_H
#define OMX_BUS_OUT_H

#include <stdint.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_ramp.h>
#include <omxdsp/mix/omx_meter.h>
#include <omxdsp/mix/omx_mix_coef.h>
#include <omxdsp/mix/omx_pan_law.h>

/**
 * @brief The out stage's two leg gains: fader × mute, split by the balance law (a stereo bus) or
 *        the stereo pan law (a mono bus).
 * @param channels The bus lane's channel count: 2 or more selects the balance law.
 * @param fader The fader, linear.
 * @param mute Non-zero: muted.
 * @param balance The balance (or pan) position, [-1, +1]; a bus without one passes 0.
 * @param tgtL Out: the left gain.
 * @param tgtR Out: the right gain.
 * @note RT-safe: at most one cosf and one sinf. Thread-safe: pure.
 */
static inline void omx_bus_out_gains(int channels, float fader, int mute, float balance,
                                     float *tgtL, float *tgtR) {
  struct omx_pan_layout l;
  omx_pan_layout_stereo(&l);
  float g[2];
  omx_mix_strip_gains(&l, channels, 1, 1.0f, fader, mute, balance, 0.0f, g);
  *tgtL = g[0];
  *tgtR = g[1];
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "bus-out/apply"
/**
 * @brief Multiply both legs in place by their gains, each ramped linearly from its cursor to its
 *        target across the block.
 * @param busL The left leg, `n` samples, scaled in place.
 * @param busR The right leg, `n` samples, scaled in place.
 * @param n Samples per leg; 0 is a no-op.
 * @param tgtL The left gain at the end of the block.
 * @param tgtR The right gain at the end of the block.
 * @param curL In: the left gain applied at the end of the previous block. Out: `tgtL`.
 * @param curR In: the right gain at the end of the previous block. Out: `tgtR`.
 * @pre `finite-bus-in`, `finite-target`.
 * @post `finite-bus-out`; `ramp-self-heals-to-finite`: the cursors end finite.
 * @note RT-safe: one pass, no allocation, no branch in the loop. Thread-safe on disjoint buses.
 */
static inline void omx_bus_out_apply(float *restrict busL, float *restrict busR, uint32_t n,
                                     float tgtL, float tgtR, float *curL, float *curR) {
  OMX_PRE(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-bus-in");
  OMX_PRE(tgtL - tgtL == 0.0f && tgtR - tgtR == 0.0f, "finite-target");
  if (n == 0) return;
  const struct omx_ramp rl = omx_ramp_begin(curL, tgtL, n);
  const struct omx_ramp rr = omx_ramp_begin(curR, tgtR, n);
  for (uint32_t i = 0; i < n; i++) {
    busL[i] *= omx_ramp_at(rl, i);
    busR[i] *= omx_ramp_at(rr, i);
  }
  omx_ramp_end(rl, curL);
  omx_ramp_end(rr, curR);
  OMX_POST(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-bus-out");
  OMX_POST(*curL - *curL == 0.0f && *curR - *curR == 0.0f, "ramp-self-heals-to-finite");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "bus-out/stage"
/**
 * @brief The out stage, run over one block: omx_bus_out_gains(), then omx_bus_out_apply(), then the post-fader
 *        per-leg peaks.
 *
 * The engine holds `*peakL`/`*peakR` into its published meter slots; nothing is published here.
 * @param busL The left leg, `n` samples, scaled in place.
 * @param busR The right leg, `n` samples, scaled in place.
 * @param n Samples per leg; 0 leaves the bus and the cursors alone and measures 0.
 * @param channels The bus lane's channel count: 2 or more selects the balance law.
 * @param fader The fader, linear.
 * @param mute Non-zero: muted.
 * @param balance The balance position, [-1, +1]; a bus without one passes 0.
 * @param curL The left ramp cursor (omx_bus_out_apply()).
 * @param curR The right ramp cursor.
 * @param peakL Out: the left leg's post-fader peak.
 * @param peakR Out: the right leg's post-fader peak.
 * @pre `balance-within-declared-range`, `finite-bus-in`.
 * @post `gain-in-range`: neither leg's gain is negative or above the fader (the balance law only
 *       attenuates); `finite-bus-out`.
 * @note RT-safe: two passes over the block, no allocation. Thread-safe on disjoint buses.
 */
static inline void omx_bus_out_run(float *busL, float *busR, uint32_t n, int channels,
                                   float fader, int mute, float balance, float *curL,
                                   float *curR, float *peakL, float *peakR) {
  OMX_PRE(balance >= -1.0f && balance <= 1.0f, "balance-within-declared-range");
  OMX_PRE(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-bus-in");
  float tgtL, tgtR;
  omx_bus_out_gains(channels, fader, mute, balance, &tgtL, &tgtR);
  OMX_POST(tgtL >= -1e-6f && tgtL <= fader + 1e-6f && tgtR >= -1e-6f && tgtR <= fader + 1e-6f,
           "gain-in-range");
  omx_bus_out_apply(busL, busR, n, tgtL, tgtR, curL, curR);
  omx_meter_leg_peaks(busL, busR, n, 1.0f, 1.0f, peakL, peakR);
  OMX_POST(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-bus-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_BUS_OUT_H */
