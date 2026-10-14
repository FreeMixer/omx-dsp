// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_meter.h
 * @brief The measure half of the console's meters: a block's peak, the per-leg post-coefficient
 *        peaks, the sustained-over clip run, the peak hold's comparison, and the gain-reduction
 *        reading's hold and recovery.
 *
 * Meters are two halves (openmixer docs/design/specs/2026-10-14-lane-output-path.md §4). Measure
 * is here: pure functions over the block a lane hands in, and over a state struct the engine owns
 * and passes in. Publish (the tap point, the atomic slots, the stores into them, the cadence, the
 * clip latch, the watched test) stays in the engine. Moved unchanged from the engine's
 * `omx_block_peak`, `omx_strip_meter_legs`, `omx_sustained_over` and the comparison inside
 * `omx_meter_hold` (openmixer packages/pipewire-native/src/mix_dsp.h), and `omx_gr_meter_step`
 * (mix_gr_meter.h).
 *
 * Pure, header-only. Not in omxdsp.h: include <omxdsp/mix/omx_meter.h>.
 */
#ifndef OMX_METER_H
#define OMX_METER_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_units.h>

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

/**
 * @brief The peak hold's measure half: what a meter slot holds once this block's peak is in.
 *
 * A slot answers "the loudest moment since you last looked": it keeps the larger of what it
 * holds and the block's peak. Strictly greater, never greater-or-equal, so an equal peak leaves
 * the slot as it is and a NaN peak never lands in it. The engine loads its slot, calls this, and
 * stores the result only when it differs; the read that clears the slot is the engine's too.
 * @param held What the slot holds, linear.
 * @param peak This block's peak, linear (omx_meter_block_peak, omx_meter_leg_peaks).
 * @return `peak` when it is greater than `held`, else `held`.
 * @note RT-safe: one compare. Thread-safe: pure.
 */
static inline float omx_meter_hold_peak(float held, float peak) {
  return peak > held ? peak : held;
}

/**
 * @brief The gain-reduction reading's hold, ms: a deeper reading stays on the meter this long.
 */
#define OMX_METER_GR_HOLD_MS 300.0f

/**
 * @brief One gain-reduction reading's state, owned by the engine (one per gate, compressor or
 *        de-esser slot). Zero-initialised: no reduction, no hold.
 */
struct omx_meter_gr {
  float db;          /**< The reading, dB, <= 0. */
  float hold_left_s; /**< Seconds of hold still owed. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "meter/gr-step"
/**
 * @brief Advance a gain-reduction reading by one block: the block's deepest applied gain, held
 *        OMX_METER_GR_HOLD_MS, then recovered with the stage's own release pole.
 *
 * A block deeper than (or as deep as) the reading replaces it at once and restarts the hold. A
 * shallower block spends the hold first; once it is spent the reading closes on the block as a
 * linear gain, by `release_pole^n` per block, the time constant the stage's own gain recovers
 * with, so the meter never lags the audio by more than the hold plus the release. It lands on the
 * block once within 0.001 dB, and never passes it. A pole outside [0, 1) (a 0 ms release, NaN)
 * lands at once.
 * @param m The reading's state, read and written.
 * @param block_db This block's minimum applied gain, dB, <= 0 (the kernel's `gr_min_db`).
 * @param n Frames in the block.
 * @param sr Sample rate, Hz, > 0.
 * @param release_pole The stage's release pole per frame at `sr` (struct omx_dyn `release_coeff`).
 * @return The reading to publish, dB, <= 0.
 * @post `reading-not-positive`, `finite`.
 * @note RT-safe: at most two powf and one log10f, no allocation. Thread-safe on disjoint states.
 */
static inline float omx_meter_gr_step(struct omx_meter_gr *m, float block_db, uint32_t n, float sr,
                                      float release_pole) {
  const float dt = (float)n / sr;
  if (block_db <= m->db) {
    m->db = block_db;
    m->hold_left_s = OMX_METER_GR_HOLD_MS * 0.001f;
  } else if (m->hold_left_s > 0.0f) {
    m->hold_left_s -= dt;
  } else {
    const float k = (release_pole >= 0.0f && release_pole < 1.0f) ? powf(release_pole, (float)n) : 0.0f;
    const float want = omx_db_to_lin(block_db);
    const float g = want + (omx_db_to_lin(m->db) - want) * k;
    m->db = omx_lin_to_db(g);
    if (!(m->db < block_db - 0.001f)) m->db = block_db; /* landed (within 0.001 dB), or NaN */
  }
  OMX_POST(m->db <= 0.0f, "reading-not-positive");
  OMX_POST(m->db - m->db == 0.0f, "finite");
  return m->db;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_METER_H */
