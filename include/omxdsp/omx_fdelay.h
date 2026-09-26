// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_fdelay.h
 * @brief The fractional delay line: one caller-owned ring read between samples through a
 *        Lagrange kernel whose ORDER (3 or 5) is a constructor parameter.
 *
 * A whole-sample delay is an exact copy (the kernel is skipped), a zero tap is skipped rather
 * than multiplied, and the ring is sized for the longest delay plus the kernel's reach.
 * Design: docs/design/specs/2026-09-20-fractional-delay-line.md (RULED) and
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 13.
 */
#ifndef OMX_FDELAY_H
#define OMX_FDELAY_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "omx_contract.h"
#include "omx_denormal.h"

/** @brief The highest Lagrange order the kernel's tap array is built for. */
#define OMX_FDELAY_MAX_ORDER 5
/** @brief Taps at the maximum order: an order-N kernel reads N+1 samples. */
#define OMX_FDELAY_MAX_TAPS (OMX_FDELAY_MAX_ORDER + 1)

/** @brief A construction's answer: a refusal is a code and leaves the line unarmed. */
enum omx_fdelay_code {
  OMX_FDELAY_OK = 0,             /**< Armed. */
  OMX_FDELAY_BAD_ORDER = 1,      /**< Not an odd order in [3, OMX_FDELAY_MAX_ORDER]. */
  OMX_FDELAY_RING_TOO_SMALL = 2, /**< No ring, or one too small for the kernel's reach at any delay. */
};

/**
 * @brief One fractional delay line over a caller-owned ring. `order` 0 means UNARMED.
 */
struct omx_fdelay {
  float *ring;   /**< The caller's `cap` floats, allocated on insert, never here. */
  uint32_t cap;  /**< Slots in the ring. */
  uint32_t wpos; /**< The write cursor, in [0, cap). */
  int order;     /**< The Lagrange order, 3 or 5; 0 when unarmed. */
};

/**
 * @brief The state's size, for a host that lays the state out itself.
 * @return `sizeof(struct omx_fdelay)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_fdelay_state_size(void) { return sizeof(struct omx_fdelay); }

/**
 * @brief The state's alignment, for a host that lays the state out itself.
 * @return `_Alignof(struct omx_fdelay)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_fdelay_state_align(void) { return _Alignof(struct omx_fdelay); }

/**
 * @brief Samples of context the kernel reads behind the read point: (order − 1) / 2.
 * @param order The Lagrange order.
 * @return The lookbehind, 0 for an unarmed order.
 * @note RT-safe and thread-safe: arithmetic.
 */
static inline uint32_t omx_fdelay_lookbehind(int order) {
  return order <= 0 ? 0u : (uint32_t)((order - 1) / 2);
}

/**
 * @brief Samples of context the kernel reads ahead of the read point: (order + 1) / 2.
 * @param order The Lagrange order.
 * @return The lookahead, 0 for an unarmed order.
 * @note RT-safe and thread-safe: arithmetic.
 */
static inline uint32_t omx_fdelay_lookahead(int order) {
  return order <= 0 ? 0u : (uint32_t)((order + 1) / 2);
}

/**
 * @brief The shortest FRACTIONAL delay an order can read, (order − 1) / 2 samples; a whole
 *        delay has no floor.
 * @param order The Lagrange order.
 * @return Samples.
 * @note RT-safe and thread-safe: arithmetic.
 */
static inline float omx_fdelay_min_delay(int order) {
  return (float)omx_fdelay_lookbehind(order);
}

/**
 * @brief The longest delay a ring can serve at an order.
 * @param cap Slots in the ring.
 * @param order The Lagrange order.
 * @return Samples; 0 when the ring cannot hold the kernel's reach.
 * @note RT-safe and thread-safe: arithmetic.
 */
static inline float omx_fdelay_max_delay(uint32_t cap, int order) {
  uint32_t reach = omx_fdelay_lookbehind(order) + 2u;
  return cap <= reach ? 0.0f : (float)(cap - reach);
}

/**
 * @brief The ring a line needs to serve a delay: the delay, the kernel's reach behind it and the
 *        slot the read point falls between. Allocate this on insert.
 * @param max_delay_samples The longest delay wanted, samples.
 * @param order The Lagrange order.
 * @return Slots.
 * @note RT-safe and thread-safe: one `ceilf`.
 */
