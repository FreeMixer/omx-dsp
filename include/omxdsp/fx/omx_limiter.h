// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_limiter.h (formerly mix_limiter.h)
 * @brief The precision limiter: a look-ahead, true-peak, stereo-linked brickwall.
 *
 * Spec: docs/design/specs/2026-09-27-precision-limiter.md §1. Composed from the library words only:
 * {@link omx_truepeak_block} is the detector, {@link omx_gaincomp_db} the request (ABOVE, knee 0, at
 * the ratio travel's ceiling, scaled by `R/(R−1)`), {@link omx_lookahead_min_push} the hold,
 * {@link omx_env_step} the release on the gain depth, {@link omx_lookahead_tick} the attack box and the
 * delayed audio. `y[n] = clamp(s[n]·x[n − T], ±c)`, `T = U + D`.
 *
 * Contract: NO PipeWire, NO napi, NO allocation, NO lock, NO libc beyond <math.h>/<string.h>. The
 * rings are caller-owned; the atom {@link omx_limiter} is a per-block snapshot.
 */
#ifndef OMX_MIX_LIMITER_H
#define OMX_MIX_LIMITER_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_contract_limits.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_envelope.h>
#include <omxdsp/omx_gaincomp.h>
#include <omxdsp/omx_lookahead.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_truepeak.h>
#include <omxdsp/omx_units.h>

/** @brief The box's unit: a gain of 1 is `2³¹` in the exact integer sum. */
#define OMX_LIMITER_BOX_ONE 2147483648.0

/** @brief The limiter's live controls. */
struct omx_limiter {
  int enabled;      /**< 0: a no-op that touches no sample and no state word. */
  float ceiling_db; /**< The true-peak ceiling, dBFS, in the declared travel. */
  float release_ms; /**< The release time constant, ms, in the declared travel. */
};

/** @brief The limiter's working state; the rings point into caller-owned memory. */
struct omx_limiter_state {
  struct omx_truepeak tp_l;      /**< The left leg's detector. */
  struct omx_truepeak tp_r;      /**< The right leg's detector. */
  struct omx_lookahead aud_l;    /**< The left leg's audio delay, tap `T`. */
  struct omx_lookahead aud_r;    /**< The right leg's audio delay, tap `T`. */
  struct omx_lookahead box;      /**< The released gain's last `D + 1` values. */
  struct omx_lookahead_min hold; /**< The request's window minimum, `W = D + 2`. */
  struct omx_env env;            /**< The release cascade, on the gain depth. */
  int64_t sum;                   /**< The box's exact sum, in units of 2⁻³¹. */
  uint32_t d;                    /**< The look-ahead `D`, frames. */
  uint32_t t;                    /**< The declared latency `T = U + D`, frames. */
  float rate;                    /**< The rate the state was armed at, Hz. */
  float blk_gain;                /**< The last enabled block's smallest applied gain `s`, linear. */
  float blk_peak;                /**< The last enabled block's largest detector reading `p`, linear. */
};

/**
 * @brief The look-ahead `D` in frames: `round(ms · rate / 1000)`, at least 1.
 * @param lookahead_ms The look-ahead, ms.
 * @param rate The sample rate, Hz.
 * @return `D`.
 * @note RT-safe and thread-safe: arithmetic.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "limiter/lookahead-frames"
static inline uint32_t omx_limiter_lookahead_frames(float lookahead_ms, float rate) {
  const long d = lrintf(lookahead_ms * rate / 1000.0f);
  const uint32_t frames = d < 1 ? 1u : (uint32_t)d;
  OMX_POST(frames >= 1u, "at-least-one-frame");
  return frames;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The ring length every declared look-ahead fits at `rate`: `D_max + U + 2`.
 * @param rate The sample rate, Hz.
 * @return Elements per ring.
 * @note RT-safe and thread-safe: arithmetic.
 */
static inline uint32_t omx_limiter_cap(float rate) {
  return omx_limiter_lookahead_frames(OMX_LIMITER_LOOKAHEAD_MS_MAX, rate) + omx_truepeak_delay() + 2u;
}

/**
 * @brief The caller's float memory for one state: four rings of `cap` (two audio, box, hold values).
 * @param cap The ring length.
 * @return Floats.
 * @note RT-safe and thread-safe: arithmetic. The hold's indices are a separate `cap` of uint32_t.
 */
static inline uint32_t omx_limiter_mem_floats(uint32_t cap) { return 4u * cap; }

/**
 * @brief The latency the armed state declares, frames: `T = U + D`.
 * @param st The state.
 * @return `T`.
 * @note RT-safe and thread-safe: a read.
 */
static inline uint32_t omx_limiter_latency(const struct omx_limiter_state *st) { return st->t; }

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "limiter/init"
/**
 * @brief Arm a state for `lookahead_ms` at `rate` over caller-owned memory; the gain starts at 1.
 * @param st The state.
 * @param lookahead_ms The look-ahead, ms.
 * @param rate The sample rate, Hz.
 * @param mem omx_limiter_mem_floats(cap) floats.
 * @param idx `cap` indices.
 * @param cap The ring length, at least `D + U + 2`.
 * @pre `rate-is-declared`, `lookahead-in-travel`, `ring-holds-the-latency`.
 * @note CONTROL thread: O(cap). Thread-safe on distinct state.
 */
static inline void omx_limiter_init(struct omx_limiter_state *st, float lookahead_ms, float rate, float *mem,
                                    uint32_t *idx, uint32_t cap) {
  OMX_PRE(omx_rate_is_declared(rate), "rate-is-declared");
  OMX_PRE(lookahead_ms >= OMX_LIMITER_LOOKAHEAD_MS_MIN && lookahead_ms <= OMX_LIMITER_LOOKAHEAD_MS_MAX,
          "lookahead-in-travel");
  memset(st, 0, sizeof(*st));
  st->rate = rate;
  st->d = omx_limiter_lookahead_frames(lookahead_ms, rate);
  st->t = omx_truepeak_delay() + st->d;
  OMX_PRE(cap >= st->t + 2u, "ring-holds-the-latency");
  omx_truepeak_init(&st->tp_l);
  omx_truepeak_init(&st->tp_r);
  omx_lookahead_init(&st->aud_l, mem, cap, 0.0f);
  omx_lookahead_init(&st->aud_r, mem + cap, cap, 0.0f);
  omx_lookahead_init(&st->box, mem + 2u * cap, cap, 1.0f);
  omx_lookahead_min_init(&st->hold, mem + 3u * cap, idx, cap);
  st->sum = (int64_t)(st->d + 1u) * (int64_t)OMX_LIMITER_BOX_ONE;
  st->blk_gain = 1.0f;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "limiter/request"
/**
 * @brief The brickwall request at true peak `peak`: the gain computer at knee 0, scaled by `R/(R−1)`.
 * @param gc The characteristic: ABOVE, knee 0, threshold the ceiling, ratio above 1.
 * @param peak The true peak, linear, above the ceiling.
 * @return `min(1, c / peak)` to float precision.
 * @pre `ratio-above-one`.
 * @post `at-most-unity`.
 * @note RT-safe: one omx_log10f() (no libm call), one `powf`. Thread-safe: pure.
 */
static inline float omx_limiter_request(const struct omx_gaincomp_params *gc, float peak) {
  OMX_PRE(gc->ratio > OMX_COMP_RATIO_MIN, "ratio-above-one");
  const float k = gc->ratio / (gc->ratio - 1.0f);
  const float r = omx_db_to_lin(omx_gaincomp_db(gc, omx_lin_to_db(peak)) * k);
  OMX_POST(r <= 1.0f, "at-most-unity");
  return r;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "limiter"
/**
 * @brief Limit one block IN PLACE, both legs on one gain.
 * @param l The left (or mono) leg.
 * @param r The right leg, or NULL for mono.
 * @param n Frames.
 * @param p The live controls.
 * @param st The armed state.
 * @pre `finite-in-l`, `finite-in-r`; behind the enable gate: `ceiling-in-travel`, `release-in-travel`.
 * @post `finite-out-l`, `finite-out-r`, `at-most-the-ceiling`; the cascade flushed by omx_env_flush();
 *       `blk_gain`/`blk_peak` hold this block's smallest applied gain and largest true-peak reading —
 *       the values the engine publishes as the row's meters, computed here once.
 * @invariant bypass identity: disabled, no sample and no state word moves.
 * @note RT-safe: O(n) plus the hold's amortised deque, `2·4·OMX_TRUEPEAK_CHUNK` floats of stack. Thread-safe
 *       on distinct state.
 */
static inline void omx_limiter_process(float *l, float *r, uint32_t n, const struct omx_limiter *p,
                                       struct omx_limiter_state *st) {
  OMX_PRE(omx_block_finite(l, n), "finite-in-l");
  OMX_PRE(omx_block_finite(r, n), "finite-in-r");
  if (!p->enabled || n == 0u) return;
  OMX_PRE(p->ceiling_db >= OMX_LIMITER_CEILING_DB_MIN && p->ceiling_db <= OMX_LIMITER_CEILING_DB_MAX,
          "ceiling-in-travel");
  OMX_PRE(p->release_ms >= OMX_LIMITER_RELEASE_MS_MIN && p->release_ms <= OMX_LIMITER_RELEASE_MS_MAX,
          "release-in-travel");
  const struct omx_gaincomp_params gc = {OMX_DYN_ABOVE, p->ceiling_db, OMX_COMP_RATIO_MAX, 0.0f, 0.0f, 1.0f};
  const struct omx_env_params ep = {0.0f, omx_pole_from_time_ms(p->release_ms, st->rate), OMX_DETECT_PEAK};
  float ac, rc;
  omx_env_stage_poles(&ep, 1u, &ac, &rc);
  const float c = omx_db_to_lin(p->ceiling_db);
  const double denom = (double)(st->d + 1u) * OMX_LIMITER_BOX_ONE;
  float pl[OMX_TRUEPEAK_CHUNK], pr[OMX_TRUEPEAK_CHUNK];
  float blk_gain = 1.0f, blk_peak = 0.0f;
  for (uint32_t done = 0; done < n;) {
    const uint32_t k = n - done < OMX_TRUEPEAK_CHUNK ? n - done : OMX_TRUEPEAK_CHUNK;
    omx_truepeak_block(&st->tp_l, l + done, k, pl);
    if (r) omx_truepeak_block(&st->tp_r, r + done, k, pr);
    for (uint32_t i = 0; i < k; i++) {
      const float pk = r ? fmaxf(pl[i], pr[i]) : pl[i];
      const float want = pk > c ? omx_limiter_request(&gc, pk) : 1.0f;
      const float m = omx_lookahead_min_push(&st->hold, want, st->d + 2u);
      const float e = 1.0f - omx_env_step(&st->env, &ep, 1.0f - m, ac, rc);
      const float old = omx_lookahead_tick(&st->box, e, st->d + 1u);
      st->sum += (int64_t)floor((double)e * OMX_LIMITER_BOX_ONE) - (int64_t)floor((double)old * OMX_LIMITER_BOX_ONE);
      const float s = (float)((double)st->sum / denom);
      blk_gain = fminf(blk_gain, s);
      blk_peak = fmaxf(blk_peak, pk);
      const float yl = s * omx_lookahead_tick(&st->aud_l, l[done + i], st->t);
      l[done + i] = fminf(c, fmaxf(-c, yl));
      if (r) {
        const float yr = s * omx_lookahead_tick(&st->aud_r, r[done + i], st->t);
        r[done + i] = fminf(c, fmaxf(-c, yr));
      }
    }
    done += k;
  }
  st->blk_gain = blk_gain;
  st->blk_peak = blk_peak;
  omx_env_flush(&st->env);
  OMX_POST(omx_block_finite(l, n), "finite-out-l");
  OMX_POST(omx_block_finite(r, n), "finite-out-r");
  OMX_POST(omx_block_absmax(l, n) <= c && (r == NULL || omx_block_absmax(r, n) <= c), "at-most-the-ceiling");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_LIMITER_H */
