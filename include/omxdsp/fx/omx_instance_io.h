// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_instance_io.h
 * @brief What every plugin instance does with a host's block before and after its kernel: the copy of
 *        in to out where they differ, and the check that a block came out as it went in.
 *
 * A host may hand a face the same buffer for in and out; a null pointer or an empty block is the
 * host's way of saying there is nothing to run.
 */
#ifndef OMX_INSTANCE_IO_H
#define OMX_INSTANCE_IO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief Copy both legs of in to out where they differ.
 * @param in_l The left input, `n` samples.
 * @param in_r The right input, `n` samples.
 * @param out_l The left output, `n` samples, may be `in_l`.
 * @param out_r The right output, `n` samples, may be `in_r`.
 * @param n The block length.
 * @note RT-safe: two memmoves at most. Thread-safe on distinct buffers.
 */
static inline void omx_instance_copy(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n) {
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
}

/**
 * @brief The face's opening: refuse a block with nothing to run, else copy in to out.
 * @param in_l The left input.
 * @param in_r The right input.
 * @param out_l The left output, may be `in_l`.
 * @param out_r The right output, may be `in_r`.
 * @param n The block length.
 * @return 0 when a pointer is null or `n` is 0 (nothing was touched), else 1 with out holding in.
 * @note RT-safe: omx_instance_copy(). Thread-safe on distinct buffers.
 */
static inline int omx_instance_copy_through(const float *in_l, const float *in_r, float *out_l, float *out_r,
                                            uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return 0;
  omx_instance_copy(in_l, in_r, out_l, out_r, n);
  return 1;
}

/**
 * @brief Whether both legs of a block are bit for bit another block.
 * @param a_l The left block, `n` samples.
 * @param a_r The right block, `n` samples.
 * @param b_l The other left block, `n` samples.
 * @param b_r The other right block, `n` samples.
 * @param n The block length.
 * @return Non-zero when both legs compare equal.
 * @note Read by a face's bypass-identity postcondition, compiled out of a release build.
 */
static inline int omx_instance_same_block(const float *a_l, const float *a_r, const float *b_l, const float *b_r,
                                          uint32_t n) {
  return memcmp(a_l, b_l, (size_t)n * sizeof(float)) == 0 && memcmp(a_r, b_r, (size_t)n * sizeof(float)) == 0;
}

#endif /* OMX_INSTANCE_IO_H */
