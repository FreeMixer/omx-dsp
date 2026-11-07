/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_live_shell.h — what every corrector's live shell (omx_hrp_lv2.h, omx_fbs_lv2.h) does the same way, once:
 * the audio passed bit-exact, the input fed to the analysis ring and counted, the pass counters a face reads as
 * "in flight", the unconsumed audio a pass is due on, and a report slot read by value.
 */
#ifndef OMX_LIVE_SHELL_H
#define OMX_LIVE_SHELL_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/analysis/omx_band_report.h>
#include <omxdsp/analysis/omx_hrp_correct.h>
#include <omxdsp/analysis/omx_rta.h>
#include <omxdsp/omx_contract.h>

/**
 * @brief The audio as it came: `out` is `in`, byte for byte (a shell not ready, or in `report`).
 * @param in The input block.
 * @param out The output block; may be `in`.
 * @param n Frames.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "live-shell/pass-through"
static inline void omx_live_pass_through(const float *in, float *out, uint32_t n) {
  if (out != in) memcpy(out, in, (size_t)n * sizeof(float));
  OMX_POST(out == in || memcmp(out, in, (size_t)n * sizeof(float)) == 0, "bit-exact");
}
#undef OMX_CONTRACT_STAGE

/** A shell's feed: the analysis ring, the frames written and consumed, the passes asked and answered. */
typedef struct {
  struct omx_ring ring;
  _Atomic uint64_t written;
  _Atomic uint64_t consumed;
  _Atomic uint32_t asked;
  _Atomic uint32_t answered;
} OmxLiveFeed;

/**
 * @brief The block out: the input as it came in `report`, through the cascade in `cut`.
 * @param cascade The bands in force.
 * @param report Nonzero for `report`.
 * @param in The input block.
 * @param out The output block; may be `in`.
 * @param n Frames.
 * @thread audio
 */
static inline void omx_live_out(OmxHrpCascade *cascade, int report, const float *in, float *out, uint32_t n) {
  if (report)
    omx_live_pass_through(in, out, n);
  else
    omx_hrp_cascade_apply(cascade, in, out, n);
}

/**
 * @brief Take the oldest unconsumed hop: read the `n` frames ending where it ends, and consume it. A reader the
 *        writer has lapped skips to the newest whole hop (late is fine; a backlog is not).
 * @param f The feed.
 * @param out Where the `n` frames go.
 * @param n Frames read, ending at the hop's end (a hop, or a longer window).
 * @param hop Frames the pass consumes.
 * @return 1 when a hop was taken, 0 when no whole hop is unconsumed.
 * @thread worker
 */
static inline int omx_live_take(OmxLiveFeed *f, float *out, uint32_t n, uint32_t hop) {
  const uint64_t written = atomic_load_explicit(&f->written, memory_order_acquire);
  uint64_t consumed = atomic_load_explicit(&f->consumed, memory_order_acquire);
  if (written - consumed < (uint64_t)hop) return 0;
  uint64_t back = written - (consumed + hop);
  if (back + (uint64_t)n > (uint64_t)OMX_RTA_RING_SIZE) {
    consumed = written - hop;
    back = 0u;
  }
  omx_ring_read_at(&f->ring, out, n, (uint32_t)back);
  atomic_store_explicit(&f->consumed, consumed + hop, memory_order_release);
  return 1;
}

/**
 * @brief Should run() process this block? An empty block is nothing to do; a shell not ready passes it as it came.
 * @param ready The shell is bound to its rate and buffers.
 * @param in The input block.
 * @param out The output block; may be `in`.
 * @param n Frames.
 * @return 1 when the caller processes the block, 0 when it is done with it.
 * @thread audio
 */
static inline int omx_live_block(int ready, const float *in, float *out, uint32_t n) {
  if (!in || !out || n == 0u) return 0;
  if (ready) return 1;
  omx_live_pass_through(in, out, n);
  return 0;
}

/**
 * @brief The input into the analysis ring, and the frames ever written counted (release order).
 * @param ring The ring.
 * @param written The writer's cursor.
 * @param in The input block.
 * @param n Frames.
 * @thread audio
 */
static inline void omx_live_feed(struct omx_ring *ring, _Atomic uint64_t *written, const float *in, uint32_t n) {
  omx_ring_write(ring, in, n);
  atomic_fetch_add_explicit(written, n, memory_order_release);
}

/**
 * @brief Frames written and not yet consumed by a pass.
 * @param written The writer's cursor.
 * @param consumed The worker's cursor.
 * @return The difference, read with acquire order.
 * @thread audio
 */
static inline uint64_t omx_live_unconsumed(const _Atomic uint64_t *written, const _Atomic uint64_t *consumed) {
  return atomic_load_explicit(written, memory_order_acquire) - atomic_load_explicit(consumed, memory_order_acquire);
}

/**
 * @brief Count one pass, asked or answered (release order, so what was written before it is visible).
 * @param counter The counter; NULL is a no-op.
 * @thread audio
 */
static inline void omx_live_count(_Atomic uint32_t *counter) {
  if (counter) atomic_fetch_add_explicit(counter, 1u, memory_order_release);
}

/**
 * @brief Is a pass in flight: the two counters differ (they wrap together at 2^32)?
 * @param asked Passes asked for.
 * @param answered Passes answered.
 * @return 1 or 0.
 * @thread audio
 */
static inline int omx_live_in_flight(const _Atomic uint32_t *asked, const _Atomic uint32_t *answered) {
  return atomic_load_explicit(asked, memory_order_acquire) != atomic_load_explicit(answered, memory_order_acquire);
}

/**
 * @brief Slot k of a report, by value; past the end, a free slot.
 * @param report The report.
 * @param n Its slots.
 * @param k The slot.
 * @return The six words.
 * @thread audio
 */
static inline OmxBandReport omx_live_report_at(const OmxBandReport *report, uint32_t n, uint32_t k) {
  return (report && k < n) ? report[k] : omx_band_report_free();
}

/**
 * @brief Every slot of a report free.
 * @param report The report.
 * @param n Its slots.
 * @thread audio
 */
static inline void omx_live_report_clear(OmxBandReport *report, uint32_t n) {
  for (uint32_t k = 0; k < n; k++) report[k] = omx_band_report_free();
}

#endif /* OMX_LIVE_SHELL_H */
