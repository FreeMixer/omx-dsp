// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_meter.h
 * @brief The measure half of the console's meters: a block's peak, the per-leg post-coefficient
 *        peaks, and the sustained-over clip run.
 *
 * Meters are two halves (openmixer docs/design/specs/2026-10-14-lane-output-path.md §4). Measure
 * is here: pure functions over the block a lane hands in. Publish (the tap point, the atomic
 * slots, the hold into them, the cadence, the clip latch, the watched test) stays in the engine.
 * Moved unchanged from the engine's `omx_block_peak`, `omx_strip_meter_legs` and
 * `omx_sustained_over` (openmixer packages/pipewire-native/src/mix_dsp.h).
 *
 * Stateless, pure, header-only. Not in omxdsp.h: include <omxdsp/mix/omx_meter.h>.
 */
#ifndef OMX_METER_H
#define OMX_METER_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Digital full scale, linear: exactly 1.0 (0 dBFS), the level a clip run must reach.
 *
 * Paired with OMX_METER_CLIP_RUN: a clip is a SUSTAINED run at full scale, never one sample over
 * (a loud-but-valid peak or a lone inter-sample over from a resampler is not a clip).
 */
#define OMX_METER_CLIP_LINEAR 1.0f

/** @brief Consecutive samples at or above OMX_METER_CLIP_LINEAR that make a real clip. */
#define OMX_METER_CLIP_RUN 3u

/**
 * @brief The block peak, max |sample|, across up to two legs.
 * @param a The first leg, `n` samples.
 * @param b The second leg, `n` samples; `b == a` (a mono buffer, read once) or NULL (one leg).
 * @param n Samples per leg.
 * @return The raw peak, no gain folded in; 0 for `n == 0`.
 * @note RT-safe: one pass, no allocation. Thread-safe: reads only.
 */
static inline float omx_meter_block_peak(const float *a, const float *b, uint32_t n) {
  float peak = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    float x = fabsf(a[i]);
    if (x > peak) peak = x;
    if (b != NULL && b != a) {
      float y = fabsf(b[i]);
      if (y > peak) peak = y;
    }
  }
  return peak;
}

/**
 * @brief Per-leg post-coefficient peaks: each leg's own peak times its own coefficient.
 *
 * A mono caller passes `inR == inL` and the same pan-blind coefficient twice, so a pan sweep never
 * moves the meter; a stereo caller passes its balance-law coefficients, so the meter follows the
 * balance.
 * @param inL The left leg, `n` samples.
 * @param inR The right leg, `n` samples (may equal `inL`).
 * @param n Samples per leg.
 * @param coef_l The left coefficient.
 * @param coef_r The right coefficient.
 * @param out_l Out: max |inL| × coef_l.
 * @param out_r Out: max |inR| × coef_r.
 * @note RT-safe: one pass, no allocation. Thread-safe: writes only the two outputs.
 */
static inline void omx_meter_leg_peaks(const float *inL, const float *inR, uint32_t n,
                                       float coef_l, float coef_r, float *out_l, float *out_r) {
  float peak_l = 0.0f, peak_r = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    float a = fabsf(inL[i]);
    if (a > peak_l) peak_l = a;
    float b = fabsf(inR[i]);
    if (b > peak_r) peak_r = b;
  }
  *out_l = peak_l * coef_l;
  *out_r = peak_r * coef_r;
}

/**
 * @brief The sustained-over clip test: a leg carries `need` consecutive samples whose
 *        |sample| × `scale` is at or above `thresh`.
 *
 * Each leg's run is its own and resets the moment a sample falls under the threshold, so a lone
 * over among low samples never accumulates a run.
 * @param a The first leg, `n` samples.
 * @param b The second leg; `b == a` (mono, read once) or NULL (one leg).
 * @param n Samples per leg.
 * @param scale The multiplier under test (a strip's head-amp gain, or 1 for a summed bus).
 * @param thresh The threshold, linear (OMX_METER_CLIP_LINEAR for full scale).
 * @param need The run length that makes a clip (OMX_METER_CLIP_RUN).
 * @return true when either leg reaches the run.
 * @note RT-safe: one pass, no allocation. Thread-safe: reads only.
 */
static inline bool omx_meter_clip_run(const float *a, const float *b, uint32_t n, float scale,
                                      float thresh, uint32_t need) {
  uint32_t run_a = 0, run_b = 0;
  for (uint32_t i = 0; i < n; i++) {
    run_a = (fabsf(a[i]) * scale >= thresh) ? run_a + 1 : 0;
    if (run_a >= need) return true;
    if (b != NULL && b != a) {
      run_b = (fabsf(b[i]) * scale >= thresh) ? run_b + 1 : 0;
      if (run_b >= need) return true;
    }
  }
  return false;
}

#endif /* OMX_METER_H */
