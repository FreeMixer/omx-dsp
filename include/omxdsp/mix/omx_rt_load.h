// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_rt_load.h
 * @brief The real-time load meter's measure half: one cycle's busy time against its budget, the
 *        smoothed reading, the peak and the overrun, and the graph driver's xrun events counted
 *        from its accumulated xrun time.
 *
 * The busy ratio is the fraction of the cycle's budget (`frames / rate` seconds) the work took:
 * 1 means the callback used the whole cycle and the next one will be late. The budget moves
 * with the quantum and the rate, so the fraction is the portable reading.
 *
 * Pure functions over values the caller passes in. The engine keeps the publish half: the
 * shared stats block, its atomics, the reader's clear of the peak, and when each fold runs.
 * Moved from the engine's `omx_rt_busy_ratio`, `omx_rt_load_update` and `omx_rt_xrun_update`
 * (OpenMixer's mix_dsp.h), bit for bit.
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_rt_load.h>.
 */
#ifndef OMX_RT_LOAD_H
#define OMX_RT_LOAD_H

#include <stdbool.h>
#include <stdint.h>

#include <omxdsp/omx_contract.h>

/**
 * @brief The smoothing coefficient of the busy-ratio reading, applied once per cycle (not per
 *        sample): about a ten-cycle time constant, 27 ms at 128 frames and 48 kHz, 213 ms at
 *        1024 frames. Fast enough to follow a DSP add, slow enough not to flicker on scheduler
 *        noise.
 */
#define OMX_RT_LOAD_SMOOTHING 0.9f

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "rt-load/ratio"
/**
 * @brief One cycle's busy ratio: `busy_ns` of measured work against a budget of `frames / sr`
 *        seconds.
 * @param busy_ns The work measured in the cycle, nanoseconds.
 * @param frames The cycle's frames (the quantum).
 * @param sr The sample rate, Hz.
 * @return The ratio; 0 for a cycle with no frames or no positive rate, so a callback that has not
 *         negotiated a format yet never divides by zero.
 * @post `finite`.
 * @note RT-safe, pure.
 */
static inline float omx_rt_load_ratio(uint64_t busy_ns, uint32_t frames, float sr) {
  if (frames == 0 || !(sr > 0.0f)) return 0.0f;
  const float budget_ns = ((float)frames / sr) * 1e9f;
  const float y = (float)busy_ns / budget_ns;
  OMX_POST(y - y == 0.0f, "finite");
  return y;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Advance the smoothed reading by one cycle's ratio.
 * @param smoothed The reading after the previous cycle.
 * @param cycles The cycles measured before this one: 0 seeds the reading with `ratio` itself, so
 *        it never ramps up from a zero nobody measured.
 * @param ratio This cycle's busy ratio (omx_rt_load_ratio()).
 * @return The new reading: `ratio` on the first cycle, else
 *         `OMX_RT_LOAD_SMOOTHING · smoothed + (1 − OMX_RT_LOAD_SMOOTHING) · ratio`.
 * @note RT-safe, pure. A convex combination: finite inputs give a finite reading.
 */
static inline float omx_rt_load_smooth(float smoothed, uint32_t cycles, float ratio) {
  return cycles == 0 ? ratio
                     : OMX_RT_LOAD_SMOOTHING * smoothed + (1.0f - OMX_RT_LOAD_SMOOTHING) * ratio;
}

/**
 * @brief The peak held since the reader last cleared it.
 * @param held The peak held so far (0 after the reader's clear).
 * @param ratio This cycle's busy ratio.
 * @return `ratio` when it is strictly greater than `held`, else `held`: an equal or NaN ratio
 *         keeps what is held.
 * @note RT-safe, pure.
 */
static inline float omx_rt_load_peak(float held, float ratio) { return ratio > held ? ratio : held; }

/**
 * @brief Whether a cycle overran: its work took the whole budget or more.
 * @param ratio The cycle's busy ratio.
 * @return true when `ratio >= 1`.
 * @note RT-safe, pure.
 */
static inline bool omx_rt_load_overrun(float ratio) { return ratio >= 1.0f; }

/**
 * @brief The graph driver's xrun events in one cycle, counted from its accumulated xrun time.
 *
 * The driver publishes only a running total of the time it lost; the count of cycles in which
 * that total grew is the one event count a follower can derive from it. The first reading is a
 * baseline and counts nothing, so time the driver lost before this node joined the graph is
 * never reported as this node's first glitch. A lower reading is a new driver starting from 0,
 * never a negative xrun: it counts nothing either.
 * @param seeded Whether a baseline was taken before this cycle.
 * @param seen_ns The accumulated xrun time the previous cycle read, nanoseconds.
 * @param xrun_ns The accumulated xrun time read in this cycle, nanoseconds.
 * @return 1 when a baseline exists and the total grew, else 0.
 * @note RT-safe, pure. The caller stores `xrun_ns` as the next cycle's `seen_ns` and marks the
 *       baseline taken.
 */
static inline uint32_t omx_rt_load_xrun_events(bool seeded, uint64_t seen_ns, uint64_t xrun_ns) {
  return (seeded && xrun_ns > seen_ns) ? 1u : 0u;
}

#endif /* OMX_RT_LOAD_H */
