// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_denormal.h
 * @brief The denormal law: a value-by-value flush and the per-thread flush-to-zero mode.
 *
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 row 16 and Appendix A.
 */
#ifndef OMX_DENORMAL_H
#define OMX_DENORMAL_H

#include <math.h>
#include <stdint.h>

#include "omx_contract.h"

/** @brief The magnitude below which omx_flush() answers exactly zero. */
#define OMX_FLUSH_THRESHOLD 1e-20f

/**
 * @brief Flush a value below OMX_FLUSH_THRESHOLD (1e-20) in magnitude to exactly zero.
 * @param x Any sample or state word.
 * @return `x`, or 0 when |x| < OMX_FLUSH_THRESHOLD.
 * @note RT-safe and thread-safe: one compare, no call.
 */
static inline float omx_flush(float x) {
  return (fabsf(x) < OMX_FLUSH_THRESHOLD) ? 0.0f : x;
}

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "dsp/denormals-off"
/**
 * @brief Set flush-to-zero and denormals-are-zero for the calling thread (MXCSR bits 15 and 6).
 * @post The two bits read back set.
 * @note RT-safe: one control-register read and write. Thread-safe: the mode is per thread; every
 *       thread that runs a process path calls this on entry.
 */
static inline void omx_denormals_off(void) {
  _mm_setcsr(_mm_getcsr() | 0x8040u);
  OMX_POST((_mm_getcsr() & 0x8040u) == 0x8040u, "no-denormal-state");
}
#undef OMX_CONTRACT_STAGE
#elif defined(__aarch64__)
/**
 * @brief Set flush-to-zero for the calling thread (FPCR bit 24).
 * @note RT-safe: one system-register read and write. Thread-safe: the mode is per thread.
 */
static inline void omx_denormals_off(void) {
  uint64_t fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ULL << 24)));
}
#else
/**
 * @brief No flush-to-zero mode on this architecture; correctness is unaffected.
 * @note RT-safe and thread-safe: does nothing.
 */
static inline void omx_denormals_off(void) {}
#endif

#endif /* OMX_DENORMAL_H */
