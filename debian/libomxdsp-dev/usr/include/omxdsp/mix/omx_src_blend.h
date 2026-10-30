// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_src_blend.h
 * @brief The source switch: a strip crossfaded from its live input to a recorded track, in place
 *        on the track buffer, over one block (omx_src_blend()).
 *
 * A step between two unrelated signals is a click, so the switch ramps across the block like
 * every coefficient change: the output leaves the live signal and arrives at the track, and at
 * every sample it is a convex combination of two bounded signals, so it never exceeds the larger
 * of them. The law is linear, not equal-power: the fade is one block long (under 1 ms at any real
 * quantum), so the 3 dB dip in the middle is inaudible, and the linear form is the omx_ramp
 * primitive, which lands exactly on its endpoint.
 *
 * Moved from OpenMixer's engine (its mix_pb.h) with the same name, arguments, arithmetic and
 * contract stage and tokens (`pb/src-blend`), bit for bit.
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_src_blend.h>.
 */
#ifndef OMX_SRC_BLEND_H
#define OMX_SRC_BLEND_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_ramp.h>

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "pb/src-blend"
/**
 * @brief Crossfade a strip from its live input to the recorded track, in place on the track:
 *        `track[i] = live[i] + a(i)·(track[i] − live[i])`, `a` ramped from `*cur` to `tgt`.
 *
 * Settled on the track (`*cur == tgt`, `tgt >= 0.5`) the track is left as it is, with no
 * arithmetic; settled on live the track becomes the live block (or silence when `live` is NULL).
 * A non-finite `tgt` or `*cur` is taken as 0 (omx_ramp_begin()).
 * @param live The live input, `n` samples, or NULL: an unpatched strip's live side is silence.
 * @param track The recorded track, `n` samples, overwritten with the blend.
 * @param n Samples in the block; 0 is a no-op.
 * @param tgt 1 for the track, 0 for live, at the end of the block.
 * @param cur In: where the last block left off. Out: the target (0 when it is not finite).
 * @pre `finite-in`: the track and the live block are finite.
 * @post `finite-out`: the blended track is finite.
 * @note RT-safe: one pass, no allocation, no lock, no IO. Thread-safe over distinct buffers and
 *       cursors.
 */
static inline void omx_src_blend(const float *live, float *track, uint32_t n, float tgt,
                                 float *cur) {
  if (n == 0u) return;
  OMX_PRE(omx_block_finite(track, n) && (live == NULL || omx_block_finite(live, n)), "finite-in");
  struct omx_ramp r = omx_ramp_begin(cur, tgt, n);
  if (r.cur == r.tgt) {
    /* SETTLED — no arithmetic. Fully on track is the ordinary state of an engaged soundcheck
     * and costs nothing; fully live only reaches here on the block a ramp-down finished. */
    if (r.tgt < 0.5f) {
      if (live != NULL) memcpy(track, live, (size_t)n * sizeof(float));
      else memset(track, 0, (size_t)n * sizeof(float));
    }
    omx_ramp_end(r, cur);
    OMX_POST(omx_block_finite(track, n), "finite-out");
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    const float a = omx_ramp_at(r, i);
    const float l = live != NULL ? live[i] : 0.0f;
    track[i] = l + a * (track[i] - l);
  }
  omx_ramp_end(r, cur);
  /* linear-sum: a per-sample ramped CROSS-FADE, `live + a*(track-live)`, is the same
   * coefficient*tap shape the vocabulary's linear-sum law names: a source change cross-fades
   * across the block. */
  OMX_POST(omx_block_finite(track, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_SRC_BLEND_H */
