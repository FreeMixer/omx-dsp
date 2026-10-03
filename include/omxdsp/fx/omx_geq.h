// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_geq.h
 * @brief The 31-band graphic EQ kernel: {@link OMX_GEQ_BANDS} peaking sections at the ISO
 *        third-octave centres, in their OWN section array, run by the library's double cascade.
 *
 * Spec: docs/design/specs/2026-09-26-graphic-eq-31.md §4, §5 finding, §6. Composed from the library
 * words only: {@link omx_biquad_cascade_d_stereo}, the double-precision cascade, runs the sections
 * over the GEQ's own array (never a claimant on `OMX_EQ_MAX_BANDS`, roster ruling 3), because a
 * float32 section at 20 Hz, Q 4.3 misses its design by 2.4 dB at 192 kHz; {@link omx_flush_d}
 * clears the history. The sections are designed on the control thread; the kernel reads no mode, no
 * centre and no Q, so `standard` and `true` differ only in the gains handed to it (§4 L4).
 *
 * A section whose gain is exactly `0.0f` is SKIPPED, and after every block its history is set to
 * the identity's — its input history, which is the previous section's output history, or the
 * block's last two input samples for section 0 — so flat is memcmp-identical to bypass and a band
 * engaged later starts from the state it would have had (§4 L2).
 *
 * Contract: NO PipeWire, NO napi, NO allocation, NO lock, NO libc beyond <math.h>/<string.h>. The
 * atom {@link omx_geq} is a per-block snapshot; the state {@link omx_geq_state} is plain inline
 * doubles.
 */
#ifndef OMX_MIX_GEQ_H
#define OMX_MIX_GEQ_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_denormal.h>

/**
 * @brief The graphic EQ's section count, the ISO third-octave set's length. OMX_GEQ_BANDS comes
 *        from the generated omx_contract_limits.h (declared once, in core's eq.ts).
 */

/** @brief The graphic EQ's resolved controls: one coefficient set and one live byte per section. */
struct omx_geq {
  int enabled;                   /**< 0: a no-op that touches no sample and no state word. */
  double c[OMX_GEQ_BANDS][5];    /**< `{b0, b1, b2, a1, a2}` per section, in double, designed off the RT thread. */
  uint8_t live[OMX_GEQ_BANDS];   /**< 0 where the section's gain is exactly 0 dB: skipped. */
};

/** @brief The graphic EQ's working state: one DF-I history per section per leg. */
struct omx_geq_state {
  double l[OMX_GEQ_BANDS][4]; /**< The left leg's `{x₁, x₂, y₁, y₂}` per section, in double. */
  double r[OMX_GEQ_BANDS][4]; /**< The right leg's. */
};

_Static_assert(sizeof(struct omx_geq_state) == sizeof(double) * 8u * OMX_GEQ_BANDS,
               "graphic EQ state must stay plain inline doubles - nothing to allocate");

/**
 * @brief Clear the state.
 * @param st The state.
 * @note CONTROL thread only.
 */
static inline void omx_geq_state_init(struct omx_geq_state *st) { memset(st, 0, sizeof(*st)); }

/**
 * @brief The latency the graphic EQ adds, in samples: zero at every rate, engaged or not (§4 L1).
 * @param p The atom.
 * @return 0.
 */
static inline int omx_geq_latency(const struct omx_geq *p) {
  (void)p;
  return 0;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "geq/set"
/**
 * @brief Build the atom from the designed sections and the gains they were designed at.
 * @param p The atom, written whole.
 * @param on Non-zero to engage the stage.
 * @param coeffs One `{b0, b1, b2, a1, a2}` set per section.
 * @param section_db The gain each section was designed at, dB; exactly `0.0f` skips it.
 * @pre `finite-coeffs` for every live section, `finite-gains`.
 * @post `live-iff-gain-nonzero`.
 * @note CONTROL thread only. Thread-safe on distinct atoms.
 */
static inline void omx_geq_set(struct omx_geq *p, int on, const double coeffs[][5],
                               const float section_db[OMX_GEQ_BANDS]) {
  OMX_PRE(omx_block_finite(section_db, OMX_GEQ_BANDS), "finite-gains");
  p->enabled = on != 0;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    memcpy(p->c[k], coeffs[k], sizeof p->c[k]);
    p->live[k] = (uint8_t)(section_db[k] != 0.0f);
    OMX_PRE(!p->live[k] || (isfinite(p->c[k][0]) && isfinite(p->c[k][1]) && isfinite(p->c[k][2]) &&
                            isfinite(p->c[k][3]) && isfinite(p->c[k][4])),
            "finite-coeffs");
    OMX_POST(p->live[k] == (section_db[k] != 0.0f), "live-iff-gain-nonzero");
  }
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "geq/settle"
/**
 * @brief After a block: prime every skipped section's history as the identity's and flush every
 *        history word, walking the sections in order so each input history is the one before it.
 * @param live The atom's live bytes.
 * @param s One leg's histories.
 * @param x1 The leg's last input sample of the block, before any section ran.
 * @param x2 The one before it.
 * @post `no-denormal-state`.
 * @note RT-safe: O(OMX_GEQ_BANDS), no call but omx_flush_d(). Thread-safe on distinct state.
 */
static inline void omx_geq_settle(const uint8_t live[OMX_GEQ_BANDS], double s[OMX_GEQ_BANDS][4],
                                  double x1, double x2) {
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    if (!live[k]) {
      s[k][0] = x1;
      s[k][1] = x2;
      s[k][2] = x1;
      s[k][3] = x2;
    }
    for (int j = 0; j < 4; j++) s[k][j] = omx_flush_d(s[k][j]);
    x1 = s[k][2];
    x2 = s[k][3];
    OMX_POST(fpclassify(s[k][2]) != FP_SUBNORMAL && fpclassify(s[k][3]) != FP_SUBNORMAL,
             "no-denormal-state");
  }
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "geq/process"
/**
 * @brief Run the graphic EQ over one block of one or two legs in place.
 * @param l The first leg, filtered in place.
 * @param r The second leg, filtered in place, or NULL for one leg.
 * @param n Samples in the block.
 * @param p The atom.
 * @param st The state.
 * @pre `finite-in`.
 * @post `finite-out`.
 * @note RT-safe: O(live sections · n) through omx_biquad_cascade_d_stereo(), O(OMX_GEQ_BANDS) per
 *       block to settle; no allocation, no lock. Thread-safe on distinct state; reentrant.
 */
static inline void omx_geq_process(float *l, float *r, uint32_t n, const struct omx_geq *p,
                                   struct omx_geq_state *st) {
  OMX_PRE(omx_lane_finite(l, r ? r : l, n), "finite-in");
  if (!p->enabled || n == 0u) return;
  const double l1 = l[n - 1u], l2 = n > 1u ? (double)l[n - 2u] : st->l[0][0];
  const double r1 = r ? (double)r[n - 1u] : 0.0, r2 = r ? (n > 1u ? (double)r[n - 2u] : st->r[0][0]) : 0.0;
  omx_biquad_cascade_d_stereo(l, r, n, OMX_GEQ_BANDS, (const double(*)[5])p->c, p->live, st->l, st->r);
  omx_geq_settle(p->live, st->l, l1, l2);
  if (r) omx_geq_settle(p->live, st->r, r1, r2);
  OMX_POST(omx_lane_finite(l, r ? r : l, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif
