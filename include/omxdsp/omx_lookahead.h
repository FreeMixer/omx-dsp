// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_lookahead.h
 * @brief The look-ahead ring: the cursor law of an exact integer delay, `out[n] = in[n − D]`, the
 *        ring that carries it, and its hold, the minimum of the last `W` pushes.
 *
 * The ring is caller-owned memory of `cap` samples per leg and ONE write cursor the caller keeps;
 * a kernel writes the current sample at the cursor, reads any tap `D < cap` at
 * `omx_lookahead_back(pos, D, cap)` (write-then-read, so `D = 0` is the sample just written) and
 * moves the cursor with `omx_lookahead_fwd`. One cursor serves every leg's buffer and every tap.
 * `struct omx_lookahead` keeps one buffer and its cursor together for a kernel with one tap per
 * leg, and `omx_lookahead_tick` is the two words in one call. Never a fractional read: a
 * look-ahead never reads between samples. Design: docs/design/specs/2026-09-26-dsp-primitives.md
 * §1 row 10.
 */
#ifndef OMX_LOOKAHEAD_H
#define OMX_LOOKAHEAD_H

#include <stdint.h>

#include "omx_contract.h"
#include "omxdsp.h"

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "lookahead/back"
/**
 * @brief The slot written `d` writes before the slot at `pos`.
 * @param pos The write cursor, in [0, cap).
 * @param d The tap in samples, in [0, cap).
 * @param cap The ring's length in samples, at least 1.
 * @return `(pos − d) mod cap`.
 * @pre `tap-inside-the-ring`.
 * @post `index-inside-the-ring`.
 * @invariant `write-cursor-inside-the-ring`.
 * @note RT-safe: one compare, one add. Thread-safe: no state.
 */