static inline uint32_t omx_fdelay_cap_for(float max_delay_samples, int order) {
  float d = max_delay_samples > 0.0f ? max_delay_samples : 0.0f;
  return (uint32_t)ceilf(d) + omx_fdelay_lookbehind(order) + 2u;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fdelay/init"
/**
 * @brief Arm a line over a caller-owned ring; the ring is not zeroed.
 * @param l The line.
 * @param ring `cap` floats the caller owns.
 * @param cap Slots in the ring.
 * @param order The Lagrange order: odd, in [3, OMX_FDELAY_MAX_ORDER].
 * @return OMX_FDELAY_OK, or the code that refused and left the line unarmed.
 * @pre `order-is-odd-and-within-the-kernel`, `a-line-owns-no-memory-of-its-own`,
 *      `the-ring-holds-the-kernels-reach`.
 * @post `an-armed-line-carries-what-it-was-given`.
 * @note RT-safe: stores only. Thread-safe on distinct state. Called on the control thread.
 */
static inline enum omx_fdelay_code omx_fdelay_init(struct omx_fdelay *l, float *ring, uint32_t cap,
                                                   int order) {
  OMX_PRE(order >= 3 && order <= OMX_FDELAY_MAX_ORDER && (order % 2) == 1,
          "order-is-odd-and-within-the-kernel");
  OMX_PRE(ring != 0, "a-line-owns-no-memory-of-its-own");
  OMX_PRE(ring == 0 || omx_fdelay_max_delay(cap, order) > 0.0f,
          "the-ring-holds-the-kernels-reach");
  l->ring = 0;
  l->cap = 0u;
  l->wpos = 0u;
  l->order = 0;
  if (order < 3 || order > OMX_FDELAY_MAX_ORDER || (order % 2) != 1) return OMX_FDELAY_BAD_ORDER;
  if (ring == 0 || omx_fdelay_max_delay(cap, order) <= 0.0f) return OMX_FDELAY_RING_TOO_SMALL;
  l->ring = ring;
  l->cap = cap;
  l->order = order;
  OMX_POST(l->order == order && l->cap == cap, "an-armed-line-carries-what-it-was-given");
  return OMX_FDELAY_OK;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "fdelay/kernel"
/**
 * @brief The Lagrange coefficients of `order` at fraction `f`, measured from the tap at local
 *        index (order − 1) / 2: `c[k] = Π(j≠k) (f − (j − off)) / (k − j)`, accumulated in double.
 * @param order The Lagrange order, in [1, OMX_FDELAY_MAX_ORDER].
 * @param f The fraction, in [0, 1).
 * @param c `order + 1` coefficients.
 * @pre `kernel-order-is-within-the-tap-array`, `fraction-is-inside-one-sample`.
 * @post `the-kernel-is-unity-at-dc`: the coefficients sum to 1 within 1e-5;
 *       `an-order-3-kernel-is-inside-the-declared-l1-norm`: at order 3, Σ|c| is at most
 *       `OMX_FDELAY_READ_L1_NORM` (the declared bound of the order-3 read's worst fraction).
 * @note RT-safe: bounded products, no call. Thread-safe: pure.
 */
static inline void omx_fdelay_lagrange(int order, float f, float *c) {
  OMX_PRE(order >= 1 && order <= OMX_FDELAY_MAX_ORDER, "kernel-order-is-within-the-tap-array");
  OMX_PRE(f >= 0.0f && f < 1.0f, "fraction-is-inside-one-sample");
  const int off = (int)omx_fdelay_lookbehind(order);
  for (int k = 0; k <= order; k++) {
    double num = 1.0, den = 1.0;
    for (int j = 0; j <= order; j++) {
      if (j == k) continue;
      num *= (double)f - (double)(j - off);
      den *= (double)(k - j);
    }
    c[k] = (float)(num / den);
  }
#ifdef OMX_CONTRACTS
  {
    double sum = 0.0, l1 = 0.0;
    for (int k = 0; k <= order; k++) { sum += (double)c[k]; l1 += fabs((double)c[k]); }
    OMX_POST(fabs(sum - 1.0) < 1e-5, "the-kernel-is-unity-at-dc");
    OMX_POST(order != 3 || l1 <= (double)OMX_FDELAY_READ_L1_NORM + 1e-6,
             "an-order-3-kernel-is-inside-the-declared-l1-norm");
  }
#endif
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Write one sample and advance the cursor; the line holds the last `cap` samples.
 * @param l The line; an unarmed line ignores the write.
 * @param x The sample.
 * @note RT-safe: one store. Thread-safe on distinct state.
 */
static inline void omx_fdelay_write(struct omx_fdelay *l, float x) {
  if (l->ring == 0 || l->cap == 0u) return;
  l->ring[l->wpos] = x;
  l->wpos = (l->wpos + 1u == l->cap) ? 0u : l->wpos + 1u;
}

#define OMX_CONTRACT_STAGE "fdelay/clamp"
/**
 * @brief The delay the line will actually deliver for a request: clamped into the ring, a
 *        non-finite request read as 0, a fractional request below the kernel's reach raised to
 *        the shortest whole delay it can read.
 * @param l The line.
 * @param delay The requested delay, samples.
 * @return The delivered delay, samples; the number a latency row publishes.
 * @post `clamped-delay-is-inside-the-ring`.
 * @note RT-safe: compares and one `floorf`. Thread-safe: a read of the state.
 */
static inline float omx_fdelay_latency(const struct omx_fdelay *l, float delay) {
  if (l->order == 0 || l->cap == 0u) return 0.0f;
  float d = (delay - delay == 0.0f) ? delay : 0.0f;
  const float maxd = omx_fdelay_max_delay(l->cap, l->order);
  if (d < 0.0f) d = 0.0f;
  if (d > maxd) d = maxd;
  const float mind = omx_fdelay_min_delay(l->order);
  if (d < mind && d != floorf(d)) d = mind;
  OMX_POST(d >= 0.0f && d <= maxd, "clamped-delay-is-inside-the-ring");
  return d;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Decompose a delay into the whole-sample distance behind the newest sample and the
 *        kernel's coordinate; the one place a delay is split.
 * @param l The line.
 * @param delay The requested delay, samples.
 * @param id The whole-sample distance.
 * @param kf The kernel's fraction, `1 − fr`, measured from the older tap.
 * @return 1 when the read is an exact copy of the tap at `*id`, 0 when the kernel runs.
 * @note RT-safe and thread-safe: arithmetic over the state.
 */
static inline int omx_fdelay_split(const struct omx_fdelay *l, float delay, uint32_t *id,
                                   float *kf) {
  const float d = omx_fdelay_latency(l, delay);
  *id = (uint32_t)d;
  const float fr = d - (float)*id;
  *kf = 1.0f - fr;
  return (fr == 0.0f || *kf >= 1.0f);
}

#define OMX_CONTRACT_STAGE "fdelay/read"
/**
 * @brief The one read body: the tap at `id` samples behind the newest written sample, exact when
 *        `c` is NULL, else the kernel over `order + 1` taps with zero taps skipped and the result
 *        flushed.
 * @param l The line.
 * @param id The whole-sample distance behind the newest written sample.
 * @param c The kernel, or NULL for the exact copy.
 * @return The sample.
 * @pre `the-oldest-tap-is-inside-the-ring`.
 * @post `the-exact-tap-is-inside-the-ring` on the exact branch; `a-read-is-finite` on the kernel.
 * @note RT-safe: `order + 1` multiply-adds. Thread-safe: a read of the state.
 */
static inline float omx_fdelay_read_at(const struct omx_fdelay *l, uint32_t id, const float *c) {
  const uint32_t cap = l->cap;
  const uint32_t last = (l->wpos == 0u) ? cap - 1u : l->wpos - 1u;
  if (c == 0) {
    const uint32_t idx = (last >= id) ? last - id : last + cap - id;
    OMX_POST(idx < cap, "the-exact-tap-is-inside-the-ring");
    return l->ring[idx];
  }
  const int order = l->order;
  const uint32_t oldest = id + omx_fdelay_lookbehind(order) + 1u;
  OMX_PRE(oldest < cap, "the-oldest-tap-is-inside-the-ring");
  uint32_t t = (last >= oldest) ? last - oldest : last + cap - oldest;
  float acc = 0.0f;
  for (int k = 0; k <= order; k++) {
    if (c[k] != 0.0f) acc += c[k] * l->ring[t];
    t = (t + 1u == cap) ? 0u : t + 1u;
  }
  acc = omx_flush(acc);
  OMX_POST(acc - acc == 0.0f, "a-read-is-finite");
  return acc;
}

/**
 * @brief Read at `delay` samples behind the most recently written sample; `delay` may be fractional.
 * @param l The line; an unarmed line reads 0.
 * @param delay The delay, samples, finite, inside the ring, and at or above the kernel's reach
 *              unless whole.
 * @return The sample.
 * @pre `line-is-armed`, `a-delay-is-a-finite-number-of-samples`, `delay-is-inside-the-ring`,
 *      `a-fractional-delay-clears-the-kernels-reach`.
 * @note RT-safe: one kernel and one read. Thread-safe: a read of the state.
 */
static inline float omx_fdelay_read(const struct omx_fdelay *l, float delay) {
  OMX_PRE(l->order != 0 && l->ring != 0, "line-is-armed");
  OMX_PRE(delay - delay == 0.0f, "a-delay-is-a-finite-number-of-samples");
  OMX_PRE(l->order == 0 || delay <= omx_fdelay_max_delay(l->cap, l->order),
          "delay-is-inside-the-ring");
  OMX_PRE(l->order == 0 || delay >= omx_fdelay_min_delay(l->order) || delay == floorf(delay),
          "a-fractional-delay-clears-the-kernels-reach");
  if (l->order == 0 || l->ring == 0 || l->cap == 0u) return 0.0f;
  uint32_t id = 0u;
  float kf = 0.0f;
  if (omx_fdelay_split(l, delay, &id, &kf)) return omx_fdelay_read_at(l, id, 0);
  float c[OMX_FDELAY_MAX_TAPS];
  omx_fdelay_lagrange(l->order, kf, c);
  return omx_fdelay_read_at(l, id, c);
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "fdelay/tick"
/**
 * @brief Write one sample and read at `delay` in the same step: the per-sample door a modulated
 *        consumer drives. An unarmed line passes `x` through.
 * @param l The line.
 * @param x The sample to write.
 * @param delay The delay to read at, samples.
 * @return The sample read.
 * @pre `line-is-armed`.
 * @note RT-safe: one write and one read. Thread-safe on distinct state.
 */
static inline float omx_fdelay_tick(struct omx_fdelay *l, float x, float delay) {
  OMX_PRE(l->order != 0 && l->ring != 0, "line-is-armed");
  if (l->order == 0 || l->ring == 0 || l->cap == 0u) return x;
  omx_fdelay_write(l, x);
  return omx_fdelay_read(l, delay);
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "fdelay"
/**
 * @brief Process a block in place at a constant delay: the kernel computed once for the block.
 * @param buf The block, delayed in place.
 * @param n Samples in the block.
 * @param l The line; an unarmed line leaves the block untouched.
 * @param delay The delay, samples.
 * @pre `finite-in`, `line-is-armed`.
 * @post `finite-out`.
 * @invariant `write-cursor-inside-the-ring`.
 * @note RT-safe: one kernel per block, one write and one read per sample. Thread-safe on
 *       distinct state.
 */
static inline void omx_fdelay_process(float *buf, uint32_t n, struct omx_fdelay *l, float delay) {
  OMX_PRE(omx_block_finite(buf, n), "finite-in");
  OMX_PRE(l->order != 0 && l->ring != 0, "line-is-armed");
  if (buf == 0 || n == 0u || l->order == 0 || l->ring == 0 || l->cap == 0u) return;
  uint32_t id = 0u;
  float kf = 0.0f;
  float c[OMX_FDELAY_MAX_TAPS];
  const int whole = omx_fdelay_split(l, delay, &id, &kf);
  if (!whole) omx_fdelay_lagrange(l->order, kf, c);
  for (uint32_t i = 0; i < n; i++) {
    omx_fdelay_write(l, buf[i]);
    buf[i] = omx_fdelay_read_at(l, id, whole ? 0 : c);
  }
  OMX_POST(omx_block_finite(buf, n), "finite-out");
  OMX_INVARIANT(l->wpos < l->cap, "write-cursor-inside-the-ring");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_FDELAY_H */
