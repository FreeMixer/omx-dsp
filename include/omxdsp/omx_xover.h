// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_xover.h
 * @brief The Linkwitz-Riley crossover, LR2 and LR4: two bands in phase whose magnitudes sum to one
 *        and whose sum is an all-pass.
 *
 * One corner, one `K = tan(π·fc/sr)`. LR4: the cookbook Butterworth low- and high-pass at
 * OMX_XOVER_LR4_SECTION_Q, each run twice, on the denominator omx_allpass2_design() returns, so
 * `lo + hi = AP2(fc)`. LR2: the same pair at OMX_XOVER_LR2_SECTION_Q run once, the high band
 * inverted, so `lo + hi = AP1(fc)`, omx_allpass1()'s transfer as the section `{a, 1, 0, a, 0}`.
 * The sections run in omx_biquad_tdf2_d(); the bands are narrowed to float at the end. An N-band tree
 * is N−1 crossovers with omx_xover_allpass() of every later corner on every earlier band. Design:
 * docs/design/specs/2026-09-26-dsp-primitives.md §1 row 6 and Appendix A.
 */
#ifndef OMX_XOVER_H
#define OMX_XOVER_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "omx_allpass.h"
#include "omx_biquad.h"
#include "omx_contract.h"
#include "omx_contract_limits.h"

/** @brief What omx_xover_design() answers. */
enum omx_xover_status {
  OMX_XOVER_OK = 0,         /**< Designed. */
  OMX_XOVER_BAD_ORDER = 1,  /**< The order is neither 2 nor 4; the crossover is the wire. */
  OMX_XOVER_BAD_CORNER = 2, /**< The corner is outside (0, sr/2); the crossover is the wire. */
};

/**
 * @brief One crossover's coefficients, built by omx_xover_design() and read by every process
 *        call; the wire (everything in `lo`, nothing in `hi`) after a refused design.
 */
struct omx_xover {
  double lp[5];     /**< The low-pass section `{b0, b1, b2, a1, a2}`. */
  double hp[5];     /**< The high-pass section, on the same denominator. */
  double ap[5];     /**< The all-pass `lo + hi` equals: the second-order design (LR4) or `{a, 1, 0, a, 0}` (LR2). */
  double hi_sign;   /**< −1 for LR2 (the inverted high band), +1 for LR4. */
  uint32_t sections; /**< Sections per band: 1 (LR2) or 2 (LR4). */
};

/**
 * @brief One crossover's state: two sections per band and the all-pass the partition
 *        postcondition runs, advanced only in a contracts build.
 */
struct omx_xover_state {
  double lp[2][2];  /**< The low band's sections, `{s₁, s₂}` each. */
  double hp[2][2];  /**< The high band's sections. */
  double ref[2];    /**< The reference all-pass of `bands-partition-unity`. */
};

/** @brief The state of omx_xover_allpass(): one all-pass section. */
struct omx_xover_ap_state {
  double s[2]; /**< `{s₁, s₂}`. */
};

/**
 * @brief The crossover state's size, for a host that lays the state out itself.
 * @return `sizeof(struct omx_xover_state)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_xover_state_size(void) { return sizeof(struct omx_xover_state); }

/**
 * @brief The crossover state's alignment, for a host that lays the state out itself.
 * @return `_Alignof(struct omx_xover_state)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_xover_state_align(void) { return _Alignof(struct omx_xover_state); }

/**
 * @brief The tree all-pass state's size, for a host that lays the state out itself.
 * @return `sizeof(struct omx_xover_ap_state)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_xover_ap_state_size(void) { return sizeof(struct omx_xover_ap_state); }

/**
 * @brief The tree all-pass state's alignment, for a host that lays the state out itself.
 * @return `_Alignof(struct omx_xover_ap_state)`.
 * @note RT-safe and thread-safe: a constant.
 */
static inline size_t omx_xover_ap_state_align(void) { return _Alignof(struct omx_xover_ap_state); }

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "xover/design"
/**
 * @brief Design an LR2 or LR4 crossover at `fc`.
 * @param c The coefficients, written whole; the wire on a refusal.
 * @param order 2 or 4.
 * @param fc Corner, Hz; inside (0, sr/2).
 * @param sr Sample rate, Hz; a declared rate.
 * @return OMX_XOVER_OK, or the refusal's code.
 * @pre `order-is-two-or-four`, `corner-inside-the-band`, `rate-is-declared`.
 * @post `finite-coeffs`.
 * @note Control-thread word: one `tan`, divides, no allocation; RT-safe all the same.
 *       Thread-safe: writes only `c`.
 */
