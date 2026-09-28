// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_lookahead.h
 * @brief The look-ahead ring's cursor law: an exact integer delay, `out[n] = in[n − D]`.
 *
 * The ring is caller-owned memory of `cap` samples per leg and ONE write cursor the caller keeps;
 * a kernel writes the current sample at the cursor, reads any tap `D < cap` at
 * `omx_lookahead_back(pos, D, cap)` (write-then-read, so `D = 0` is the sample just written) and
 * moves the cursor with `omx_lookahead_fwd`. One cursor serves every leg's buffer and every tap.
 * Never a fractional read: a look-ahead never reads between samples. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 10.
 */
#ifndef OMX_LOOKAHEAD_H
#define OMX_LOOKAHEAD_H

#include <stdint.h>

#include "omx_contract.h"

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

#endif /* OMX_LOOKAHEAD_H */
