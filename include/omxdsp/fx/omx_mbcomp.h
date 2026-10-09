// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_mbcomp.h
 * @brief The multiband compressor: a Linkwitz-Riley 4 crossover tree splitting each leg into
 *        2 to OMX_MBC_MAX_BANDS in-phase bands, one compressor per band, the bands summed.
 *
 * Spec: docs/design/specs/2026-09-26-native-multiband-compressor.md §1–§5 (openmixer#920). Composed
 * from the library words only: {@link omx_xover_process} splits, {@link omx_xover_allpass} puts every
 * band below a later corner in phase, {@link omx_dynamics_keyed} is each band's compressor with its
 * OWN atom and state. Band k's path is `HP(f1)…HP(f(k−1))·LP(fk)·AP(f(k+1))…AP(f(N−1))`, so the band
 * magnitudes partition unity and share one phase: at rest the stage is the allpass `Π AP(fk)` (L1),
 * and with held gains `|Y/X| = Σ g_k·|B_k|`, a convex combination (L3).
 *
 * One oversampling factor for the whole stage ({@link omx_mbcomp_factor}): every band's audio takes
 * the same tap or the bands sum into a comb (L6). A band that is off runs a unity atom, so it takes
 * that tap too and its gain is exactly 1.
 *
 * The stage's numbers are core's `MBC_*` (openmixer `packages/core/src/multiband-limits.ts`, spec
 * §3). omx-contract carries no multiband row, so the four this kernel computes with are defined
 * here under the names its render spells; `tools/contract-single-source.sh` names each of them
 * the day a release carries the row, and they leave this file then.
 *
 * Not in the omxdsp.h umbrella, as no fx/ header is. Contract: NO PipeWire, NO allocation, NO lock.
 * The atom {@link omx_mbcomp} is a per-block snapshot whose crossovers are designed on the control
 * thread ({@link omx_mbcomp_design}); the state {@link omx_mbcomp_state} is inline and sized by
 * OMX_MBC_MAX_BANDS.
 */
#ifndef OMX_MIX_MBCOMP_H
#define OMX_MIX_MBCOMP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/omx_xover.h>

/** @brief The band floor: `MBC_MIN_BANDS`, the floor of core's `MBC_LIMITS.bandCount` (spec §3b). */
#define OMX_MBC_MIN_BANDS 2
/** @brief The band capacity: `MBC_MAX_BANDS`, the ceiling of `MBC_LIMITS.bandCount`; sizes the inline state. */
#define OMX_MBC_MAX_BANDS 5
/** @brief `MBC_CORNER_MAX_FRACTION` (spec §4 L10): a corner at or above this fraction of the live rate is designed AT it. */
#define OMX_MBC_CORNER_MAX_FRACTION 0.45f
/** @brief `MBC_CROSSOVER_DEFAULTS_2` (spec §3c): the come-up corner of two bands, Hz. */
#define OMX_MBC_CROSSOVER_DEFAULTS_2_INIT { 1000 }
/** @brief `MBC_CROSSOVER_DEFAULTS_3` (spec §3c): the come-up corners of three bands, Hz. */
#define OMX_MBC_CROSSOVER_DEFAULTS_3_INIT { 200, 2000 }
/** @brief `MBC_CROSSOVER_DEFAULTS_4` (spec §3c): the come-up corners of four bands, Hz. */
#define OMX_MBC_CROSSOVER_DEFAULTS_4_INIT { 150, 800, 5000 }
/** @brief `MBC_CROSSOVER_DEFAULTS_5` (spec §3c): the come-up corners of five bands, Hz. */
#define OMX_MBC_CROSSOVER_DEFAULTS_5_INIT { 100, 400, 1600, 6400 }

/** @brief Crossovers in a full tree. */
#define OMX_MBC_MAX_XOVERS (OMX_MBC_MAX_BANDS - 1u)

/**
 * @brief §3c's come-up corners by band count, row `bands`: the one C copy, read through
 *        omx_mbcomp_come_up_hz().
 */
static const float OMX_MBC_COME_UP_HZ[OMX_MBC_MAX_BANDS + 1u][OMX_MBC_MAX_XOVERS] = {
    [2] = OMX_MBC_CROSSOVER_DEFAULTS_2_INIT,
    [3] = OMX_MBC_CROSSOVER_DEFAULTS_3_INIT,
    [4] = OMX_MBC_CROSSOVER_DEFAULTS_4_INIT,
    [5] = OMX_MBC_CROSSOVER_DEFAULTS_5_INIT,
};

/**
 * @brief The come-up corners for `bands` bands (§3c), ascending.
 * @param bands OMX_MBC_MIN_BANDS … OMX_MBC_MAX_BANDS; outside it, the floor's row.
 * @return `bands − 1` corners, Hz.
 * @note RT-safe, thread-safe: a read-only table.
 */
static inline const float *omx_mbcomp_come_up_hz(uint32_t bands) {
  return OMX_MBC_COME_UP_HZ[(bands >= OMX_MBC_MIN_BANDS && bands <= OMX_MBC_MAX_BANDS) ? bands : OMX_MBC_MIN_BANDS];
}

/** @brief The stage's resolved controls, one block's snapshot. */
struct omx_mbcomp {
  int enabled;                                /**< 0: a no-op that touches no sample and no state word (L2). */
  uint32_t bands;                             /**< Bands in use, OMX_MBC_MIN_BANDS … OMX_MBC_MAX_BANDS. */
  int ovs_mode;                               /**< OMX_DYN_OVS_AUTO / _OFF / _X4 — ONE choice for the stage. */
  struct omx_xover xo[OMX_MBC_MAX_XOVERS];    /**< The LR4 crossovers, ascending; `bands − 1` in use. */
  struct omx_dyn band[OMX_MBC_MAX_BANDS];     /**< Each band's compressor atom; `enabled == 0` is a band that is off. */
};

/** @brief One leg's crossover tree state. */
struct omx_mbcomp_leg {
  struct omx_xover_state xs[OMX_MBC_MAX_XOVERS];                       /**< Crossover k's sections. */
  struct omx_xover_ap_state ap[OMX_MBC_MAX_XOVERS][OMX_MBC_MAX_XOVERS]; /**< `ap[k][j]`: band k's compensation at corner j > k. */
};

/** @brief The stage's state: two legs' trees and one compressor state per band. */
struct omx_mbcomp_state {
  struct omx_mbcomp_leg leg[2];                /**< Leg L, leg R. */
  struct omx_dyn_state dyn[OMX_MBC_MAX_BANDS]; /**< Band k's detector, oversamplers and tap. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "mbcomp/design"
/**
 * @brief Design the stage's crossovers at `hz` (ascending), each an LR4 at its corner, a corner at
 *        or above `OMX_MBC_CORNER_MAX_FRACTION·sr` designed AT it (L10).
 * @param m The atom; `bands` and `xo` are written.
 * @param bands Band count, OMX_MBC_MIN_BANDS … OMX_MBC_MAX_BANDS.
 * @param hz `bands − 1` corners, Hz, strictly increasing.
 * @param sr The live rate, Hz; a declared rate.
 * @return OMX_XOVER_OK, or the first refusal (the stage then keeps that crossover the wire).
 * @pre `band-count-declared`, `corners-increasing`.
 * @note Control-thread word: one `tan` per corner, no allocation. Thread-safe: writes only `m`.
 */
static inline enum omx_xover_status omx_mbcomp_design(struct omx_mbcomp *m, uint32_t bands,
                                                      const float *hz, double sr) {
  const int count_ok = bands >= OMX_MBC_MIN_BANDS && bands <= OMX_MBC_MAX_BANDS;
  OMX_PRE(count_ok, "band-count-declared");
  m->bands = count_ok ? bands : OMX_MBC_MIN_BANDS;
  int rising = 1;
  for (uint32_t k = 1; k + 1u < m->bands; k++) rising &= hz[k] > hz[k - 1u];
  OMX_PRE(rising, "corners-increasing");
  (void)rising;
  enum omx_xover_status st = OMX_XOVER_OK;
  const double top = (double)OMX_MBC_CORNER_MAX_FRACTION * sr;
  for (uint32_t k = 0; k + 1u < m->bands; k++) {
    const double f = (double)hz[k] < top ? (double)hz[k] : top;
    const enum omx_xover_status s = omx_xover_design(&m->xo[k], 4u, f, sr);
    if (st == OMX_XOVER_OK) st = s;
  }
  return st;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The stage's ONE oversampling factor, derived: 4 when `ovs_mode` is X4, or AUTO with any
 *        band that is on attacking faster than OMX_DYN_OVS_AUTO_MS; else 1. A disabled stage is 1.
 * @param m The atom.
 * @return 1 or 4 — what every band runs at and what the stage's latency term reads (L6).
 * @note RT-safe, thread-safe: reads `m` only.
 */
static inline uint32_t omx_mbcomp_factor(const struct omx_mbcomp *m) {
  if (!m->enabled || m->ovs_mode == OMX_DYN_OVS_OFF) return 1u;
  if (m->ovs_mode == OMX_DYN_OVS_X4) return 4u;
  uint32_t f = 1u;
  for (uint32_t k = 0; k < m->bands && k < OMX_MBC_MAX_BANDS; k++) {
    const struct omx_dyn one = {.enabled = m->band[k].enabled, .ovs_mode = OMX_DYN_OVS_AUTO,
                                .attack_ms = m->band[k].attack_ms};
    if (omx_dyn_oversample_factor(&one) == 4u) f = 4u;
  }
  return f;
}

/**
 * @brief Band k's atom as the stage runs it: the band's own, forced to the stage's factor; a band
 *        that is off becomes the unity compressor (ratio 1, no knee, no make-up), so its gain is
 *        exactly 1 and its audio takes the same tap as every other band's.
 * @param m The atom.
 * @param k Band index, below `m->bands`.
 * @param factor The stage factor (omx_mbcomp_factor()).
 * @return The resolved band atom.
 * @note RT-safe, thread-safe: a value.
 */
static inline struct omx_dyn omx_mbcomp_band_atom(const struct omx_mbcomp *m, uint32_t k, uint32_t factor) {
  struct omx_dyn d = m->band[k];
  if (!d.enabled) {
    d.gc.mode = OMX_DYN_ABOVE;
    d.gc.thresh_db = 0.0f;
    d.gc.ratio = 1.0f;
    d.gc.knee_db = 0.0f;
    d.gc.range_db = 0.0f;
    d.gc.makeup_lin = 1.0f;
    d.detect = OMX_DETECT_RMS;
    d.attack_coeff = 0.0f;
    d.release_coeff = 0.0f;
    d.attack_ms = 0.0f;
  }
  d.enabled = 1;
  d.ovs_mode = factor == 4u ? OMX_DYN_OVS_X4 : OMX_DYN_OVS_OFF;
  return d;
}

/**
 * @brief Clear the stage's state: silent trees, every band's compressor armed at the stage factor.
 * @param st The state.
 * @param factor The stage factor (omx_mbcomp_factor()), 1 or 4; every band is armed at it.
 * @note Control-thread word. Thread-safe on distinct state.
 */
static inline void omx_mbcomp_state_init(struct omx_mbcomp_state *st, uint32_t factor) {
  memset(st, 0, sizeof *st);
  for (uint32_t k = 0; k < OMX_MBC_MAX_BANDS; k++) omx_dyn_state_init(&st->dyn[k], factor);
}

/**
 * @brief Band k's live gain reduction, dB (≤ 0 plus its make-up), re-run from its envelope — the
 *        per-band meter's reading, one derivation with the sample it describes.
 * @param m The atom.
 * @param st The state.
 * @param k Band index, below `m->bands`.
 * @return The gain computer's answer at the band's detector level; 0 for a band that is off.
 * @note RT-safe, thread-safe: reads only.
 */
static inline float omx_mbcomp_band_gr_db(const struct omx_mbcomp *m, const struct omx_mbcomp_state *st,
                                          uint32_t k) {
  if (!m->band[k].enabled) return 0.0f;
  const struct omx_env_params e = omx_dyn_env_params(&m->band[k]);
  const float lev = omx_env_level(&st->dyn[k].env, &e);
  return omx_gaincomp_db(&m->band[k].gc, omx_lin_to_db(lev));
}

/**
 * @brief Split one leg's chunk into the in-phase bands.
 * @param x The leg's chunk; finite; read only.
 * @param band `bands` scratch blocks of `n`, written.
 * @param n Samples, at most OMX_DYN_OVS_CHUNK.
 * @param m The atom.
 * @param leg The leg's tree state.
 * @note RT-safe: `bands − 1` crossovers and `(bands − 1)(bands − 2)/2` allpasses per sample.
 */
static inline void omx_mbcomp_split(const float *x, float band[][OMX_DYN_OVS_CHUNK], uint32_t n,
                                    const struct omx_mbcomp *m, struct omx_mbcomp_leg *leg) {
  const uint32_t nx = m->bands - 1u;
  for (uint32_t k = 0; k < nx; k++) omx_xover_process(k == 0 ? x : band[k], band[k], band[k + 1u], n, &m->xo[k], &leg->xs[k]);
  for (uint32_t k = 0; k + 1u < nx; k++)
    for (uint32_t j = k + 1u; j < nx; j++) omx_xover_allpass(band[k], n, &m->xo[j], &leg->ap[k][j]);
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "mbcomp"
/**
 * @brief Run the stage on a strip's leg(s) in place.
 * @param l Leg L, `n` samples; finite.
 * @param r Leg R, or NULL for a mono strip; finite.
 * @param n Samples.
 * @param m The atom (omx_mbcomp_design() for its crossovers).
 * @param st The state.
 * @pre `finite-in-l`, `finite-in-r`; when enabled, `band-count-declared`.
 * @post `finite-out`.
 * @note RT-safe: no allocation, lock or syscall; scratch of OMX_MBC_MAX_BANDS × 2 × OMX_DYN_OVS_CHUNK
 *       floats on the stack whatever `n`. Thread-safe on distinct state.
 */
static inline void omx_mbcomp_process(float *l, float *r, uint32_t n, const struct omx_mbcomp *m,
                                      struct omx_mbcomp_state *st) {
  OMX_PRE_LEGS_FINITE(l, r, n);
  if (!m->enabled || n == 0) return;
  const int count_ok = m->bands >= OMX_MBC_MIN_BANDS && m->bands <= OMX_MBC_MAX_BANDS;
  OMX_PRE(count_ok, "band-count-declared");
  if (!count_ok) return;
  const uint32_t factor = omx_mbcomp_factor(m);
  struct omx_dyn atom[OMX_MBC_MAX_BANDS];
  for (uint32_t k = 0; k < m->bands; k++) atom[k] = omx_mbcomp_band_atom(m, k, factor);
  float bl[OMX_MBC_MAX_BANDS][OMX_DYN_OVS_CHUNK], br[OMX_MBC_MAX_BANDS][OMX_DYN_OVS_CHUNK];
  for (uint32_t done = 0; done < n;) {
    const uint32_t c = (n - done > OMX_DYN_OVS_CHUNK) ? OMX_DYN_OVS_CHUNK : (n - done);
    omx_mbcomp_split(l + done, bl, c, m, &st->leg[0]);
    if (r) omx_mbcomp_split(r + done, br, c, m, &st->leg[1]);
    for (uint32_t k = 0; k < m->bands; k++) omx_dynamics_keyed(bl[k], r ? br[k] : NULL, NULL, c, &atom[k], &st->dyn[k]);
    for (uint32_t i = 0; i < c; i++) {
      float yl = 0.0f, yr = 0.0f;
      for (uint32_t k = 0; k < m->bands; k++) {
        yl += bl[k][i];
        yr += r ? br[k][i] : 0.0f;
      }
      l[done + i] = yl;
      if (r) r[done + i] = yr;
    }
    done += c;
  }
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_MBCOMP_H */
