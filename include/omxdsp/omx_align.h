// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_align.h (formerly the alignment stage of the console's mix_dsp.h)
 * @brief The input alignment delay: one microphone pushed back to meet another.
 *
 * A per-STRIP input-stage latency: 100 % wet, no feedback, applied beside trim and polarity so
 * everything downstream (the tap points, the analyser rings the measurement reads, EQ, dynamics,
 * the sends) sees the corrected signal. It is not the FX delay (fx/omx_delay.h, a musical echo)
 * and not the console's output delay (a loudspeaker zone, per destination).
 *
 * It is an ORDER-5 FRACTIONAL line ({@link omx_fdelay}), not a whole-sample ring
 * (the ruled fractional-delay design):
 *   - the target is `ms × rate / 1000` fractional samples, never rounded first;
 *   - a whole-sample target is an exact copy (the kernel is skipped), so 0 is a passthrough;
 *   - below OMX_ALIGN_FRACTION_MIN_RATE (44.1/48 kHz, where Lagrange-5 loses 6.67-11.40 dB at
 *     20 kHz) and below the kernel's reach (2 samples) the TARGET is the NEAREST WHOLE SAMPLE —
 *     where the line settles; a move between targets still slews through the kernel;
 *   - a re-point SLEWS the read point by OMX_ALIGN_SLEW per sample and lands exactly — one
 *     continuous read of one signal, never a blend of two taps, so delay review D-2's
 *     quantum-wide hole cannot occur, and a move is block-size invariant;
 *   - ONE target for both legs (delay review D-6): one split and one kernel serve the pair.
 *
 * 100 ms is 34 m of air — further apart than two microphones on one source can be while still
 * hearing that source. The rings are the caller's, allocated lazily on the control thread on the
 * first non-zero delay; a NULL ring is a passthrough.
 *
 * Contract: NO allocation, NO lock, NO libc beyond <math.h>.
 */
#ifndef OMX_ALIGN_H
#define OMX_ALIGN_H

#include <math.h>
#include <stdint.h>

#include <omxdsp/omxdsp_definitions.h>
#include "omx_contract.h"
#include "omx_fdelay.h"

/** @brief The alignment's ceiling, ms: the contract's input alignment travel top (INPUT_ALIGN_DELAY_RANGE.max). */
#define OMX_ALIGN_MAX_MS OMX_INPUT_ALIGN_DELAY_RANGE_MAX
/** @brief Highest graph rate the alignment ring is sized for, so MAX_MS is reachable at every rate. */
#define OMX_ALIGN_MAX_RATE OMX_RT_HARD_TARGET_RATE
/** @brief The Lagrange order the alignment line constructs (fractional-delay-line §3: strip alignment). */
#define OMX_ALIGN_ORDER 5
/** @brief Per-leg ring capacity, samples: MAX_MS at MAX_RATE plus the order-5 kernel's reach — the
 * value of `omx_fdelay_cap_for(19200, OMX_ALIGN_ORDER)`, asserted equal by the align_fdelay oracle. */
#define OMX_ALIGN_CAP (((OMX_ALIGN_MAX_RATE / 1000) * OMX_ALIGN_MAX_MS) + ((OMX_ALIGN_ORDER - 1) / 2) + 2)
/** @brief How fast a re-point moves the read point: samples of delay per sample of time. A ratio, so the
 * pitch excursion during a move (3.1 %) and the time a move takes are the same at every rate. */
#define OMX_ALIGN_SLEW (1.0f / 32.0f)
/** @brief The lowest graph rate at which the fraction is applied; below it the target is the nearest
 * whole sample (smart-alignment §7: the fractional-delay line's §2 numbers). */
#define OMX_ALIGN_FRACTION_MIN_RATE 88200.0f

/** @brief One strip's alignment lines + RT state. Rings are CONTROL-thread owned and NULL until the
 * first non-zero delay ({@link omx_align_arm}); a NULL ring is a passthrough, so the RT needs no
 * other gate. */
