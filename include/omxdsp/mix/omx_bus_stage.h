// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_bus_stage.h
 * @brief The three bus stages a console calls by name, with their contracts: one source summed
 *        into a stereo bus with a click-free ramp (omx_mix_strip()), a bus's out gain
 *        (omx_bus_out_gain()) and a bus's whole out stage with its meter hold
 *        (omx_bus_out_stage()).
 *
 * Each is a call over the rest of the mix module (omx_bus_sum.h, omx_bus_out.h, omx_meter.h) that
 * states, under its own contract stage, what the console relies on: a non-finite coefficient is
 * recorded and the contribution mutes (`bus/mix-strip`), the ramp cursors heal to finite, the
 * balance stays inside its declared range and the out gain never inverts or exceeds the fader
 * (`bus/output-gain`), and the composed out stage leaves the bus finite (`bus/output-stage`).
 * Moved from OpenMixer's engine (its mix_dsp.h) with the same names, arguments, arithmetic and
 * contract stages and tokens, bit for bit (FreeMixer/omx-dsp#185).
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_bus_stage.h>.
 */
#ifndef OMX_BUS_STAGE_H
#define OMX_BUS_STAGE_H

#include <stdatomic.h>
#include <stdint.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/mix/omx_bus_out.h>
#include <omxdsp/mix/omx_bus_sum.h>
#include <omxdsp/mix/omx_meter.h>
#include <omxdsp/mix/omx_mix_coef.h>

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "bus/mix-strip"
/**
 * @brief Sum one source into a stereo bus: each leg accumulates its input times a gain ramped
 *        linearly from its cursor to its target across the block (`bus[i] += in[i] · g(i)`).
 *
 * The summing law: linear in the input, and accumulating, so many sources fold into the same
 * bus. Each leg is one entry of the mix matrix through omx_bus_sum_accumulate(). A mono source
 * passes `inR == inL`; the inputs may alias each other, the bus legs may not alias anything.
 * A non-finite target or cursor is taken as 0: the contribution mutes for the block instead of
 * turning the whole bus to NaN, the precondition records it, and the cursors end finite, so a
 * poisoned ramp recovers on its next block. On return the cursors hold the targets (0 for a
 * non-finite one), so no ramp drift accumulates across blocks.
 * @param inL The left input, `n` samples.
 * @param inR The right input, `n` samples; `inL` for a mono source.
 * @param busL The left bus leg, `n` samples, accumulated into.
 * @param busR The right bus leg, `n` samples, accumulated into.
 * @param n Samples per leg; 0 is a no-op.
 * @param tgtL The left gain at the end of the block.
 * @param tgtR The right gain at the end of the block.
 * @param curL In: the left gain at the end of the previous block. Out: `tgtL`, or 0 when it is
 *             not finite.
 * @param curR In: the right gain at the end of the previous block. Out: `tgtR`, or 0.
 * @pre `finite-tap-l`, `finite-tap-r`: the inputs are finite; `finite-coef`: both targets are
 *      finite (a violation is recorded and survived); `finite-accumulator-in`.
 * @post `finite-accumulator-out`; `ramp-self-heals-to-finite`: both cursors end finite.
 * @note RT-safe: one pass per leg, no allocation, no branch in the loop. Thread-safe on disjoint
 *       buses and cursors.
 */
static inline void omx_mix_strip(const float *inL, const float *inR, float *restrict busL,
                                 float *restrict busR, uint32_t n, float tgtL, float tgtR,
                                 float *curL, float *curR) {
  OMX_PRE(omx_block_finite(inL, n), "finite-tap-l");
  OMX_PRE(omx_block_finite(inR, n), "finite-tap-r");
  OMX_PRE(tgtL - tgtL == 0.0f && tgtR - tgtR == 0.0f, "finite-coef");
  OMX_PRE(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-accumulator-in");
  if (n == 0) return;
  const struct omx_mixmatrix_entry el = omx_bus_sum_entry(0u, 0u, *curL, tgtL);
  const struct omx_mixmatrix_entry er = omx_bus_sum_entry(0u, 1u, *curR, tgtR);
  omx_bus_sum_accumulate(busL, inL, n, &el);
  omx_bus_sum_accumulate(busR, inR, n, &er);
  *curL = el.g_cur;
  *curR = er.g_cur;
  OMX_POST(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-accumulator-out");
  OMX_POST(*curL - *curL == 0.0f && *curR - *curR == 0.0f, "ramp-self-heals-to-finite");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "bus/output-gain"
/**
 * @brief A bus's out gain: fader × mute split across the legs by the balance law (a stereo bus)
 *        or the stereo pan law (a mono bus), applied in place with a click-free linear ramp.
 *
 * omx_bus_out_gains() then omx_bus_out_apply(), with the range the console holds the out gain to:
 * the balance only attenuates a leg toward 0 and never inverts it or lifts it above the fader ×
 * mute it multiplies, whatever the fader's travel above unity. The meter is not here: a caller
 * that runs a guard between the gain and the meter calls omx_meter_leg_peaks() itself, after it.
 * @param busL The left leg, `n` samples, scaled in place.
 * @param busR The right leg, `n` samples, scaled in place.
 * @param n Samples per leg; 0 leaves the bus alone and sets the cursors to the targets.
 * @param channels The bus lane's channel count: 2 or more selects the balance law.
 * @param fader The fader, linear.
 * @param mute Non-zero: muted.
 * @param balance The balance position, [-1, +1]; a bus without one passes 0.
 * @param curL In: the left gain at the end of the previous block. Out: the left target.
 * @param curR In: the right gain at the end of the previous block. Out: the right target.
 * @pre `balance-within-declared-range`: `balance` in [-1, +1].
 * @post `fold-coefficient-in-range`: each leg's gain in [−1e-6, fader × mute × (1 + 1e-6) + 1e-6].
 * @note RT-safe: one cosf and one sinf at most, one pass over the block, no allocation.
 *       Thread-safe on disjoint buses and cursors.
 */
static inline void omx_bus_out_gain(float *busL, float *busR, uint32_t n, int channels, float fader,
                                    int mute, float balance, float *curL, float *curR) {
  OMX_PRE(balance >= -1.0f && balance <= 1.0f, "balance-within-declared-range");
  float tgtL, tgtR;
  omx_bus_out_gains(channels, fader, mute, balance, &tgtL, &tgtR);
  OMX_POST(tgtL >= -1e-6f && tgtL <= omx_mix_strip_level(1.0f, fader, mute) * (1.0f + 1e-6f) + 1e-6f &&
               tgtR >= -1e-6f && tgtR <= omx_mix_strip_level(1.0f, fader, mute) * (1.0f + 1e-6f) + 1e-6f,
           "fold-coefficient-in-range");
  omx_bus_out_apply(busL, busR, n, tgtL, tgtR, curL, curR);
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "bus/output-stage"
/**
 * @brief A bus's whole out stage over one block: omx_bus_out_run() (the gain, then the post-fader
 *        per-leg peaks), each peak then held into the caller's meter slot.
 *
 * The hold is the read-and-clear meter's writer half: a slot keeps the larger of what it holds
 * and this block's peak (omx_meter_hold_peak(), strictly greater), so it answers "the loudest
 * moment since the reader last cleared it" whatever the reader's cadence. The slots are relaxed
 * atomics: a meter orders nothing else, and a peak read one block late is never a torn value.
 * The caller owns the slots and clears them (an exchange to 0) when it reads.
 * @param busL The left leg, `n` samples, scaled in place.
 * @param busR The right leg, `n` samples, scaled in place.
 * @param n Samples per leg; 0 leaves the bus and the cursors alone and holds a peak of 0.
 * @param channels The bus lane's channel count: 2 or more selects the balance law.
 * @param fader The fader, linear.
 * @param mute Non-zero: muted.
 * @param balance The balance position, [-1, +1]; a bus without one passes 0.
 * @param curL The left ramp cursor (omx_bus_out_apply()).
 * @param curR The right ramp cursor.
 * @param peakL The left meter slot, held.
 * @param peakR The right meter slot, held.
 * @post `finite-bus-out`: the composition leaves the bus finite.
 * @note RT-safe: two passes over the block, two relaxed loads and at most two relaxed stores, no
 *       allocation. Thread-safe on disjoint buses, cursors and slots; a reader may clear a slot
 *       concurrently.
 */
static inline void omx_bus_out_stage(float *busL, float *busR, uint32_t n, int channels,
                                     float fader, int mute, float balance, float *curL,
                                     float *curR, _Atomic float *peakL, _Atomic float *peakR) {
  float mL, mR;
  omx_bus_out_run(busL, busR, n, channels, fader, mute, balance, curL, curR, &mL, &mR);
  const float held_l = atomic_load_explicit(peakL, memory_order_relaxed);
  const float held_r = atomic_load_explicit(peakR, memory_order_relaxed);
  const float next_l = omx_meter_hold_peak(held_l, mL);
  if (next_l != held_l) atomic_store_explicit(peakL, next_l, memory_order_relaxed);
  const float next_r = omx_meter_hold_peak(held_r, mR);
  if (next_r != held_r) atomic_store_explicit(peakR, next_r, memory_order_relaxed);
  OMX_POST(omx_block_finite(busL, n) && omx_block_finite(busR, n), "finite-bus-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_BUS_STAGE_H */
