// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_band_dyn.h
 * @brief The band-dynamics word: a constant section `B` whose output the compressor's gain computer
 *        scales, `y = u + (g − 1)·B(u)` — the dynamic EQ band, and the de-esser's split form.
 *
 * Composed from the
 * library words only: {@link omx_biquad} runs `B`, {@link omx_env_step} is the detector,
 * {@link omx_gaincomp_db} is the characteristic in either mode. `B` is designed on the control
 * thread and is power-complementary to `1 − B` (bell: the cookbook bandpass; shelves: `½(1 ± A₁)`),
 * so a cut never exceeds its input at any frequency and a boost never exceeds `range_db` (§4 L4).
 *
 * The signed offset, per sample: `d = sign(range)·min(|gr|, |range|)`, `gr = gaincomp(level) ≤ 0`,
 * `g = 10^(d/20)`; at `d = 0` the sample is `u` itself (§4 L1).
 *
 * Contract: NO PipeWire, NO napi, NO allocation, NO lock, NO libc beyond <math.h>/<string.h>. The
 * atom {@link omx_band_dyn} is a per-block snapshot; the state {@link omx_band_dyn_state} is plain
 * inline floats.
 */
#ifndef OMX_BAND_DYN_H
#define OMX_BAND_DYN_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_envelope.h>
#include <omxdsp/omx_gaincomp.h>
#include <omxdsp/omx_units.h>

/**
 * @brief One dynamic band's resolved controls.
 *
 * `dyn.gc.mode` is OMX_DYN_ABOVE or OMX_DYN_BELOW; `dyn.gc.range_db` is SIGNED — below zero the
 * deepest cut, above zero the largest boost; `dyn.detect` is OMX_DETECT_PEAK; `dyn.gc.makeup_lin`
 * is 1. `b_c` is `B`, `{b0,b1,b2,a1,a2}`, normalised, a-terms subtracted, designed in TypeScript.
 */
struct omx_band_dyn {
  int enabled;        /**< 0: a no-op that touches no sample and no state word. */
  struct omx_dyn dyn; /**< The compressor's own resolved atom; `range_db` signed. */
  float b_c[5];       /**< The detector/application section `B`. */
};

/** @brief One dynamic band's working state: one `B` history per leg and ONE detector cascade. */
struct omx_band_dyn_state {
  struct omx_env env; /**< The detector cascade, in the detector's level domain. */
  float b_l[4];       /**< `B`'s history on the left leg. */
  float b_r[4];       /**< `B`'s history on the right leg. */
  float gr_min_db;    /**< The deepest `g` inside the LAST run, dB <= 0 (omx_gr_block_db() of it):
                       *   the host's gain-reduction reading; 0 = none. */
};

_Static_assert(sizeof(struct omx_band_dyn_state) == sizeof(float) * (OMX_DYN_ENV_STAGES + 9u),
               "band-dynamics state must stay plain inline floats - nothing to allocate");

/** @brief The samples omx_band_dyn_run() computes the gain for at once. */
#define OMX_BAND_DYN_CHUNK 64u

/** @brief Per-block resolved detector: the cascade parameters and its per-stage poles. */
struct omx_band_dyn_block {
  struct omx_env_params ep; /**< The detector parameters, from the atom. */
  float ac;                 /**< The per-stage attack pole. */
  float rc;                 /**< The per-stage release pole. */
};

/**
 * @brief Clear one band's state.
 * @param st The state.
 * @note CONTROL thread only.
 */
static inline void omx_band_dyn_state_init(struct omx_band_dyn_state *st) { memset(st, 0, sizeof(*st)); }

/**
 * @brief The latency the band adds, in base-rate samples: zero at every rate, engaged or not (§4 L6).
 * @param p The atom.
 * @return 0.
 * @note RT-safe: no work. Thread-safe: pure.
 */