static inline enum omx_xover_status omx_xover_design(struct omx_xover *c, uint32_t order, double fc,
                                                     double sr) {
  memset(c, 0, sizeof *c);
  c->lp[0] = 1.0;
  c->ap[0] = 1.0;
  c->hi_sign = 1.0;
  c->sections = 1u;
  const int order_ok = order == 2u || order == 4u;
  const int inside = sr > 0.0 && fc > 0.0 && fc < 0.5 * sr;
  OMX_PRE(order_ok, "order-is-two-or-four");
  if (!order_ok) return OMX_XOVER_BAD_ORDER;
  OMX_PRE(inside, "corner-inside-the-band");
  if (!inside) return OMX_XOVER_BAD_CORNER;
  OMX_PRE(OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  const double q = order == 4u ? OMX_XOVER_LR4_SECTION_Q : OMX_XOVER_LR2_SECTION_Q;
  double den[5];
  omx_allpass2_design(fc, q, sr, den);
  const double K = omx_allpass_prewarp(fc, sr);
  const double norm = 1.0 / (1.0 + K / q + K * K);
  const double kk = K * K * norm;
  c->lp[0] = kk; c->lp[1] = 2.0 * kk; c->lp[2] = kk; c->lp[3] = den[3]; c->lp[4] = den[4];
  c->hp[0] = norm; c->hp[1] = -2.0 * norm; c->hp[2] = norm; c->hp[3] = den[3]; c->hp[4] = den[4];
  if (order == 4u) {
    memcpy(c->ap, den, sizeof den);
    c->hi_sign = 1.0;
    c->sections = 2u;
  } else {
    const double a = omx_allpass1_coef_d(fc, sr);
    c->ap[0] = a; c->ap[1] = 1.0; c->ap[2] = 0.0; c->ap[3] = a; c->ap[4] = 0.0;
    c->hi_sign = -1.0;
    c->sections = 1u;
  }
  OMX_POST(omx_block_finite_d(c->lp, 5u) && omx_block_finite_d(c->hp, 5u) && omx_block_finite_d(c->ap, 5u),
           "finite-coeffs");
  return OMX_XOVER_OK;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "xover/process"
/**
 * @brief Split a block into its low and high bands; `lo` may be `x` itself.
 * @param x The input block; finite.
 * @param lo The low band, `n` samples; may alias `x`.
 * @param hi The high band, `n` samples; never aliases `x` or `lo`.
 * @param n Samples in the block.
 * @param c The coefficients (omx_xover_design()).
 * @param s The state, updated in place.
 * @pre `finite-in`.
 * @post `finite-out`; `bands-partition-unity`: `lo + hi` equals the all-pass `c->ap` over the
 *       same input to OMX_XOVER_PARTITION_TOL of `max(1, block peak)`.
 * @invariant `state-finite`, on return.
 * @note RT-safe: `2·sections` double sections per sample, no call, no allocation; the double
 *       state's denormals are covered by the thread's FTZ mode. Thread-safe on distinct state.
 */
static inline void omx_xover_process(const float *x, float *lo, float *hi, uint32_t n,
                                     const struct omx_xover *c, struct omx_xover_state *s) {
  OMX_PRE(omx_block_finite(x, n), "finite-in");
#ifdef OMX_CONTRACTS
  double worst = 0.0, peak = 0.0;
#endif
  const float sign = (float)c->hi_sign;
  for (uint32_t i = 0; i < n; i++) {
    const double xi = x[i];
    double l = omx_biquad_tdf2_d(xi, c->lp, s->lp[0]);
    double h = omx_biquad_tdf2_d(xi, c->hp, s->hp[0]);
    if (c->sections == 2u) {
      l = omx_biquad_tdf2_d(l, c->lp, s->lp[1]);
      h = omx_biquad_tdf2_d(h, c->hp, s->hp[1]);
    }
    lo[i] = (float)l;
    hi[i] = sign * (float)h;
#ifdef OMX_CONTRACTS
    const double r = omx_biquad_tdf2_d(xi, c->ap, s->ref);
    const double d = fabs((double)lo[i] + (double)hi[i] - r), m = fabs(xi);
    worst = d > worst ? d : worst;
    peak = m > peak ? m : peak;
#endif
  }
#ifdef OMX_CONTRACTS
  OMX_POST(worst <= (double)OMX_XOVER_PARTITION_TOL * (peak > 1.0 ? peak : 1.0), "bands-partition-unity");
#endif
  OMX_POST(omx_block_finite(lo, n) && omx_block_finite(hi, n), "finite-out");
  OMX_INVARIANT(omx_block_finite_d(&s->lp[0][0], 4u) && omx_block_finite_d(&s->hp[0][0], 4u) &&
                    omx_block_finite_d(s->ref, 2u),
                "state-finite");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "xover/allpass"
/**
 * @brief The crossover's all-pass over a block in place — what a tree applies to every band below
 *        a later corner so all bands stay in phase.
 * @param buf The block, filtered in place; finite.
 * @param n Samples in the block.
 * @param c The coefficients (omx_xover_design()); `c->ap` is run.
 * @param s The section's state, updated in place.
 * @pre `finite-in`.
 * @post `finite-out`.
 * @invariant `state-finite`, on return.
 * @note RT-safe: one double section per sample, no call. Thread-safe on distinct state.
 */
static inline void omx_xover_allpass(float *buf, uint32_t n, const struct omx_xover *c,
                                     struct omx_xover_ap_state *s) {
  OMX_PRE(omx_block_finite(buf, n), "finite-in");
  for (uint32_t i = 0; i < n; i++) buf[i] = (float)omx_biquad_tdf2_d(buf[i], c->ap, s->s);
  OMX_POST(omx_block_finite(buf, n), "finite-out");
  OMX_INVARIANT(omx_block_finite_d(s->s, 2u), "state-finite");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_XOVER_H */
