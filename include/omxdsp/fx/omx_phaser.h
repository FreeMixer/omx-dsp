// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_phaser.h
 * @brief The native PHASER stage: `N` identical first-order all-pass sections swept in octaves by
 *        one LFO, a feedback path with a unit delay around the chain, and a convex wet/dry.
 *
 *     w[n] = Aᴺ( x[n] + fb·w[n−1] )        y[n] = (1 − mix)·x[n] + mix·w[n]
 *     fc   = min(F_TOP, base·2^(omx_lfo_sweep(0, depth, s)))
 *
 * Glue over libomxdsp's `omx_allpass1` (dsp-primitives §1 row 4), `omx_lfo` (row 12) and
 * `omx_flush` (row 16); it mints no primitive. The state is caller-owned and fixed-size; nothing
 * here allocates, locks or calls libm per sample: `tan` and `exp2f` run once per control point.
 * Design: docs/design/specs/2026-09-22-native-phaser.md §2–§4.
 */
#ifndef OMX_MIX_PHASER_H
#define OMX_MIX_PHASER_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_allpass.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_lfo.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_wetdry.h>

/** @brief The most sections one leg runs (spec §3 `stages` travel, {2, 4, …, 12}). */
#define OMX_PHASER_MAX_STAGES 12
/** @brief The highest centre frequency the sweep reaches, Hz, at every rate (spec §2 `F_TOP`). */
#define OMX_PHASER_F_TOP_HZ 16000.0f
/** @brief The feedback clamp, the kernel's enforcement half of the row's ±0.9 travel (spec §3). */
#define OMX_PHASER_FB_MAX 0.9f
/** @brief Control points per second: the coefficient is recomputed every `sr/6000` samples (§4). */
#define OMX_PHASER_CONTROL_HZ 6000.0f

/** @brief The resolved control atom, built once per block by the caller. */
struct omx_phaser {
  int enabled;     /**< 0 returns before a sample or a state word is touched. */
  int stages;      /**< Sections per leg, even, in {2, …, OMX_PHASER_MAX_STAGES}. */
  float base_hz;   /**< The lowest centre frequency, Hz, positive. */
  float depth_oct; /**< The sweep above `base`, octaves, non-negative. */
  float lfo_inc;   /**< The oscillator's turns per sample (`omx_lfo_inc`); 0 freezes it. */
  float feedback;  /**< Signed, inside ±OMX_PHASER_FB_MAX. */
  float mix;       /**< 0 = dry (bit-identical), 1 = wet only. */
};

/** @brief Caller-owned state: both legs' sections and feedback words, the LFO, the ramp. */
struct omx_phaser_state {
  struct omx_allpass1 sec[2][OMX_PHASER_MAX_STAGES]; /**< Each leg's sections. */
  float w[2];          /**< Each leg's chain output one sample ago, the feedback's unit delay. */
  struct omx_lfo lfo;  /**< The one oscillator both legs read. */
  float a_cur;         /**< The coefficient this sample. */
  float a_tgt;         /**< The coefficient the ramp arrives at, at the next control point. */
  float da;            /**< The ramp's step per sample. */
  float sr;            /**< The rate the ramp was primed at; 0 before the first block. */
  uint32_t k;          /**< Samples per control interval at `sr`. */
  uint32_t ctr;        /**< Samples left before the next control point. */
  int stages_live;     /**< The section count the state was last run at. */
};

/**
 * @brief Arm a state: every word zero, the oscillator at its turn's zero, the ramp unprimed.
 * @param s The state.
 * @note Control-rate. RT-safe (no allocation) and thread-safe on distinct state.
 */
static inline void omx_phaser_state_init(struct omx_phaser_state *s) { memset(s, 0, sizeof *s); }

/**
 * @brief Whether a section count is on the travel: even, 2 … OMX_PHASER_MAX_STAGES.
 * @param n The count.
 * @return 1 when legal, else 0.
 * @note RT-safe and thread-safe: pure.
 */
static inline int omx_phaser_stages_legal(int n) {
  return n >= 2 && n <= OMX_PHASER_MAX_STAGES && (n % 2) == 0;
}

/**
 * @brief The section count the kernel runs for an asked count: the legal member at or below it.
 * @param n The asked count.
 * @return `n` clamped to [2, OMX_PHASER_MAX_STAGES], an odd count rounded down to even.
 * @note RT-safe and thread-safe: pure. The row's travel refuses an illegal count; this is the
 *       enforcement half, so an odd count never reaches the chain (spec §2).
 */
static inline int omx_phaser_stages_run(int n) {
  const int c = n < 2 ? 2 : n > OMX_PHASER_MAX_STAGES ? OMX_PHASER_MAX_STAGES : n;
  return c & ~1;
}

/**
 * @brief Samples per control interval, `max(1, lrintf(sr/OMX_PHASER_CONTROL_HZ))`.
 * @param sr Sample rate, Hz, positive.
 * @return The interval: 7 at 44.1 k, 8 at 48 k, 16 at 96 k, 32 at 192 k.
 * @note RT-safe and thread-safe: one division.
 */
static inline uint32_t omx_phaser_control_interval(float sr) {
  const long k = lrintf(sr / OMX_PHASER_CONTROL_HZ);
  return k < 1 ? 1u : (uint32_t)k;
}

/**
 * @brief The feedback, clamped to ±OMX_PHASER_FB_MAX; a non-finite feedback is no feedback (0).
 * @param fb The asked feedback.
 * @return The feedback the kernel runs.
 * @note RT-safe and thread-safe: omx_clamp_or (primitives spec §1 row 23).
 */