static inline int omx_band_dyn_latency(const struct omx_band_dyn *p) {
  (void)p;
  return 0;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "band-dyn/begin"
/**
 * @brief Resolve the detector once per block, at rate multiplier 1 (the band never oversamples).
 * @param dyn The atom's dynamics.
 * @return The block's detector.
 * @post `poles-in-unit-interval`: both per-stage poles lie in [0, 1].
 * @note RT-safe: one pole derivation per block. Thread-safe: pure.
 */
static inline struct omx_band_dyn_block omx_band_dyn_begin(const struct omx_dyn *dyn) {
  struct omx_band_dyn_block bk;
  bk.ep = omx_dyn_env_params(dyn);
  omx_env_stage_poles(&bk.ep, 1u, &bk.ac, &bk.rc);
  OMX_POST(bk.ac >= 0.0f && bk.ac <= 1.0f && bk.rc >= 0.0f && bk.rc <= 1.0f, "poles-in-unit-interval");
  return bk;
}
#undef OMX_CONTRACT_STAGE

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "band-dyn/offset-db"
/**
 * @brief The signed offset `d`, dB, at detector level `level` (linear, the detector's domain).
 * @param gc The characteristic; `range_db` signed.
 * @param level The detector level, linear, not negative.
 * The level is read in dB by omx_lin_to_db_poly(), no libm call: within 1e-4 dB of
 * omx_lin_to_db() up to +480 dB, so `d` is within `s·1e-4` dB of the libm form, `s` the
 * characteristic's steepest slope (`1 − 1/ratio` above, `ratio − 1` below).
 * @return `d ∈ [min(0, range_db), max(0, range_db)]`.
 * @post `offset-within-range` (§4 L5), `finite`.
 * @note RT-safe and pure, no libm call. The meter re-runs this on the level the last sample
 *       used, so the reading and the stage are one computation.
 */
static inline float omx_band_dyn_offset_db(const struct omx_gaincomp_params *gc, float level) {
  struct omx_gaincomp_params c = *gc;
  const float span = -fabsf(gc->range_db);
  c.range_db = span;
  float gr = omx_gaincomp_db(&c, omx_lin_to_db_poly(level));
  gr = gr < span ? span : gr;
  gr = gr > 0.0f ? 0.0f : gr;
  const float d = gc->range_db > 0.0f ? -gr : gr;
  OMX_POST(d >= fminf(0.0f, gc->range_db) && d <= fmaxf(0.0f, gc->range_db), "offset-within-range");
  OMX_POST(d - d == 0.0f, "finite");
  return d;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "band-dyn/gain"
/**
 * @brief The linear gain `g = 10^(d/20)` at detector level `level`, no libm call.
 * @param gc The characteristic.
 * @param level The detector level, linear, not negative.
 * @return `g`; exactly 1 where `d` is 0.
 * @post `gain-positive-finite`.
 * @note RT-safe and pure: omx_band_dyn_offset_db() and omx_db_to_lin_poly(), no libm call.
 */
static inline float omx_band_dyn_gain(const struct omx_gaincomp_params *gc, float level) {
  const float g = omx_db_to_lin_poly(omx_band_dyn_offset_db(gc, level));
  OMX_POST(g > 0.0f && g - g == 0.0f, "gain-positive-finite");
  return g;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "band-dyn/level"
/**
 * @brief One sample through `B` and the detector: the band's output per leg and the detector level.
 * @param c `B`'s coefficients.
 * @param st The state.
 * @param bk The block's detector.
 * @param xl The left sample.
 * @param xr The right sample (ignored when `stereo` is 0).
 * @param stereo Non-zero for two legs, folded 0.5/0.5 into ONE detector.
 * @param bl Out: `B(xl)`.
 * @param br Out: `B(xr)`, 0 in mono.
 * @return The detector level, linear (omx_env_level() of the cascade).
 * @note RT-safe: two biquads, one cascade step.
 */
static inline float omx_band_dyn_level(const float c[5], struct omx_band_dyn_state *st,
                                       const struct omx_band_dyn_block *bk, float xl, float xr, int stereo,
                                       float *bl, float *br) {
  *bl = omx_biquad(xl, c, st->b_l);
  *br = stereo ? omx_biquad(xr, c, st->b_r) : 0.0f;
  const float k = stereo ? 0.5f * (fabsf(*bl) + fabsf(*br)) : fabsf(*bl);
  return omx_env_step(&st->env, &bk->ep, k, bk->ac, bk->rc);
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "band-dyn/detect"
/**
 * @brief One sample through `B` and the detector: the band's output per leg and the linear gain.
 * @param c `B`'s coefficients.
 * @param st The state.
 * @param gc The characteristic.
 * @param bk The block's detector.
 * @param xl The left sample.
 * @param xr The right sample (ignored when `stereo` is 0).
 * @param stereo Non-zero for two legs, folded 0.5/0.5 into ONE detector.
 * @param bl Out: `B(xl)`.
 * @param br Out: `B(xr)`, 0 in mono.
 * @return `g = 10^(d/20)`: omx_band_dyn_gain() of omx_band_dyn_level(), bit for bit what
 *         omx_band_dyn_run() applies to that sample.
 * @note RT-safe: two biquads, one cascade step, one gain-computer call, no libm call.
 */
static inline float omx_band_dyn_detect(const float c[5], struct omx_band_dyn_state *st,
                                        const struct omx_gaincomp_params *gc,
                                        const struct omx_band_dyn_block *bk, float xl, float xr,
                                        int stereo, float *bl, float *br) {
  return omx_band_dyn_gain(gc, omx_band_dyn_level(c, st, bk, xl, xr, stereo, bl, br));
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "band-dyn/split"
/**
 * @brief The split application: `u` itself at `g == 1`, else `u + (g − 1)·b`.
 * @param u The sample leaving the static section.
 * @param g The linear gain.
 * @param b `B(u)`.
 * @return The dynamic band's sample.
 * @pre `gain-positive`.
 * @note RT-safe: one multiply-add. Thread-safe: pure.
 */
static inline float omx_band_dyn_split(float u, float g, float b) {
  OMX_PRE(g > 0.0f, "gain-positive");
  return g == 1.0f ? u : u + (g - 1.0f) * b;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "band-dyn/flush"
/**
 * @brief Flush every recursive word of the state out of the subnormal range, once per block.
 * @param st The state.
 * @post `no-denormal-state`: every word is a fixed point of omx_flush().
 * @note RT-safe: O(1). Thread-safe on distinct state.
 */
static inline void omx_band_dyn_flush(struct omx_band_dyn_state *st) {
  omx_env_flush(&st->env);
  for (int k = 0; k < 4; k++) {
    st->b_l[k] = omx_flush(st->b_l[k]);
    st->b_r[k] = omx_flush(st->b_r[k]);
  }
  OMX_POST(st->env.stage[OMX_DYN_ENV_STAGES - 1] == omx_flush(st->env.stage[OMX_DYN_ENV_STAGES - 1]) &&
               st->b_l[0] == omx_flush(st->b_l[0]) && st->b_r[0] == omx_flush(st->b_r[0]),
           "no-denormal-state");
}
#undef OMX_CONTRACT_STAGE

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "band-dyn"
/**
 * @brief The engaged loop: detect, compute, apply — split (`u + (g − 1)·b`) or wideband (`g·u`).
 *
 * In chunks of OMX_BAND_DYN_CHUNK samples: the band and the detector sample by sample (the only
 * serial part), then the gain of the whole chunk on the libm-free unit pair, then the
 * application. Every sample takes the gain omx_band_dyn_detect() gives it, bit for bit,
 * whatever the block size. Against the libm pair (omx_lin_to_db(), omx_db_to_lin()) the state
 * is byte-identical — the detector never reads `g` — and `g` is within `s·1e-4 + 2e-5` dB, `s`
 * the characteristic's steepest slope: for the de-esser (above, ratio ≤ 20) within 1.2e-4 dB.
 * @param l The left (or mono) leg.
 * @param r The right leg, or NULL for mono.
 * @param n Frames, at least 1.
 * @param dyn The resolved dynamics; `range_db` signed.
 * @param c `B`'s coefficients.
 * @param st The state.
 * @param wideband Non-zero: the gain lands on the whole signal (the de-esser's wideband mode).
 * @param enabled 0: a no-op that touches no sample and no state word.
 * @pre `finite-in-l`, `finite-in-r`; behind the enable gate: `ratio-at-least-one`,
 *      `knee-not-negative`, `peak-detector`, `no-make-up`.
 * @post `finite-out-l`, `finite-out-r`; `st->gr_min_db` holds the run's deepest `g` in dB.
 * @note RT-safe: O(n), no allocation (four chunk arrays on the stack), no lock, no IO, no libm call
 *       per sample.
 */
static inline void omx_band_dyn_run(float *l, float *r, uint32_t n, const struct omx_dyn *dyn, const float c[5],
                                    struct omx_band_dyn_state *st, int wideband, int enabled) {
  OMX_PRE(omx_block_finite(l, n), "finite-in-l");
  OMX_PRE(omx_block_finite(r, n), "finite-in-r");
  if (!enabled || n == 0u) return;
  OMX_PRE(dyn->gc.ratio >= 1.0f, "ratio-at-least-one");
  OMX_PRE(dyn->gc.knee_db >= 0.0f, "knee-not-negative");
  OMX_PRE(dyn->detect == OMX_DETECT_PEAK, "peak-detector");
  OMX_PRE(dyn->gc.makeup_lin == 1.0f, "no-make-up");
  const struct omx_band_dyn_block bk = omx_band_dyn_begin(dyn);
  const int stereo = r != NULL;
  float gmin = 1.0f;
  for (uint32_t o = 0; o < n; o += OMX_BAND_DYN_CHUNK) {
    const uint32_t m = n - o < OMX_BAND_DYN_CHUNK ? n - o : OMX_BAND_DYN_CHUNK;
    float lv[OMX_BAND_DYN_CHUNK], bl[OMX_BAND_DYN_CHUNK], br[OMX_BAND_DYN_CHUNK], g[OMX_BAND_DYN_CHUNK];
    for (uint32_t i = 0; i < m; i++)
      lv[i] = omx_band_dyn_level(c, st, &bk, l[o + i], stereo ? r[o + i] : 0.0f, stereo, &bl[i], &br[i]);
    for (uint32_t i = 0; i < m; i++) g[i] = omx_band_dyn_gain(&dyn->gc, lv[i]);
    for (uint32_t i = 0; i < m; i++) {
      gmin = g[i] < gmin ? g[i] : gmin;
      l[o + i] = wideband ? l[o + i] * g[i] : omx_band_dyn_split(l[o + i], g[i], bl[i]);
      if (stereo) r[o + i] = wideband ? r[o + i] * g[i] : omx_band_dyn_split(r[o + i], g[i], br[i]);
    }
  }
  st->gr_min_db = omx_gr_block_db(gmin);
  omx_band_dyn_flush(st);
  OMX_POST(omx_block_finite(l, n), "finite-out-l");
  OMX_POST(omx_block_finite(r, n), "finite-out-r");
}

/**
 * @brief Run one dynamic band over its leg(s) IN PLACE, after the band's own static section.
 * @param l The left (or mono) leg.
 * @param r The right leg, or NULL for mono.
 * @param n Frames.
 * @param p The atom.
 * @param st The state.
 * @pre `mode-is-a-member` behind the enable gate; those of omx_band_dyn_run().
 * @note RT-safe. Disabled: no sample and no state word moves.
 */
static inline void omx_band_dyn_process(float *l, float *r, uint32_t n, const struct omx_band_dyn *p,
                                        struct omx_band_dyn_state *st) {
  OMX_PRE(!p->enabled || p->dyn.gc.mode == OMX_DYN_ABOVE || p->dyn.gc.mode == OMX_DYN_BELOW, "mode-is-a-member");
  omx_band_dyn_run(l, r, n, &p->dyn, p->b_c, st, 0, p->enabled);
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_BAND_DYN_H */
