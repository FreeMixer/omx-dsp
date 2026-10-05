// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_mixmatrix.h
 * @brief The summing matrix multiply, `Y = G·X`: dense and sparse, ramped where a coefficient
 *        moves inside the block.
 *
 * Operator decision 2026-10-04: the console's summing is one matrix multiply — X holds the strip
 * blocks, Y the output blocks (main, buses, sends, post-fader taps), G one coefficient per
 * strip×output, each entry the fader law's doing (omx_fader_law.h) and nothing else's. This header
 * is the ONLY place that touches audio for it.
 *
 * `g(s,o,i) = g_prev(s,o) + step(s,o)·i`, `step = (g_cur − g_prev)/n` — omx_ramp.h's own
 * `g(i) = cur + step·i`, inlined rather than called so the per-sample line stays one multiply-add
 * and the compiler's auto-vectoriser sees a plain loop; an entry that does not move (`g_prev ==
 * g_cur`) takes the plain `g_cur·x[i]` path and never pays the ramp's divide. Both forms OVERWRITE
 * every output block (`Y = G·X`, never `Y += G·X`): a caller never pre-zeroes `out`.
 *
 * Dense walks the whole strip×output grid; sparse walks only the entries a caller declares
 * non-zero — a strip that feeds few outputs then costs that few, not every output. Both are one
 * pass over the block, no allocation, no thread, no external BLAS: straight-line C over `restrict`
 * pointers, left to -O2's auto-vectoriser (no intrinsics unless measured necessary). Design:
 * docs/design/specs/2026-10-04-mix-matrix.md.
 */
#ifndef OMX_MIXMATRIX_H
#define OMX_MIXMATRIX_H

#include <stdint.h>

#include "omx_contract.h"

/** @brief One sparse entry of G: the strip×output it connects, and its ramp's two ends. */
struct omx_mixmatrix_entry {
  uint32_t strip; /**< The strip this entry reads — a row of X. */
  uint32_t out;   /**< The output this entry writes — a row of Y. */
  float g_prev;   /**< The coefficient at sample 0 of the block. */
  float g_cur;    /**< The coefficient at the end of the block (and the next block's g_prev). */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "mixmatrix/dense"
/**
 * @brief Dense ramped multiply-accumulate: `out[o][i] = Σ_s g(s,o,i)·in[s][i]`, every output.
 * @param out `n_out` pointers to a block of `n` frames each; each block is OVERWRITTEN, never added to.
 * @param in `n_strips` pointers to a block of `n` frames each.
 * @param g_prev `n_strips·n_out` coefficients, out-major (`g_prev[o*n_strips+s]`): the block's start.
 * @param g_cur Same layout: the block's end, and the next block's `g_prev`.
 * @param n_strips Strips — rows of X.
 * @param n_out Outputs — rows of Y.
 * @param n Frames per block, at least 1.
 * @pre `block-not-empty`, `every-strip-finite`.
 * @post `every-output-finite`.
 * @note RT-safe: fixed-trip-count loops of multiplies and adds, no call, no allocation. The inner
 *       loop over `i` is the only one that runs once per frame; `restrict`-qualified locals let it
 *       vectorise without intrinsics. Thread-safe on disjoint `out` blocks.
 */
static inline void omx_mixmatrix_dense(float *const *out, const float *const *in,
                                        const float *g_prev, const float *g_cur,
                                        uint32_t n_strips, uint32_t n_out, uint32_t n) {
  OMX_PRE(n >= 1u, "block-not-empty");
  for (uint32_t s = 0; s < n_strips; s++) OMX_PRE(omx_block_finite(in[s], n), "every-strip-finite");
  for (uint32_t o = 0; o < n_out; o++) {
    float *restrict yo = out[o];
    for (uint32_t i = 0; i < n; i++) yo[i] = 0.0f;
    for (uint32_t s = 0; s < n_strips; s++) {
      const float *restrict xs = in[s];
      const float gp = g_prev[o * n_strips + s], gc = g_cur[o * n_strips + s];
      if (gp == gc) {
        for (uint32_t i = 0; i < n; i++) yo[i] += gc * xs[i];
      } else {
        const float step = (gc - gp) / (float)n;
        for (uint32_t i = 0; i < n; i++) yo[i] += (gp + step * (float)i) * xs[i];
      }
    }
  }
  for (uint32_t o = 0; o < n_out; o++) OMX_POST(omx_block_finite(out[o], n), "every-output-finite");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "mixmatrix/sparse"
/**
 * @brief Sparse ramped multiply-accumulate: only the declared non-zero entries of G visit a sample.
 * @param out `n_out` pointers to a block of `n` frames each; every block is zeroed first, then
 *        each entry accumulates into its own `out` row (so overall `Y = G·X`, never added to).
 * @param in `n_strips` pointers to a block of `n` frames each.
 * @param entries The non-zero entries of G for this block.
 * @param n_entries Entries in `entries`.
 * @param n_strips Strips — rows of X.
 * @param n_out Outputs — rows of Y.
 * @param n Frames per block, at least 1.
 * @pre `block-not-empty`, `every-strip-finite`, `entries-in-range`.
 * @post `every-output-finite`.
 * @note RT-safe: the same shape as omx_mixmatrix_dense(), the outer loop over `entries` instead of
 *       the full strip×output grid, so a strip that feeds few outputs costs that few. Thread-safe
 *       on disjoint `out` blocks.
 */
static inline void omx_mixmatrix_sparse(float *const *out, const float *const *in,
                                         const struct omx_mixmatrix_entry *entries,
                                         uint32_t n_entries, uint32_t n_strips, uint32_t n_out,
                                         uint32_t n) {
  OMX_PRE(n >= 1u, "block-not-empty");
  for (uint32_t s = 0; s < n_strips; s++) OMX_PRE(omx_block_finite(in[s], n), "every-strip-finite");
#ifdef OMX_CONTRACTS
  int in_range = 1;
  for (uint32_t e = 0; e < n_entries; e++)
    if (entries[e].strip >= n_strips || entries[e].out >= n_out) in_range = 0;
  OMX_PRE(in_range, "entries-in-range");
#endif
  for (uint32_t o = 0; o < n_out; o++) {
    float *restrict yo = out[o];
    for (uint32_t i = 0; i < n; i++) yo[i] = 0.0f;
  }
  for (uint32_t e = 0; e < n_entries; e++) {
    float *restrict yo = out[entries[e].out];
    const float *restrict xs = in[entries[e].strip];
    const float gp = entries[e].g_prev, gc = entries[e].g_cur;
    if (gp == gc) {
      for (uint32_t i = 0; i < n; i++) yo[i] += gc * xs[i];
    } else {
      const float step = (gc - gp) / (float)n;
      for (uint32_t i = 0; i < n; i++) yo[i] += (gp + step * (float)i) * xs[i];
    }
  }
  for (uint32_t o = 0; o < n_out; o++) OMX_POST(omx_block_finite(out[o], n), "every-output-finite");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIXMATRIX_H */