static inline float omx_phaser_clamp_fb(float fb) {
  return omx_clamp_or(fb, -OMX_PHASER_FB_MAX, OMX_PHASER_FB_MAX, 0.0f);
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "phaser/fc"
/**
 * @brief The centre frequency at one shape value: the sweep in octaves above `base`, clamped.
 * @param p The atom.
 * @param shape The LFO's shape, in [−1, 1].
 * @return `min(F_TOP, base·2^(omx_lfo_sweep(0, depth, shape)))`, Hz.
 * @pre `a-base-is-positive`.
 * @post `fc-inside-the-top`.
 * @note Control-rate: one `exp2f`. RT-safe and thread-safe: pure.
 */
static inline float omx_phaser_fc(const struct omx_phaser *p, float shape) {
  OMX_PRE(p->base_hz > 0.0f, "a-base-is-positive");
  const float fc = p->base_hz * exp2f(omx_lfo_sweep(0.0f, p->depth_oct, shape));
  const float out = fc < OMX_PHASER_F_TOP_HZ ? fc : OMX_PHASER_F_TOP_HZ;
  OMX_POST(out > 0.0f && out <= OMX_PHASER_F_TOP_HZ, "fc-inside-the-top");
  return out;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The DERIVED `sweepHz` readback: the effective range of `fc` after the F_TOP clamp.
 * @param p The atom.
 * @param lo Receives the lowest centre, Hz.
 * @param hi Receives the highest centre, Hz.
 * @note Control-rate. RT-safe and thread-safe: pure.
 */
static inline void omx_phaser_sweep_hz(const struct omx_phaser *p, float *lo, float *hi) {
  *lo = omx_phaser_fc(p, -1.0f);
  *hi = omx_phaser_fc(p, 1.0f);
}

/**
 * @brief The `output-le-input-plus` bound on the LEVEL, linear: `(1 − mix) + mix/(1 − |fb|)`.
 * @param p The atom; the feedback is read clamped.
 * @return The largest level gain at any frequency.
 * @note RT-safe and thread-safe: pure.
 */
static inline float omx_phaser_level_bound(const struct omx_phaser *p) {
  const float fb = fabsf(omx_phaser_clamp_fb(p->feedback));
  return (1.0f - p->mix) + p->mix / (1.0f - fb);
}

#define OMX_CONTRACT_STAGE "phaser/process"
/**
 * @brief Run one block through the stage, both legs in place.
 * @param l The left leg, `n` samples.
 * @param r The right leg, `n` samples.
 * @param n The block length.
 * @param p The atom.
 * @param s The state.
 * @param sr The graph rate, Hz, a declared rate.
 * @pre `a-mix-is-inside-unity`, `stages-is-a-legal-member`, `feedback-inside-the-clamp`,
 *      `the-top-is-below-nyquist`.
 * @post `finite-out`, per sample, by omx_wetdry_mix().
 * @note RT-safe: no allocation, lock or per-sample libm call. Thread-safe on distinct state.
 *       `enabled == 0` or `mix == 0` returns before a sample or a state word is touched. A
 *       `stages` change zeroes every section from the smaller count up (spec §3: it can click).
 */
static inline void omx_phaser_process(float *l, float *r, uint32_t n, const struct omx_phaser *p,
                                      struct omx_phaser_state *s, float sr) {
  if (!p->enabled || p->mix == 0.0f) return;
  OMX_PRE(p->mix > 0.0f && p->mix <= 1.0f, "a-mix-is-inside-unity");
  OMX_PRE(omx_phaser_stages_legal(p->stages), "stages-is-a-legal-member");
  OMX_PRE(fabsf(p->feedback) <= OMX_PHASER_FB_MAX, "feedback-inside-the-clamp");
  OMX_PRE(OMX_PHASER_F_TOP_HZ < 0.5f * sr, "the-top-is-below-nyquist");
  const int ns = omx_phaser_stages_run(p->stages);
  if (ns != s->stages_live) {
    const int from = ns < s->stages_live ? ns : s->stages_live;
    for (int leg = 0; leg < 2; leg++)
      for (int k = from; k < OMX_PHASER_MAX_STAGES; k++) s->sec[leg][k].s = 0.0f;
    s->stages_live = ns;
  }
  if (s->sr != sr) {
    s->sr = sr;
    s->k = omx_phaser_control_interval(sr);
    s->a_cur = s->a_tgt = omx_allpass1_coef(omx_phaser_fc(p, omx_lfo_at(&s->lfo, 0.0f)), sr);
    s->da = 0.0f;
    s->ctr = 0u;
  }
  s->lfo.inc = p->lfo_inc;
  const float fb = omx_phaser_clamp_fb(p->feedback);
  const float mix = p->mix, dry = 1.0f - p->mix;
  const uint32_t nsec = (uint32_t)ns;
  for (uint32_t i = 0; i < n; i++) {
    if (s->ctr == 0u) {
      s->a_cur = s->a_tgt;
      float ahead = (float)s->k * s->lfo.inc;
      ahead -= floorf(ahead);
      s->a_tgt = omx_allpass1_coef(omx_phaser_fc(p, omx_lfo_at(&s->lfo, ahead)), sr);
      s->da = (s->a_tgt - s->a_cur) / (float)s->k;
      s->ctr = s->k;
    }
    s->a_cur += s->da;
    const float a = s->a_cur;
    const float xl = l[i], xr = r[i];
    const float wl = omx_allpass1_cascade(s->sec[0], nsec, xl + fb * s->w[0], a);
    const float wr = omx_allpass1_cascade(s->sec[1], nsec, xr + fb * s->w[1], a);
    omx_wetdry_loop(&l[i], &r[i], &s->w[0], &s->w[1], xl, xr, wl, wr, dry, mix);
    omx_lfo_advance(&s->lfo);
    s->ctr--;
  }
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_PHASER_H */
