// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_fastmath.h
 * @brief The vector math library calls the per-sample kernels make: tanhf, expf, log10f, powf.
 *
 * glibc ships SIMD versions of these functions in libmvec (`_ZGVbN4v_tanhf` and the like on
 * x86-64, `_ZGVnN4v_tanhf` on aarch64) and the compiler calls them from a vectorised loop when the
 * function is declared `#pragma omp declare simd`. glibc's own <math.h> makes those declarations only
 * under `__FAST_MATH__`, which this library never uses (`-ffast-math` sets flush-to-zero in the
 * process at load); this header makes the same declarations, for the functions the build found in
 * the libmvec it links, and nothing else.
 *
 * ## How a build turns it on
 *
 * The compile flags carry, per function the build-time probe (tools/fastmath-probe.sh) found
 * declared AND exported by the installed libmvec, `-DOMX_FASTMATH_MVEC_TANHF=1` (`_EXPF`, `_LOG10F`,
 * `_POWF`), plus `-fopenmp-simd -fno-math-errno`, and the link carries `-lmvec`. omxdsp.pc says so to a
 * consumer. Without those macros this header declares nothing, OMX_FM_SIMD is empty and the kernels
 * call the libm exactly as before: a system without libmvec (Debian 12 on arm64, glibc older than
 * 2.40 for tanhf and powf on aarch64, older than 2.35 on x86-64) builds and runs the plain path, and
 * so does a consumer that ignores the flags. A partial set is the plain path for every loop that needs a
 * function it lacks: the dynamics gain loops vectorise only with log10f and powf (OMX_DYN_LANES), the drive's
 * tanh loop only with tanhf (OMX_DRIVE_LANES); `make fastmath-partial` holds it for log10f and expf alone. `-fopenmp-simd` only reads the `omp simd` pragmas; it
 * links no OpenMP runtime. `-fno-math-errno` lets the compiler treat the four functions as pure (it enables no
 * exception; the library never reads errno or the FP flags), so a loop over them vectorises. The trapping-math
 * default stays: a loop that must vectorise selects with omx_fm_select(), never `?:` on floats, so its arithmetic
 * is evaluated unconditionally and the compiler has nothing to speculate (tools/fastmath-vec-check.sh holds it).
 *
 * ## What it is not
 *
 * Not `-ffast-math`, and not a second implementation: the vector functions are libmvec's, the scalar
 * ones the libm's. A kernel's output through the vector path is NOT bit-identical to the scalar one.
 * The difference is bounded by the table below, held by test/fx/fastmath.test.c, and the audio
 * difference of every kernel that uses it by `make fastmath-diff`.
 *
 * ## Error (float ULP of the exactly rounded result; the bound), valid range, specials
 *
 * Measured on x86-64, glibc 2.43: tanhf 1.03 (the libm's own tanhf on glibc 2.36: 2.10), expf 2.87, log10f 1.91,
 * powf(10, x) 0.62 (the oracle prints
 * them); the bounds leave room for the aarch64 implementations.
 *
 *   function  max ULP  range the library uses it over
 *   tanhf     3        any finite float (the shaper's input, drive times a sample, up to ~1e4)
 *   expf      3        [-87, 88.5] (normal results); below ~-104 the result is 0, above 88.72 it is +inf
 *   log10f    3        [OMX_LIN_TO_DB_FLOOR_LIN, 1e6], the argument omx_lin_to_db floors
 *   powf      3        the base 10 with an exponent in [-12, 4], the dB-to-linear law
 *
 * NaN in gives NaN out, tanhf(+-inf) is +-1, tanhf(+-0) is a zero of either sign (libmvec on aarch64 does not keep
 * the sign of a zero: it is the same sample, and nothing divides by the shaper's output), expf(-inf) is 0, a denormal result is finite (zero or
 * denormal), and a finite argument in the range gives a finite result: the finite law the kernels'
 * contracts state holds through the vector path as through the libm.
 */
#ifndef OMXDSP_FASTMATH_H
#define OMXDSP_FASTMATH_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdint.h>
#include <string.h>

#if defined(OMX_FASTMATH_MVEC_TANHF) || defined(OMX_FASTMATH_MVEC_EXPF) || defined(OMX_FASTMATH_MVEC_LOG10F) || \
    defined(OMX_FASTMATH_MVEC_POWF)
/** @brief 1 when the build declared at least one libmvec function to the compiler. */
#define OMX_FASTMATH_MVEC 1
#endif

#ifdef OMX_FASTMATH_MVEC
/** @brief Asks the compiler to vectorise the loop that follows; empty without libmvec. */
#define OMX_FM_SIMD _Pragma("omp simd")
/** @brief On a function a vectorised loop calls: it must be inlined into the loop (GCC 12 leaves them as calls); empty without libmvec. */
#define OMX_FM_ALWAYS __attribute__((always_inline))
#else
/** @brief Without libmvec in the build: empty, the loop is the scalar one. */
#define OMX_FM_SIMD
/** @brief Without libmvec in the build: empty. */
#define OMX_FM_ALWAYS
#endif

#ifdef OMX_FASTMATH_MVEC_TANHF
#pragma omp declare simd notinbranch
extern float tanhf(float);
#endif
#ifdef OMX_FASTMATH_MVEC_EXPF
#pragma omp declare simd notinbranch
extern float expf(float);
#endif
#ifdef OMX_FASTMATH_MVEC_LOG10F
#pragma omp declare simd notinbranch
extern float log10f(float);
#endif
#ifdef OMX_FASTMATH_MVEC_POWF
#pragma omp declare simd notinbranch
extern float powf(float, float);
#endif

/**
 * @brief `cond ? a : b` for floats as a bit select, so a loop whose condition does not vary still vectorises.
 *
 * Some compilers (GCC 12) cannot vectorise a select of floats on a scalar condition that is the same for every
 * sample, such as the gain computer's mode; the same select on integer bits they can. The result is bit for bit
 * the operand `cond ? a : b` picks.
 * @param cond Non-zero to take `a`.
 * @param a The float taken when `cond` is non-zero.
 * @param b The float taken otherwise.
 * @return `a` or `b`, bit for bit.
 * @note Three integer operations. Thread-safe: pure.
 * @thread audio
 */
static inline OMX_FM_ALWAYS float omx_fm_select(int cond, float a, float b) {
  const uint32_t m = (uint32_t)0 - (uint32_t)(cond != 0);
  uint32_t x, y;
  memcpy(&x, &a, sizeof x);
  memcpy(&y, &b, sizeof y);
  x = (x & m) | (y & ~m);
  memcpy(&a, &x, sizeof a);
  return a;
}

/** @brief The samples a tile of OMX_FM_MAP() holds: a multiple of every libmvec vector width (4, 8, 16 floats). */
#define OMX_FM_TILE 16u

/** @brief OMX_FM_MAP() as the plain loop, for a loop that takes no lane (a function it calls is not in libmvec). */
#define OMX_FM_MAP_PLAIN(out, in, n, v, expr)                                                                     \
  do {                                                                                                            \
    const float *const omx_fm_in_ = (in);                                                                         \
    float *const omx_fm_out_ = (out);                                                                             \
    const uint32_t omx_fm_n_ = (n);                                                                               \
    for (uint32_t omx_fm_j_ = 0; omx_fm_j_ < omx_fm_n_; omx_fm_j_++) {                                            \
      const float v = omx_fm_in_[omx_fm_j_];                                                                      \
      omx_fm_out_[omx_fm_j_] = (expr);                                                                            \
    }                                                                                                             \
  } while (0)

#ifdef OMX_FASTMATH_MVEC
/**
 * @brief `out[i] = expr`, `v` bound to `in[i]`, over `n` samples, every sample through the SAME call whatever `n` is.
 *
 * A vectorised loop over a run-time `n` has a remainder (and may have an alignment peel) that the compiler runs as
 * the scalar loop, on the libm: a sample there gets the libm's bits and the same sample inside the vector body gets
 * libmvec's, so a kernel's output would depend on where the caller's block ends. This walks the block in tiles of
 * OMX_FM_TILE samples copied into an aligned local array, the last tile padded with its own last sample, and runs
 * the expression over the whole tile as a loop of constant trip count, which the compiler vectorises with no
 * remainder and no peel. The block size is then not observable (test/fx/fastmath_blocks.test.c). Without libmvec
 * it is the plain loop. `out` may be `in`. The expression must be pure in `v`: no state, one sample in, one out.
 * @note No allocation, one tile on the stack. Thread-safe on distinct blocks.
 */
#define OMX_FM_MAP(out, in, n, v, expr)                                                                           \
  do {                                                                                                            \
    const float *const omx_fm_in_ = (in);                                                                         \
    float *const omx_fm_out_ = (out);                                                                             \
    const uint32_t omx_fm_n_ = (n);                                                                               \
    for (uint32_t omx_fm_o_ = 0; omx_fm_o_ < omx_fm_n_; omx_fm_o_ += OMX_FM_TILE) {                               \
      const uint32_t omx_fm_k_ = omx_fm_n_ - omx_fm_o_ < OMX_FM_TILE ? omx_fm_n_ - omx_fm_o_ : OMX_FM_TILE;       \
      float omx_fm_t_[OMX_FM_TILE] __attribute__((aligned(64)));                                                  \
      memcpy(omx_fm_t_, omx_fm_in_ + omx_fm_o_, omx_fm_k_ * sizeof(float));                                       \
      for (uint32_t omx_fm_j_ = omx_fm_k_; omx_fm_j_ < OMX_FM_TILE; omx_fm_j_++)                                  \
        omx_fm_t_[omx_fm_j_] = omx_fm_t_[omx_fm_k_ - 1u];                                                         \
      OMX_FM_SIMD for (uint32_t omx_fm_j_ = 0; omx_fm_j_ < OMX_FM_TILE; omx_fm_j_++) {                            \
        const float v = omx_fm_t_[omx_fm_j_];                                                                     \
        omx_fm_t_[omx_fm_j_] = (expr);                                                                            \
      }                                                                                                           \
      memcpy(omx_fm_out_ + omx_fm_o_, omx_fm_t_, omx_fm_k_ * sizeof(float));                                      \
    }                                                                                                             \
  } while (0)
#else
/** @brief Without libmvec in the build: the plain loop. */
#define OMX_FM_MAP OMX_FM_MAP_PLAIN
#endif

/** @brief The largest error of a libmvec tanhf against the libm's, in float ULP. */
#define OMX_FM_TANHF_MAX_ULP 3.0
/** @brief The largest error of a libmvec expf against the libm's, in float ULP. */
#define OMX_FM_EXPF_MAX_ULP 3.0
/** @brief The largest error of a libmvec log10f against the libm's, in float ULP. */
#define OMX_FM_LOG10F_MAX_ULP 3.0
/** @brief The largest error of a libmvec powf against the libm's, in float ULP. */
#define OMX_FM_POWF_MAX_ULP 3.0

/**
 * @brief Which of the four functions this translation unit calls through libmvec.
 * @return A bit set: 1 tanhf, 2 expf, 4 log10f, 8 powf; 0 when every call is the libm's.
 * @note A constant. Thread-safe: pure.
 * @thread audio
 */
static inline int omx_fastmath_enabled(void) {
  return 0
#ifdef OMX_FASTMATH_MVEC_TANHF
         | 1
#endif
#ifdef OMX_FASTMATH_MVEC_EXPF
         | 2
#endif
#ifdef OMX_FASTMATH_MVEC_LOG10F
         | 4
#endif
#ifdef OMX_FASTMATH_MVEC_POWF
         | 8
#endif
      ;
}

#endif /* OMXDSP_FASTMATH_H */