struct omx_align_state {
  float *ring_l;                    /**< The left ring, OMX_ALIGN_CAP floats; NULL: a passthrough. */
  float *ring_r;                    /**< The right ring, OMX_ALIGN_CAP floats. */
  uint32_t cap;                     /**< Slots per ring. */
  struct omx_fdelay line_l, line_r; /**< Armed over ring_l / ring_r at OMX_ALIGN_ORDER. */
  float cur;                        /**< RT-owned applied delay, fractional samples, both legs. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "align/arm"
/**
 * @brief Arm a strip's two alignment lines over two zeroed rings of OMX_ALIGN_CAP floats; the
 *        applied delay starts at 0.
 * @param a The state.
 * @param rl The left ring.
 * @param rr The right ring.
 * @return 1 when both lines armed, 0 (and nothing published) otherwise.
 * @pre `align-rings-are-given`.
 * @post `the-ceiling-is-reachable-at-the-highest-rate`.
 * @note CONTROL thread, before the RT can see `ring_l` (the caller fences). No allocation.
 */
static inline int omx_align_arm(struct omx_align_state *a, float *rl, float *rr) {
  OMX_PRE(rl != NULL && rr != NULL, "align-rings-are-given");
  if (rl == NULL || rr == NULL) return 0;
  if (omx_fdelay_init(&a->line_l, rl, OMX_ALIGN_CAP, OMX_ALIGN_ORDER) != OMX_FDELAY_OK) return 0;
  if (omx_fdelay_init(&a->line_r, rr, OMX_ALIGN_CAP, OMX_ALIGN_ORDER) != OMX_FDELAY_OK) return 0;
  a->cur = 0.0f;
  a->cap = OMX_ALIGN_CAP;
  a->ring_r = rr;
  a->ring_l = rl; /* last: the RT's gate */
  OMX_POST(omx_fdelay_max_delay(a->cap, OMX_ALIGN_ORDER) >= (float)((OMX_ALIGN_MAX_RATE / 1000) * OMX_ALIGN_MAX_MS),
           "the-ceiling-is-reachable-at-the-highest-rate");
  return 1;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "align/target"
/**
 * @brief The alignment's TARGET — where the line settles — for `ms` at rate `sr`.
 *
 * `ms × sr / 1000` fractional samples, clamped into [0, OMX_ALIGN_MAX_MS at sr] and the ring, a
 * non-finite or negative request read as 0, and the nearest whole sample below
 * OMX_ALIGN_FRACTION_MIN_RATE or under the kernel's reach. The ONE ms → samples conversion this
 * stage takes; the number a latency or readback publishes.
 * @param ms The requested delay, ms.
 * @param sr The graph rate, Hz.
 * @return The target, samples.
 * @post `the-target-is-inside-the-ring`.
 * @note RT-safe and thread-safe: arithmetic.
 */
static inline float omx_align_target(float ms, float sr) {
  float m = (ms - ms == 0.0f && ms > 0.0f) ? ms : 0.0f;
  if (m > (float)OMX_ALIGN_MAX_MS) m = (float)OMX_ALIGN_MAX_MS;
  /* In double, divided by 1000 rather than multiplied by 0.001f (which is inexact): a dialled
   * delay that IS a whole number of samples (0.5 ms at 96 kHz) arrives as exactly 48.0, so the
   * line takes its exact-copy branch instead of a kernel 4e-6 of a sample off whole. */
  float d = (float)((double)m * (double)sr / 1000.0);
  const float maxd = omx_fdelay_max_delay(OMX_ALIGN_CAP, OMX_ALIGN_ORDER);
  if (!(d >= 0.0f)) d = 0.0f;
  if (d > maxd) d = maxd;
  /* Where the line SETTLES: the nearest whole sample below OMX_ALIGN_FRACTION_MIN_RATE and below
   * the kernel's reach (smart-alignment §7); the moving read point is never rounded here. */
  if (d != floorf(d) && (sr < OMX_ALIGN_FRACTION_MIN_RATE || d < omx_fdelay_min_delay(OMX_ALIGN_ORDER)))
    d = floorf(d + 0.5f);
  OMX_POST(d >= 0.0f && d <= maxd, "the-target-is-inside-the-ring");
  return d;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "align/read"
/**
 * @brief The delay the line READS at an applied position `d`: `d` itself, except a fraction under
 *        the order-5 kernel's reach (2 samples), which reads the nearest whole sample — the only
 *        rounding of a MOVING read point, and only within two samples of zero.
 * @param d The applied position, samples.
 * @return The delivered delay, samples.
 * @pre `the-applied-delay-is-not-negative`.
 * @post `a-fractional-read-clears-the-kernels-reach`.
 * @note RT-safe and thread-safe: arithmetic.
 */
static inline float omx_align_deliverable(float d) {
  OMX_PRE(d >= 0.0f, "the-applied-delay-is-not-negative");
  const float out =
      (d < omx_fdelay_min_delay(OMX_ALIGN_ORDER) && d != floorf(d)) ? floorf(d + 0.5f) : d;
  OMX_POST(out == floorf(out) || out >= omx_fdelay_min_delay(OMX_ALIGN_ORDER),
           "a-fractional-read-clears-the-kernels-reach");
  return out;
}

/**
 * @brief Split delivered delay `dd` into the whole distance `*id` and, unless it is an exact copy,
 *        the kernel `c`; the one split both of omx_align_apply's loops take.
 * @param a The state.
 * @param dd The delivered delay, samples.
 * @param id Out: the whole distance.
 * @param c Out: OMX_FDELAY_MAX_TAPS kernel taps.
 * @return `c`, or NULL for an exact copy.
 * @pre `the-read-is-inside-the-ring`.
 * @post `the-whole-distance-is-inside-the-ring`.
 * @note RT-safe: no allocation, bounded work. RT thread only.
 */
static inline const float *omx_align_kernel(const struct omx_align_state *a, float dd,
                                            uint32_t *id, float *c) {
  OMX_PRE(dd >= 0.0f && dd <= omx_fdelay_max_delay(a->line_l.cap, OMX_ALIGN_ORDER),
          "the-read-is-inside-the-ring");
  float kf = 0.0f;
  if (omx_fdelay_split(&a->line_l, dd, id, &kf)) return NULL;
  omx_fdelay_lagrange(OMX_ALIGN_ORDER, kf, c);
  OMX_POST(*id < a->line_l.cap, "the-whole-distance-is-inside-the-ring");
  return c;
}

/**
 * @brief Sample `i` of both legs through their lines at one split: write, then read `id` behind.
 * @param a The state.
 * @param l The left (or mono) leg.
 * @param r The right leg, or NULL for mono.
 * @param i The frame.
 * @param id The whole distance.
 * @param k The kernel, or NULL for an exact copy.
 * @pre `both-reads-are-inside-the-ring`.
 * @note RT-safe: no allocation, fixed work. RT thread only.
 */
static inline void omx_align_rw(struct omx_align_state *a, float *l, float *r, uint32_t i,
                                uint32_t id, const float *k) {
  OMX_PRE(id < a->line_l.cap && (r == NULL || id < a->line_r.cap), "both-reads-are-inside-the-ring");
  omx_fdelay_write(&a->line_l, l[i]);
  l[i] = omx_fdelay_read_at(&a->line_l, id, k);
  if (r != NULL) {
    omx_fdelay_write(&a->line_r, r[i]);
    r[i] = omx_fdelay_read_at(&a->line_r, id, k);
  }
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "align"
/**
 * @brief Delay a strip's legs in place toward `tgt` samples ({@link omx_align_target}).
 *
 * Steady (`cur == tgt`): one kernel for the block. Moving: the applied delay steps toward `tgt`
 * by OMX_ALIGN_SLEW per sample and lands exactly on it; the kernel follows it per sample. An
 * unarmed state (NULL ring) is a passthrough.
 * @param a The state.
 * @param l The left (or mono) leg.
 * @param r The right leg, or NULL for a mono strip.
 * @param n Frames.
 * @param tgt The target, samples.
 * @pre `the-target-is-inside-the-ring`.
 * @invariant `write-cursor-inside-the-ring`.
 * @note RT-safe: no allocation, bounded work. RT thread only.
 */
static inline void omx_align_apply(struct omx_align_state *a, float *l, float *r, uint32_t n,
                                   float tgt) {
  if (a->ring_l == NULL || l == NULL || n == 0u || a->line_l.order == 0) return;
  OMX_PRE(tgt >= 0.0f && tgt <= omx_fdelay_max_delay(a->cap, OMX_ALIGN_ORDER),
          "the-target-is-inside-the-ring");
  float cur = a->cur;
  uint32_t i = 0u, id = 0u;
  float c[OMX_FDELAY_MAX_TAPS];
  for (; i < n && cur != tgt; i++) {
    const float gap = tgt - cur;
    cur = (gap > OMX_ALIGN_SLEW) ? cur + OMX_ALIGN_SLEW
          : (gap < -OMX_ALIGN_SLEW) ? cur - OMX_ALIGN_SLEW
                                    : tgt;
    const float *k = omx_align_kernel(a, omx_align_deliverable(cur), &id, c);
    omx_align_rw(a, l, r, i, id, k);
  }
  a->cur = cur;
  if (i < n) {
    const float *k = omx_align_kernel(a, omx_align_deliverable(cur), &id, c);
    for (; i < n; i++) omx_align_rw(a, l, r, i, id, k);
  }
  OMX_INVARIANT(a->line_l.wpos < a->line_l.cap, "write-cursor-inside-the-ring");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_ALIGN_H */
