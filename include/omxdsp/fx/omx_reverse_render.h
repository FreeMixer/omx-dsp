// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_reverse_render.h (formerly the console's mix_reverse_render.h)
 * @brief The PRE-HIT reverse swell, offline, over a whole region.
 *
 * Spec: the native delay reverb spec, Amendment 2026-09-17 §G (the
 * operator's 04:05 ruling: "queue the pre-hit reverse as an offline render lane. It must not
 * affect anything if not enabled"). §B of the same amendment REFUSED this gesture on the live
 * strip and named the arithmetic that refuses it: a swell landing BEFORE the hit needs the dry
 * delayed by the span the mirror covers — 600 ms at the 300 ms default — which no live desk can
 * carry and which delay compensation would then push onto the whole strip.
 *
 * ## What offline lifts: the future exists
 *
 * A causal kernel cannot mirror a tail it has not heard yet, so the live `reverse` voice mirrors
 * inside a ring and the swell lands in the windows AFTER the hit. A recorded region IS the
 * future — every sample exists before one is written — so this kernel plays the region BACKWARD
 * through the reverb network, mirrors the wet back into forward time and mixes it against a dry
 * that NEVER MOVES. There is no strip, no callback and no compensation here, only a buffer.
 *
 * ## The same kernel, and the exact word for "the same"
 *
 * The network is `fx/omx_reverb.h`'s, driven through `omx_reverb_process` with `mix: 1` so what
 * comes back is wet only — no second reverberator, no second summing, no FFT. What is NOT reused
 * is `OMX_REVERB_REVERSE`'s ring: that output stage mirrors inside a causal kernel, and using it
 * on an already-reversed buffer would mirror TWICE and lay a forward-decaying tail before the
 * hit, which is the opposite of the gesture. So the render runs the NETWORK the row's algorithm
 * names — `reverse` and `gated` both name ROOM's comb network and map to ROOM here, while
 * `plate` and `hall` render as themselves, which is the same swell in that network's colour.
 *
 * ## `reverse_ms` is the SWELL'S SPAN, and the window is what enforces it
 *
 * A whole-region mirror of a decaying tail has no end: the swell would creep backwards for the
 * network's entire RT60 and an operator who asked for 300 ms would get whatever `size` implied.
 * So the region is cut, IN THE REVERSED DOMAIN, into blocks of `W = reverse_ms` at a hop of
 * `H = W/2`, each block run through a FRESHLY ZEROED state — which is what stops a response
 * outliving its block — and each block's wet multiplied by a Bartlett triangle of length `W`
 * before it is summed. Triangles at hop `H` add to exactly one at every sample, INCLUDING the
 * first `H`: the block starting at `-H` covers it, and without that block the region's last `H`
 * milliseconds would render at a fading weight (the artefact a "start at zero" loop would have
 * shipped invisibly).
 *
 * A hit at offset `t` inside a block therefore gets between `H` and `W` samples of response,
 * never more than `W`, and after the mirror that response sits in `[hit - W, hit)`. Two
 * consequences are SAMPLE-EXACT and are asserted as such in test/fx/reverse_render.test.c: the wet
 * is exactly 0.0 at and after the hit (the reversed-domain response is causal, so the mirror
 * cannot invent a sample before its cause), and exactly 0.0 more than `W` before it.
 *
 * `predelay_ms` runs in the reversed domain, so it opens a GAP between the end of the swell and
 * the hit — stated because the sign is not obvious, and measured in the test.
 *
 * ## Not RT, and not an audio path
 *
 * This runs on a worker over a file, on a console that is idle after the show. It allocates
 * nothing (every buffer is the caller's), opens nothing and touches no PipeWire object, which is
 * what lets test/fx/reverse_render.test.c compile it standalone against closed-form oracles.
 */
#ifndef OMX_REVERSE_RENDER_H
#define OMX_REVERSE_RENDER_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_reverb.h>

/** @brief The window's floor, ms: the ROW's range, one meaning for `reverseMs` live and offline. */
#define OMX_REVERSE_RENDER_MIN_MS OMX_REVERB_REVERSE_MIN_MS
/** @brief The window's ceiling, ms: the ROW's range. */
#define OMX_REVERSE_RENDER_MAX_MS OMX_REVERB_REVERSE_MAX_MS

/**
 * @brief Every buffer the render needs, owned by the CALLER.
 *
 * `pool` + `st` are one ordinary reverb state, re-laid out per block; `tmp_*` hold one window of
 * audio on its way through the network (so they must be at least `W` frames, which
 * {@link omx_reverse_render_window} answers for a given `reverse_ms` and rate); `wet_*` hold the
 * accumulated wet for the whole region, in the REVERSED domain, and are mirrored on the way out.
 */
struct omx_reverse_render_scratch {
  struct omx_reverb_state *st; /**< One reverb state, re-laid out per block. */
  float *pool;                 /**< The state's float pool. */
  uint32_t pool_len;           /**< Floats in `pool`. */
  float *tmp_l, *tmp_r;        /**< One window of audio on its way through the network. */
  uint32_t tmp_cap;            /**< Frames in each `tmp_*`, at least the window. */
  float *wet_l, *wet_r;        /**< The accumulated wet, reversed domain. */
  uint32_t wet_cap;            /**< Frames in each `wet_*`, at least the region. */
};

/**
 * @brief The window in samples: `reverse_ms` clamped to the row's range, rounded to an EVEN length
 *        so the Bartlett triangle at hop W/2 sums to exactly one.
 * @param reverse_ms The swell's span, ms.
 * @param sr The rate, Hz.
 * @return The window `W`, frames, even and at least 2.
 * @note Thread-safe: arithmetic.
 */
static inline uint32_t omx_reverse_render_window(float reverse_ms, float sr) {
  float ms = reverse_ms;
  if (!(ms >= (float)OMX_REVERSE_RENDER_MIN_MS)) ms = (float)OMX_REVERSE_RENDER_MIN_MS;
  if (ms > (float)OMX_REVERSE_RENDER_MAX_MS) ms = (float)OMX_REVERSE_RENDER_MAX_MS;
  uint32_t h = (uint32_t)((ms * 0.001f) * sr * 0.5f);
  if (h < 1u) h = 1u;
  return 2u * h;
}

/**
 * @brief Which NETWORK an algorithm names. `reverse` and `gated` are ROOM's comb network handed an
 *        output stage (spec §B/§C), and the output stage is what offline replaces — so they render
 *        as ROOM, and the other two render as themselves.
 * @param algorithm The row's algorithm.
 * @return OMX_REVERB_PLATE, OMX_REVERB_HALL or OMX_REVERB_ROOM.
 * @note Thread-safe: arithmetic.
 */
static inline int omx_reverse_render_network(int algorithm) {
  if (algorithm == OMX_REVERB_PLATE) return OMX_REVERB_PLATE;
  if (algorithm == OMX_REVERB_HALL) return OMX_REVERB_HALL;
  return OMX_REVERB_ROOM;
}

/**
 * @brief Render `n` frames IN PLACE: the dry stays exactly where it is and the mirrored wet is
 *        summed onto it at `p->mix`.
 *
 * Returns 0 on success, -1 when an argument or a scratch buffer cannot carry the render (the
 * caller refuses in a code; this kernel has no opinion about which). `p->enabled == 0` is a
 * refusal rather than a silent copy: a render that quietly produced the source back is the worst
 * answer an operation can land. `p->mix == 0` succeeds and leaves the region bit-identical, the
 * same law the insert keeps.
 * @param l The left leg, the whole region.
 * @param r The right leg, the whole region.
 * @param n Frames in the region.
 * @param p The row's reverb parameters.
 * @param sr The rate, Hz.
 * @param sc The caller's buffers.
 * @return 0, or -1 on a refusal.
 * @note NOT RT: a worker over a whole region. No allocation. Thread-safe on distinct buffers.
 */
static inline int omx_reverse_render(float *l, float *r, uint32_t n, const struct omx_reverb *p,
                                     float sr, struct omx_reverse_render_scratch *sc) {
  if (!l || !r || !p || !sc || !sc->st || !sc->pool || !sc->tmp_l || !sc->tmp_r) return -1;
  if (!sc->wet_l || !sc->wet_r) return -1;
  if (n == 0u || !(sr > 0.0f) || !p->enabled) return -1;
  if (sc->wet_cap < n) return -1;
  const uint32_t w = omx_reverse_render_window(p->reverse_ms, sr);
  const uint32_t h = w / 2u;
  if (sc->tmp_cap < w) return -1;

  float mix = omx_unit(p->mix);
  if (mix == 0.0f) return 0; /* bit-identical dry */

  memset(sc->wet_l, 0, (size_t)n * sizeof(float));
  memset(sc->wet_r, 0, (size_t)n * sizeof(float));

  /* The block's parameters are the row's, wet-only and on the network the algorithm names. The
   * mix is applied ONCE, after the mirror, so the dry the operator recorded is never run through
   * the network's own dry path. */
  struct omx_reverb bp = *p;
  bp.mix = 1.0f;
  bp.algorithm = omx_reverse_render_network(p->algorithm);

  const float inv_h = 1.0f / (float)h;
  for (int64_t start = -(int64_t)h; start < (int64_t)n; start += (int64_t)h) {
    /* A fresh state per block is the truncation: no response can outlive the block it was born
     * in, which is what makes the swell's span `reverse_ms` and not the network's RT60. */
    memset(sc->st, 0, sizeof(*sc->st));
    memset(sc->pool, 0, (size_t)sc->pool_len * sizeof(float));
    omx_reverb_state_layout(sc->st, sc->pool, sc->pool_len, sr);
    if (sc->st->_exhausted) return -1;

    for (uint32_t i = 0; i < w; i++) {
      const int64_t k = start + (int64_t)i;              /* reversed-domain index */
      const int64_t src = (int64_t)n - 1 - k;            /* the forward sample it mirrors */
      const int inside = (k >= 0 && k < (int64_t)n);
      sc->tmp_l[i] = inside ? l[src] : 0.0f;
      sc->tmp_r[i] = inside ? r[src] : 0.0f;
    }
    omx_reverb_process(sc->tmp_l, sc->tmp_r, w, &bp, sc->st, sr);
    for (uint32_t i = 0; i < w; i++) {
      const int64_t k = start + (int64_t)i;
      if (k < 0 || k >= (int64_t)n) continue;
      const float tri = (i < h) ? ((float)i * inv_h) : (2.0f - (float)i * inv_h);
      sc->wet_l[k] += tri * sc->tmp_l[i];
      sc->wet_r[k] += tri * sc->tmp_r[i];
    }
  }

  const float dry = 1.0f - mix;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t m = n - 1u - i; /* the mirror: the wet of forward `i` was written at `m` */
    l[i] = dry * l[i] + mix * sc->wet_l[m];
    r[i] = dry * r[i] + mix * sc->wet_r[m];
  }
  return 0;
}

#endif /* OMX_REVERSE_RENDER_H */
