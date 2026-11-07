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

#include <omxdsp/omx_contract.h>

/**
 * @brief Copy both legs of in to out where they differ.
 * @param in_l The left input, `n` samples.
 * @param in_r The right input, `n` samples.
 * @param out_l The left output, `n` samples, may be `in_l`.
 * @param out_r The right output, `n` samples, may be `in_r`.
 * @param n The block length.
 * @note Two memmoves at most. Thread-safe on distinct buffers.
 * @thread audio
 */
static inline void omx_instance_copy(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n) {
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
}

/**
 * @brief Whether a host block has something to run: every pointer set and `n` above 0.
 * @param in_l The left input.
 * @param in_r The right input.
 * @param out_l The left output.
 * @param out_r The right output.
 * @param n The block length.
 * @return 1 when the block can be run, else 0 (nothing may be touched).
 * @note Thread-safe: reads its arguments only.
 * @thread audio
 */
static inline int omx_instance_block_ok(const float *in_l, const float *in_r, const float *out_l, const float *out_r,
                                        uint32_t n) {
  return in_l && in_r && out_l && out_r && n != 0u;
}

/**
 * @brief The face's opening: refuse a block with nothing to run, else copy in to out.
 * @param in_l The left input.
 * @param in_r The right input.
 * @param out_l The left output, may be `in_l`.
 * @param out_r The right output, may be `in_r`.
 * @param n The block length.
 * @return 0 when a pointer is null or `n` is 0 (nothing was touched), else 1 with out holding in.
 * @note Omx_instance_copy(). Thread-safe on distinct buffers.
 * @thread audio
 */
static inline int omx_instance_copy_through(const float *in_l, const float *in_r, float *out_l, float *out_r,
                                            uint32_t n) {
  if (!omx_instance_block_ok(in_l, in_r, out_l, out_r, n)) return 0;
  omx_instance_copy(in_l, in_r, out_l, out_r, n);
  return 1;
}

/**
 * @brief The opening of a face whose kernel runs only while engaged: copy in to out, then whether to run.
 * @param in_l The left input.
 * @param in_r The right input.
 * @param out_l The left output, may be `in_l`.
 * @param out_r The right output, may be `in_r`.
 * @param n The block length.
 * @param engaged Whether the instance is ready and its kernel switched on.
 * @return 1 when out holds in and the kernel is to run on it, else 0 (out holds in, or nothing was touched).
 * @note Omx_instance_copy_through(). Thread-safe on distinct buffers.
 * @thread audio
 */
static inline int omx_instance_through_engaged(const float *in_l, const float *in_r, float *out_l, float *out_r,
                                               uint32_t n, int engaged) {
  return omx_instance_copy_through(in_l, in_r, out_l, out_r, n) && engaged;
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
 * @thread audio
 */
static inline int omx_instance_same_block(const float *a_l, const float *a_r, const float *b_l, const float *b_r,
                                          uint32_t n) {
  return memcmp(a_l, b_l, (size_t)n * sizeof(float)) == 0 && memcmp(a_r, b_r, (size_t)n * sizeof(float)) == 0;
}

/**
 * @brief The face's bind: zero its state and take the rate if the oracle set holds it.
 * @param s The instance.
 * @param size The instance's size.
 * @param sr The rate.
 * @return 1 when usable, 0 when `s` is null or the rate is outside the set.
 * @note Allocates nothing.
 * @thread control
 */
static inline int omx_instance_bind(void *s, size_t size, float sr) {
  if (!s) return 0;
  memset(s, 0, size);
  return omx_rate_is_oracle_full(sr);
}

/**
 * @brief The face's bind for a kernel that takes any positive rate: zero its state and take the rate.
 * @param s The instance.
 * @param size The instance's size.
 * @param sr The rate.
 * @return 1 when usable, 0 when `s` is null or the rate is not positive (NaN included).
 * @note Allocates nothing.
 * @thread control
 */
static inline int omx_instance_bind_positive(void *s, size_t size, float sr) {
  if (!s) return 0;
  memset(s, 0, size);
  return sr > 0.0f;
}

/**
 * @brief Silence a face's two rings of `cap` samples each, before its kernel state is armed again over them.
 * @param ring_l The left ring.
 * @param ring_r The right ring.
 * @param cap Samples in each ring.
 * @note Two bounded memsets, no allocation.
 * @thread audio
 */
static inline void omx_instance_rings_clear(float *ring_l, float *ring_r, uint32_t cap) {
  memset(ring_l, 0, (size_t)cap * sizeof(float));
  memset(ring_r, 0, (size_t)cap * sizeof(float));
}

/**
 * @brief The face's run over a kernel that processes in place: copy in to out, stop there when the instance
 *        is not ready, else run `kernel_call` on out; a bypassed atom leaves out equal to in.
 *
 * Expands inside the face's run function, so it returns from it, and its postcondition is recorded under the
 * face's own contract stage. `s` is the instance, with `ready` and `atom.enabled`; `kernel_call` is the
 * kernel's call on `out_l` and `out_r`.
 */
#define OMX_INSTANCE_RUN_IN_PLACE(s, in_l, in_r, out_l, out_r, n, kernel_call)                                 \
  do {                                                                                                         \
    if (!omx_instance_copy_through((in_l), (in_r), (out_l), (out_r), (n))) return;                             \
    if (!(s) || !(s)->ready) return;                                                                           \
    kernel_call;                                                                                               \
    OMX_POST((s)->atom.enabled || omx_instance_same_block((out_l), (out_r), (in_l), (in_r), (n)),             \
             "bypass-identity");                                                                               \
  } while (0)

#endif /* OMX_INSTANCE_IO_H */