static inline uint32_t omx_lookahead_back(uint32_t pos, uint32_t d, uint32_t cap) {
  OMX_PRE(d < cap, "tap-inside-the-ring");
  OMX_INVARIANT(pos < cap, "write-cursor-inside-the-ring");
  const uint32_t i = pos >= d ? pos - d : pos + cap - d;
  OMX_POST(i < cap, "index-inside-the-ring");
  return i;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "lookahead/fwd"
/**
 * @brief The cursor `k` writes after `pos`.
 * @param pos The write cursor, in [0, cap).
 * @param k The writes to advance by, in [0, cap].
 * @param cap The ring's length in samples, at least 1.
 * @return `(pos + k) mod cap`.
 * @pre `step-inside-the-ring`.
 * @post `index-inside-the-ring`.
 * @invariant `write-cursor-inside-the-ring`.
 * @note RT-safe: one add, one compare. Thread-safe: no state.
 */
static inline uint32_t omx_lookahead_fwd(uint32_t pos, uint32_t k, uint32_t cap) {
  OMX_PRE(k <= cap, "step-inside-the-ring");
  OMX_INVARIANT(pos < cap, "write-cursor-inside-the-ring");
  const uint32_t w = pos + k;
  const uint32_t i = w >= cap ? w - cap : w;
  OMX_POST(i < cap, "index-inside-the-ring");
  return i;
}
#undef OMX_CONTRACT_STAGE

/** @brief One ring: `cap` caller-owned floats and the write cursor. */
struct omx_lookahead {
  float *buf;   /**< `cap` floats, owned by the caller. */
  uint32_t cap; /**< The ring's length; a tap reaches at most `cap − 1` writes back. */
  uint32_t pos; /**< The next write, in `[0, cap)`. */
};

OMXDSP_STATE_LAYOUT(lookahead, struct omx_lookahead)

#define OMX_CONTRACT_STAGE "lookahead/init"
/**
 * @brief Arm a ring over `buf`, every slot holding `fill`.
 * @param la The ring.
 * @param buf `cap` floats, owned by the caller.
 * @param cap The ring's length, at least 1.
 * @param fill The value a tap reads before `D` writes have happened; finite.
 * @pre `ring-not-empty`, `finite-fill`.
 * @note CONTROL thread: O(cap). Thread-safe on distinct state.
 */
static inline void omx_lookahead_init(struct omx_lookahead *la, float *buf, uint32_t cap, float fill) {
  OMX_PRE(cap >= 1u && buf != NULL, "ring-not-empty");
  OMX_PRE(fill - fill == 0.0f, "finite-fill");
  la->buf = buf;
  la->cap = cap;
  la->pos = 0u;
  for (uint32_t i = 0; i < cap; i++) buf[i] = fill;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "lookahead/tick"
/**
 * @brief Write `x`, then read the sample written `d` writes ago (`d = 0` returns `x`); the
 *        cursor law's two words over the ring's own cursor.
 * @param la The ring.
 * @param x The input sample; finite.
 * @param d The tap, in samples, in `[0, cap)`.
 * @return `in[n − d]`, bit for bit.
 * @pre `tap-inside-the-ring`, `finite-in`.
 * @post `delayed-copy`: at `d = 0` the read is the write.
 * @invariant `write-cursor-inside-the-ring`.
 * @note RT-safe: one store, one load. Thread-safe on distinct state.
 */
static inline float omx_lookahead_tick(struct omx_lookahead *la, float x, uint32_t d) {
  OMX_PRE(d < la->cap, "tap-inside-the-ring");
  OMX_PRE(x - x == 0.0f, "finite-in");
  const uint32_t w = la->pos;
  la->buf[w] = x;
  const float y = la->buf[omx_lookahead_back(w, d, la->cap)];
  la->pos = omx_lookahead_fwd(w, 1u, la->cap);
  OMX_POST(d != 0u || y == x, "delayed-copy");
  OMX_INVARIANT(la->pos < la->cap, "write-cursor-inside-the-ring");
  return y;
}
#undef OMX_CONTRACT_STAGE

/** @brief The hold: a monotone deque of (value, push index) over caller-owned arrays of `cap`. */
struct omx_lookahead_min {
  float *val;     /**< `cap` floats, owned by the caller; non-decreasing from head to tail. */
  uint32_t *when; /**< `cap` push indices, owned by the caller. */
  uint32_t cap;   /**< The largest window. */
  uint32_t head;  /**< The oldest live entry. */
  uint32_t count; /**< Live entries, in `[0, cap]`. */
  uint32_t t;     /**< The next push index; wraps, compared by difference. */
};

OMXDSP_STATE_LAYOUT(lookahead_min, struct omx_lookahead_min)

#define OMX_CONTRACT_STAGE "lookahead-min/init"
/**
 * @brief Arm an empty hold over `val` and `when`.
 * @param h The hold.
 * @param val `cap` floats, owned by the caller.
 * @param when `cap` indices, owned by the caller.
 * @param cap The largest window, at least 1.
 * @pre `ring-not-empty`.
 * @note CONTROL thread. Thread-safe on distinct state.
 */
static inline void omx_lookahead_min_init(struct omx_lookahead_min *h, float *val, uint32_t *when, uint32_t cap) {
  OMX_PRE(cap >= 1u && val != NULL && when != NULL, "ring-not-empty");
  h->val = val;
  h->when = when;
  h->cap = cap;
  h->head = 0u;
  h->count = 0u;
  h->t = 0u;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "lookahead-min/push"
/**
 * @brief Push `x` and return the minimum of the last `w` pushes, `x` included.
 * @param h The hold.
 * @param x The value; finite.
 * @param w The window, in `[1, cap]`.
 * @return `min(x[n − w + 1 .. n])`, bit for bit one of the pushed values.
 * @pre `window-inside-the-ring`, `finite-in`.
 * @post `at-most-the-push`: the result is not above `x`.
 * @invariant `deque-inside-the-ring`.
 * @note RT-safe: amortised O(1), worst case O(cap) in one call. Thread-safe on distinct state.
 */
static inline float omx_lookahead_min_push(struct omx_lookahead_min *h, float x, uint32_t w) {
  OMX_PRE(w >= 1u && w <= h->cap, "window-inside-the-ring");
  OMX_PRE(x - x == 0.0f, "finite-in");
  while (h->count > 0u) {
    const uint32_t tail = omx_lookahead_fwd(h->head, h->count - 1u, h->cap);
    if (h->val[tail] < x) break;
    h->count--;
  }
  while (h->count > 0u && h->t - h->when[h->head] >= w) {
    h->head = omx_lookahead_fwd(h->head, 1u, h->cap);
    h->count--;
  }
  const uint32_t slot = omx_lookahead_fwd(h->head, h->count, h->cap);
  h->val[slot] = x;
  h->when[slot] = h->t;
  h->count++;
  h->t++;
  const float m = h->val[h->head];
  OMX_POST(m <= x, "at-most-the-push");
  OMX_INVARIANT(h->count <= h->cap && h->head < h->cap, "deque-inside-the-ring");
  return m;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_LOOKAHEAD_H */
