// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_contract.h
 * @brief PRE / POST / INVARIANT contracts for every DSP primitive and kernel.
 *
 * Compiled only under `OMX_CONTRACTS` (test and bench builds); the release build compiles every
 * contract to nothing. A violation never aborts, allocates or prints: it is counted and, for the
 * first `OMX_CONTRACT_MAX` violations, recorded in one process-wide ledger a test reads afterwards.
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §7 (the law) and §4 F1 (the ledger).
 */
#ifndef OMX_CONTRACT_H
#define OMX_CONTRACT_H

#include <stdint.h>
#include "omx_contract_limits.h"

#ifdef OMX_CONTRACTS

#include <stdatomic.h>

/** @brief Violations kept in full; the count stays exact past it. */
#define OMX_CONTRACT_MAX 64u

/**
 * @brief One recorded violation.
 * @note A reader takes the record only once `ready` is set (see omx_contract_record_ready()).
 */
struct omx_contract_record {
  const char *stage;      /**< The `OMX_CONTRACT_STAGE` of the function that evaluated it. */
  const char *token;      /**< The law's token, a short stable string. */
  const char *kind;       /**< `"pre"`, `"post"` or `"invariant"`. */
  uint32_t frame;         /**< The frame index for a per-sample law, else 0. */
  _Atomic uint32_t ready; /**< 1 once the fields above are stored, released by the writer. */
};

/**
 * @brief The process-wide ledger: fixed size, no allocation, no I/O, safe from any number of
 *        threads evaluating contracts at once.
 * @invariant `count` counts every violation seen; `rec[i]` holds the i-th for `i < OMX_CONTRACT_MAX`.
 */
struct omx_contract_ledger {
  struct omx_contract_record rec[OMX_CONTRACT_MAX]; /**< The first OMX_CONTRACT_MAX violations. */
  _Atomic uint32_t count;  /**< Violations seen, exact even past OMX_CONTRACT_MAX. */
  _Atomic uint32_t checks; /**< Contracts evaluated: the floor that proves a battery ran. */
};

/** @brief The one ledger, defined by the translation unit that sets `OMX_CONTRACT_STORAGE`. */
extern struct omx_contract_ledger omx_contract_log;

#ifdef OMX_CONTRACT_STORAGE
struct omx_contract_ledger omx_contract_log;
#endif

/**
 * @brief Record one evaluated contract and, if it failed, its violation.
 * @param ok Non-zero when the law held.
 * @param stage The evaluating function's stage name (a string literal that outlives the ledger).
 * @param token The law's token (a string literal).
 * @param kind `"pre"`, `"post"` or `"invariant"`.
 * @param frame Frame index for a per-sample law, 0 otherwise.
 * @post `checks` grew by one; on a failure `count` grew by one and, when a slot was free, the
 *       record is stored and published with a release store on its `ready` word.
 * @note RT-safe: two relaxed fetch-adds, plain stores, one release store; no allocation, no lock.
 * @note Thread-safe: each violation claims its own slot through the fetch-add on `count`.
 */
static inline void omx_contract_note(int ok, const char *stage, const char *token,
                                     const char *kind, uint32_t frame) {
  atomic_fetch_add_explicit(&omx_contract_log.checks, 1u, memory_order_relaxed);
  if (ok) return;
  const uint32_t i = atomic_fetch_add_explicit(&omx_contract_log.count, 1u, memory_order_relaxed);
  if (i >= OMX_CONTRACT_MAX) return;
  struct omx_contract_record *r = &omx_contract_log.rec[i];
  r->stage = stage;
  r->token = token;
  r->kind = kind;
  r->frame = frame;
  atomic_store_explicit(&r->ready, 1u, memory_order_release);
}

/**
 * @brief Whether a ledger record's fields are readable.
 * @param r A record of `omx_contract_log.rec`.
 * @return 1 once the writer published the record, 0 while it is still being stored.
 * @note Thread-safe: an acquire load pairing with the writer's release.
 */
static inline int omx_contract_record_ready(const struct omx_contract_record *r) {
  return atomic_load_explicit(&r->ready, memory_order_acquire) != 0u;
}

/**
 * @brief Forget every recorded violation; `checks` is kept.
 * @post `count` is 0 and no record reads ready.
 * @note Test-only, called while no other thread evaluates contracts.
 */
static inline void omx_contract_reset(void) {
  for (uint32_t i = 0; i < OMX_CONTRACT_MAX; i++)
    atomic_store_explicit(&omx_contract_log.rec[i].ready, 0u, memory_order_relaxed);
  atomic_store_explicit(&omx_contract_log.count, 0u, memory_order_relaxed);
}

/*
 * OMX_CONTRACT_STAGE has no default: a header using these macros without defining it fails to
 * compile, so no violation is ever recorded without a stage to attribute it to.
 */

