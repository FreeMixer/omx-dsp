// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_width.h
 * @brief The stereo width word: the M/S matrix at a width, a complementary comb or a Haas delay for a
 *        mono source, and the low end summed to mono through an LR4 split of the side.
 *
 * Per sample, `M = (L + R)/2`, `S = (L − R)/2`, then
 *
 *     S' = w·S + (comb ? g·M[n − d] : 0)                 the width, and the complementary comb's side
 *     bass mono:  M' = AP(M),  S'' = HP(S')              the LR4 crossover's all-pass and high band
 *     L = M' + S'',  R = M' − S''                        and with Haas, R = (M' − S'')[n − d]
 *
 * `g` is OMX_WIDTH_COMB_GAIN. The comb's legs are `M ± g·M[n − d]`, a complementary pair whose sum is
 * `2M`: the mono sum is the mid, whatever the comb. With bass mono, `M' + lo(S') + S'' = AP(L)`
 * (omx_xover's partition `lo + hi = AP`): each leg is its own all-pass less the side's low band, so
 * below the corner the side is gone and above it the image is the input's, in phase. Width 1, no
 * pseudo-stereo and no bass mono is not run: the caller's legs pass bit for bit.
 *
 * Contract: no allocation, no lock. The design runs on the control thread (one tan); the state
 * holds the delay as a ring of OMX_WIDTH_RING samples, the longest delay at the highest rate.
 */
#ifndef OMX_WIDTH_H
#define OMX_WIDTH_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "omx_contract.h"
#include "omx_xover.h"
#include <omxdsp/omxdsp_definitions.h>

/** @brief The delay ring, samples: WIDTH_MAX_DELAY_MS at 192 kHz and one more, to a power of two. The
 *         comb's gain is OMX_WIDTH_COMB_GAIN, WIDTH_COMB_GAIN, from omxdsp_definitions.h. */
#define OMX_WIDTH_RING 8192u
_Static_assert(OMX_WIDTH_MAX_DELAY_MS * 192 + 1 <= OMX_WIDTH_RING, "the ring holds the longest delay at 192 kHz");

/** @brief The pseudo-stereo modes, WIDTH_PSEUDOS' order. */
enum omx_width_pseudo { OMX_WIDTH_OFF = 0, OMX_WIDTH_COMB = 1, OMX_WIDTH_HAAS = 2 };

/** @brief The designed word. */
struct omx_width {
  float w;              /**< The side's scale, widthPct/100. */
  int pseudo;           /**< An enum omx_width_pseudo. */
  uint32_t d;           /**< The pseudo-stereo's delay, samples. */
  int bass_mono;        /**< 1 sums the low end to mono. */
  struct omx_xover x;   /**< The bass mono crossover, LR4. */
  int active;           /**< 0 when the word is the identity and is not run. */
};

/** @brief The word's state. A zeroed state is silence. */
struct omx_width_state {
  struct omx_xover_state xs;    /**< The side's split. */
  struct omx_xover_ap_state ap; /**< The mid's all-pass. */
  float ring[OMX_WIDTH_RING];   /**< The delayed signal: the mid (comb) or the right leg (Haas). */
  uint32_t pos;                 /**< The ring's write index. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "width/delay"
/**
 * @brief The pseudo-stereo's delay in samples: `pseudo_ms` at `sr`, rounded to the nearest sample.
 * @param pseudo_ms The delay, ms; held inside [0, WIDTH_MAX_DELAY_MS].
 * @param sr The rate, Hz.
 * @return The samples, below OMX_WIDTH_RING.
 * @post `inside-the-ring`.
 * @note RT-safe: arithmetic. Pure.
 */
static inline uint32_t omx_width_delay(float pseudo_ms, float sr) {
  const float top = (float)OMX_WIDTH_MAX_DELAY_MS;
  const float ms = pseudo_ms > top ? top : pseudo_ms > 0.0f ? pseudo_ms : 0.0f;
  const uint32_t d = (uint32_t)lrintf(ms * 0.001f * sr);
  OMX_POST(d < OMX_WIDTH_RING, "inside-the-ring");
  return d < OMX_WIDTH_RING ? d : OMX_WIDTH_RING - 1u;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "width/design"
/**
 * @brief Design the word.
 * @param p Receives the design.
 * @param w The side's scale, at least 0.
 * @param pseudo An enum omx_width_pseudo.
 * @param d The delay, samples, below OMX_WIDTH_RING.
 * @param bass_mono 1 to sum the low end to mono below `bass_hz`.
 * @param bass_hz The crossover's corner, Hz, inside (0, sr/2).
 * @param sr The rate, Hz.
 * @pre `width-not-negative`.
 * @note Control thread: one tan when bass mono is on. Thread-safe: writes only `p`.
 */
static inline void omx_width_design(struct omx_width *p, float w, int pseudo, uint32_t d, int bass_mono, double bass_hz,
                                    double sr) {
  OMX_PRE(w >= 0.0f, "width-not-negative");
  p->w = w;
  p->pseudo = pseudo == OMX_WIDTH_COMB || pseudo == OMX_WIDTH_HAAS ? pseudo : OMX_WIDTH_OFF;
  p->d = d < OMX_WIDTH_RING ? d : OMX_WIDTH_RING - 1u;
  p->bass_mono = bass_mono ? 1 : 0;
  if (p->bass_mono) omx_xover_design(&p->x, 4u, bass_hz, sr);
  p->active = w != 1.0f || p->pseudo != OMX_WIDTH_OFF || p->bass_mono;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "width/process"
/**
 * @brief One block through the word, both legs in place.
 * @param l The left leg.
 * @param r The right leg.
 * @param n Frames.
 * @param p The design.
 * @param st The state.
 * @pre `finite-in`.
 * @post `finite-out`.
 * @note RT-safe: per sample a few multiplies, and with bass mono four double sections and one
 *       all-pass; no allocation. Thread-safe on distinct state.
 */
static inline void omx_width_process(float *l, float *r, uint32_t n, const struct omx_width *p, struct omx_width_state *st) {
  if (!p->active || n == 0u) return;
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
  const uint32_t mask = OMX_WIDTH_RING - 1u;
  for (uint32_t i = 0; i < n; i++) {
    float m = 0.5f * (l[i] + r[i]);
    float s = p->w * (0.5f * (l[i] - r[i]));
    if (p->pseudo == OMX_WIDTH_COMB) {
      st->ring[st->pos] = m;
      s += OMX_WIDTH_COMB_GAIN * st->ring[(st->pos - p->d) & mask];
      st->pos = (st->pos + 1u) & mask;
    }
    if (p->bass_mono) {
      float lo, hi;
      omx_xover_process(&s, &lo, &hi, 1u, &p->x, &st->xs);
      omx_xover_allpass(&m, 1u, &p->x, &st->ap);
      s = hi;
    }
    l[i] = m + s;
    r[i] = m - s;
    if (p->pseudo == OMX_WIDTH_HAAS) {
      st->ring[st->pos] = r[i];
      r[i] = st->ring[(st->pos - p->d) & mask];
      st->pos = (st->pos + 1u) & mask;
    }
  }
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_WIDTH_H */