/** @brief A precondition: what the function requires of its caller. */
#define OMX_PRE(cond, token) omx_contract_note(!!(cond), OMX_CONTRACT_STAGE, (token), "pre", 0u)
/** @brief A postcondition: what the function guarantees about what it produced. */
#define OMX_POST(cond, token) omx_contract_note(!!(cond), OMX_CONTRACT_STAGE, (token), "post", 0u)
/** @brief An invariant of the function's state, true between blocks and inside one. */
#define OMX_INVARIANT(cond, token) \
  omx_contract_note(!!(cond), OMX_CONTRACT_STAGE, (token), "invariant", 0u)
/** @brief OMX_PRE naming the frame the law broke at. */
#define OMX_PRE_AT(cond, token, frame) \
  omx_contract_note(!!(cond), OMX_CONTRACT_STAGE, (token), "pre", (uint32_t)(frame))
/** @brief OMX_POST naming the frame the law broke at. */
#define OMX_POST_AT(cond, token, frame) \
  omx_contract_note(!!(cond), OMX_CONTRACT_STAGE, (token), "post", (uint32_t)(frame))

/** @brief Whether `sr` is one of the rates the console declares (omx_rate_is_declared()). */
#define OMX_RATE_IS_DECLARED(sr) omx_rate_is_declared((float)(sr))

#else /* !OMX_CONTRACTS: the release build */

#define OMX_PRE(cond, token) ((void)0)
#define OMX_POST(cond, token) ((void)0)
#define OMX_INVARIANT(cond, token) ((void)0)
#define OMX_PRE_AT(cond, token, frame) ((void)0)
#define OMX_POST_AT(cond, token, frame) ((void)0)
#define OMX_RATE_IS_DECLARED(sr) (1)

#endif /* OMX_CONTRACTS */

/**
 * @brief Whether a sample rate is one the console declares.
 * @param sr Sample rate, Hz.
 * @return 1 when `sr` is a member of the generated `OMX_DECLARED_RATES`, else 0.
 * @note RT-safe and thread-safe: a read of a constant table.
 */
static inline int omx_rate_is_declared(float sr) {
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++)
    if (OMX_DECLARED_RATES[i] == sr) return 1;
  return 0;
}

/**
 * @brief Whether every sample of a block is finite.
 * @param b The block, or NULL (a block a function was not handed promises nothing).
 * @param n Samples in the block.
 * @return 1 when `b` is NULL or every sample is finite, else 0.
 * @note RT-safe and thread-safe: a read-only pass, no call.
 */
static inline int omx_block_finite(const float *b, uint32_t n) {
  if (b == 0) return 1;
  for (uint32_t i = 0; i < n; i++) {
    float x = b[i];
    if (!(x - x == 0.0f)) return 0; /* false for NaN and for both infinities */
  }
  return 1;
}

/**
 * @brief Whether every word of a double block is finite: `Σ b[i]·0` is 0, or NaN when a word is
 *        NaN or infinite.
 * @param b The block.
 * @param n Words in the block.
 * @return 1 when every word is finite, else 0.
 * @note RT-safe and thread-safe: a read-only pass, no call.
 */
static inline int omx_block_finite_d(const double *b, uint32_t n) {
  double z = 0.0;
  for (uint32_t i = 0; i < n; i++) z += b[i] * 0.0;
  return z == 0.0;
}

/**
 * @brief Whether every sample of both legs of a stereo block is finite.
 * @param l The left leg, or NULL.
 * @param r The right leg, or NULL.
 * @param n Frames in the block.
 * @return 1 when both legs are finite (omx_block_finite), else 0.
 * @note RT-safe and thread-safe: two read-only passes, no call.
 */
static inline int omx_block_pair_finite(const float *l, const float *r, uint32_t n) {
  return omx_block_finite(l, n) && omx_block_finite(r, n);
}

/**
 * @brief Whether both legs of a lane are finite.
 * @param l The L leg.
 * @param r The R leg, or NULL on a mono lane.
 * @param n Samples per leg.
 * @return 1 when both legs pass omx_block_finite(), else 0.
 * @note RT-safe and thread-safe: a read-only pass.
 */
static inline int omx_lane_finite(const float *l, const float *r, uint32_t n) {
  return omx_block_finite(l, n) && omx_block_finite(r, n);
}

/**
 * @brief The largest absolute sample of a block.
 * @param b The block, or NULL.
 * @param n Samples in the block.
 * @return max |b[i]|, or 0 for a NULL block.
 * @note RT-safe and thread-safe: a read-only pass.
 */
static inline float omx_block_absmax(const float *b, uint32_t n) {
  float m = 0.0f;
  if (b == 0) return 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    float a = b[i] < 0.0f ? -b[i] : b[i];
    if (a > m) m = a;
  }
  return m;
}

#endif /* OMX_CONTRACT_H */
