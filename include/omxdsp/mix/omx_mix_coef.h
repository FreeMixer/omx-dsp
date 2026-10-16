// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_mix_coef.h
 * @brief The coefficients of the summing matrix G: a strip's gains per speaker (level × pan or
 *        balance, or the two factors apart), the mono fold's gain, the fillers that turn them into
 *        omx_mixmatrix_sparse() entries, the silent-source test and the settled-silence test.
 *
 * Strip coefficients (gain × fader × mute × pan) are the function that fills G, the mono fold is entries in G and not
 * a code path, and a silent source has its entries left out of the sparse list for the block (the
 * test is here; leaving them out is the engine's list building). Which sources feed which output
 * is routing, and stays the engine's.
 *
 * The gains are the engine's own arithmetic bit for bit: `omx_strip_eff`, `omx_strip_coefs` and
 * `omx_strip_main_coefs` (mix_dsp.h), `omx_strip_main_coefs_lcr` (mix_lcr.h), the factored
 * `omx_sum_main_split` (mix_sum.h), the fold's `tgt × fold_coef` (mix_bus.h, mixer_rt.c) and the
 * settled-silence test `omx_strip_silent` (mix_dsp.h).
 *
 * Stateless, pure, header-only. Not in omxdsp.h: include <omxdsp/mix/omx_mix_coef.h>.
 */
#ifndef OMX_MIX_COEF_H
#define OMX_MIX_COEF_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include <omxdsp/omx_balance_law.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_mixmatrix.h>
#include <omxdsp/mix/omx_meter.h>
#include <omxdsp/mix/omx_pan_law.h>

/**
 * @brief A strip's level before pan: gain × fader × mute, linear amplitude.
 *
 * The scalar the post-fader meter tracks too, so the meter follows exactly what the mix applies.
 * @param gain The head gain, linear.
 * @param fader The fader, linear.
 * @param mute Non-zero: muted (level 0).
 * @return gain × fader × (mute ? 0 : 1).
 * @note RT-safe: two multiplies. Thread-safe: pure.
 */
static inline float omx_mix_strip_level(float gain, float fader, int mute) {
  return gain * fader * (mute ? 0.0f : 1.0f);
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "mix/strip-gains"
/**
 * @brief A strip's gain into each speaker of a layout: its level times the pan law (a mono
 *        source) or the balance law (a stereo source).
 *
 * A mono source (`channels < 2`) is spread by omx_pan_gains(). A stereo source keeps its image:
 * its left leg goes to the first axis speaker and its right leg to the last, each scaled by
 * omx_balance_law(), and it puts nothing into a discrete centre. `active == 0` (the strip is not
 * assigned to this bus) gives every gain 0 and nothing else.
 * @param l The bus's layout.
 * @param channels The strip's channel count: 1 mono, 2 or more stereo.
 * @param active Non-zero: the strip is assigned to this bus.
 * @param gain The head gain, linear.
 * @param fader The fader, linear.
 * @param mute Non-zero: muted.
 * @param pan The pan (mono) or balance (stereo) position, [-1, +1].
 * @param divergence The centre's divergence (omx_pan_gains()); ignored without a centre.
 * @param g Out: omx_pan_layout_speakers(l) gains, in the layout's speaker order.
 * @pre `stereo-source-on-a-pair`: a stereo source's layout has two axis speakers.
 * @note RT-safe: at most one cosf, one sinf and one sqrtf. Thread-safe: pure.
 */
static inline void omx_mix_strip_gains(const struct omx_pan_layout *l, int channels, int active,
                                       float gain, float fader, int mute, float pan,
                                       float divergence, float *g) {
  const uint32_t ns = omx_pan_layout_speakers(l);
  if (!active) {
    for (uint32_t i = 0; i < ns; i++) g[i] = 0.0f;
    return;
  }
  const float eff = omx_mix_strip_level(gain, fader, mute);
  if (channels >= 2) {
    OMX_PRE(l->n_axis == 2u, "stereo-source-on-a-pair");
    float a, b;
    omx_balance_law(pan, &a, &b);
    for (uint32_t i = 0; i < ns; i++) g[i] = 0.0f;
    g[0] = eff * a;
    g[l->n_axis - 1u] = eff * b;
    return;
  }
  omx_pan_gains(l, pan, divergence, g);
  for (uint32_t i = 0; i < ns; i++) g[i] = eff * g[i];
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "mix/strip-split"
/**
 * @brief A strip's gains into a bus as two factors: its level and its image, one gain per speaker,
 *        so that `coef × img[k]` is omx_mix_strip_gains()'s `g[k]` bit for bit.
 *
 * The image is the pan law (a mono source) or the balance law (a stereo source) alone; the level
 * is omx_mix_strip_level(). A bus that keeps its level and its picture apart (a bus sum that ramps
 * the level and holds the image) takes these instead of the product. `coef × img[k]` is the same
 * float multiplication, in the same order and on the same operands, as omx_mix_strip_gains().
 * `active == 0` gives a zero level AND a zero image: gating only the level would make an unassigned
 * hard-right mono strip's left gain `+0 × −4.4e-08 = −0`, where the product is `+0`. Moved from the
 * engine's `omx_sum_main_split` (OpenMixer's mix_sum.h), bit for bit, and widened to any layout.
 * @param l The bus's layout.
 * @param channels The strip's channel count: 1 mono, 2 or more stereo.
 * @param active Non-zero: the strip is assigned to this bus.
 * @param gain The head gain, linear.
 * @param fader The fader, linear.
 * @param mute Non-zero: muted.
 * @param pan The pan (mono) or balance (stereo) position, [-1, +1].
 * @param divergence The centre's divergence (omx_pan_gains()); ignored without a centre.
 * @param coef Out: the level; 0 when unassigned or muted.
 * @param img Out: omx_pan_layout_speakers(l) gains, in the layout's speaker order.
 * @pre `outs-present`; `finite-controls`: the gain and the fader are finite;
 *      `stereo-source-on-a-pair`: a stereo source's layout has two axis speakers.
 * @post `image-is-a-gain`: every image gain in [−1e-6, 1] (a far end reads −4.4e-08, a rounding of
 *       zero); `a-muted-channel-is-silent`: a muted strip's level is 0; `an-unassigned-channel-is-silent`.
 * @note RT-safe: at most one cosf, one sinf and one sqrtf. Thread-safe: pure.
 */
static inline void omx_mix_strip_split(const struct omx_pan_layout *l, int channels, int active,
                                       float gain, float fader, int mute, float pan,
                                       float divergence, float *coef, float *img) {
  OMX_PRE(coef != NULL && img != NULL, "outs-present");
  OMX_PRE(gain - gain == 0.0f && fader - fader == 0.0f, "finite-controls");
  const uint32_t ns = omx_pan_layout_speakers(l);
  if (!active) {
    *coef = 0.0f;
    for (uint32_t i = 0; i < ns; i++) img[i] = 0.0f;
    OMX_POST(*coef == 0.0f, "an-unassigned-channel-is-silent");
    return;
  }
  if (channels >= 2) {
    OMX_PRE(l->n_axis == 2u, "stereo-source-on-a-pair");
    float a, b;
    omx_balance_law(pan, &a, &b);
    for (uint32_t i = 0; i < ns; i++) img[i] = 0.0f;
    img[0] = a;
    img[l->n_axis - 1u] = b;
  } else {
    omx_pan_gains(l, pan, divergence, img);
  }
  *coef = omx_mix_strip_level(gain, fader, mute);
#ifdef OMX_CONTRACTS
  int in_range = 1;
  for (uint32_t i = 0; i < ns; i++) in_range &= img[i] >= -1e-6f && img[i] <= 1.0f;
  OMX_POST(in_range, "image-is-a-gain");
#endif
  OMX_POST(mute == 0 || *coef == 0.0f, "a-muted-channel-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Which row of X feeds each speaker for a strip: a mono strip's one row everywhere; a
 *        stereo strip's left row for the speakers left of centre and the centre, its right row
 *        for the rest.
 * @param l The bus's layout.
 * @param channels The strip's channel count: 1 mono, 2 or more stereo.
 * @param row_l The strip's left (or only) row of X.
 * @param row_r The strip's right row of X; ignored for a mono strip.
 * @param rows Out: omx_pan_layout_speakers(l) rows.
 * @note RT-safe: one pass over the speakers. Thread-safe: pure.
 */
static inline void omx_mix_strip_rows(const struct omx_pan_layout *l, int channels, uint32_t row_l,
                                      uint32_t row_r, uint32_t *rows) {
  const uint32_t ns = omx_pan_layout_speakers(l);
  for (uint32_t i = 0; i < ns; i++)
    rows[i] = (channels >= 2 && i < l->n_axis && l->pos[i] > 0.0f) ? row_r : row_l;
}

/**
 * @brief The mono fold's gain: a level times the per-leg fold coefficient, and 0 when that is
 *        not finite.
 *
 * A stereo source folded into one mono output lands as (L + R) × this, so a correlated source
 * arrives at unity with the default 0.5 (−6 dB) instead of +6 dB hot. A non-finite product reads
 * as 0, the engine's fail-safe: one bad control value must not keep a bus at NaN.
 * @param level The contribution's level (a send's coefficient, or a route's gain × mute).
 * @param fold_coef The per-leg fold coefficient, linear (0.5 by default).
 * @return level × fold_coef, or 0 when that is NaN or infinite.
 * @note RT-safe: one multiply, one isfinite. Thread-safe: pure.
 */
static inline float omx_mix_fold_gain(float level, float fold_coef) {
  float t = level * fold_coef;
  if (!isfinite(t)) t = 0.0f;
  return t;
}

/**
 * @brief Write the sparse entries of G for a set of gains, leaving out each one that is 0 and
 *        has already settled at 0.
 *
 * An entry still ramping to 0 (`g_prev != 0`) stays in: cutting a ramp short is a click.
 * @param e Out: up to `n` entries; the first return-value many are written.
 * @param rows The row of X each gain reads.
 * @param outs The row of Y each gain writes.
 * @param g_prev Each gain at the start of the block (the previous block's `g_cur`).
 * @param g_cur Each gain at the end of the block.
 * @param n Gains.
 * @return The entries written.
 * @note RT-safe: one pass, no allocation. Thread-safe: writes only `e`.
 */
static inline uint32_t omx_mix_fill_entries(struct omx_mixmatrix_entry *e, const uint32_t *rows,
                                            const uint32_t *outs, const float *g_prev,
                                            const float *g_cur, uint32_t n) {
  uint32_t k = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (g_prev[i] == 0.0f && g_cur[i] == 0.0f) continue;
    e[k].strip = rows[i];
    e[k].out = outs[i];
    e[k].g_prev = g_prev[i];
    e[k].g_cur = g_cur[i];
    k++;
  }
  return k;
}

/**
 * @brief Write the sparse entries of G that fold a stereo source into one mono output: its left
 *        row and its right row, each at the fold gain.
 *
 * The pair is left out when the gain is 0 and has already settled at 0. A mono source passes its
 * one row twice, as the engine reads a mono source as `inR == inL`.
 * @param e Out: up to 2 entries.
 * @param row_l The source's left row of X.
 * @param row_r The source's right row of X.
 * @param out The mono output's row of Y.
 * @param g_prev The fold gain at the start of the block (omx_mix_fold_gain()).
 * @param g_cur The fold gain at the end of the block.
 * @return The entries written: 0 or 2.
 * @note RT-safe: stores only. Thread-safe: writes only `e`.
 */
static inline uint32_t omx_mix_fill_fold(struct omx_mixmatrix_entry *e, uint32_t row_l,
                                         uint32_t row_r, uint32_t out, float g_prev, float g_cur) {
  if (g_prev == 0.0f && g_cur == 0.0f) return 0u;
  e[0].strip = row_l;
  e[0].out = out;
  e[0].g_prev = g_prev;
  e[0].g_cur = g_cur;
  e[1].strip = row_r;
  e[1].out = out;
  e[1].g_prev = g_prev;
  e[1].g_cur = g_cur;
  return 2u;
}

/**
 * @brief Whether a source is silent this block: its block peak is below `floor_lin`.
 *
 * A silent source's entries may be left out of the sparse list for the block; the engine's list
 * building decides that.
 * @param a The source's first leg, `n` samples.
 * @param b Its second leg; `b == a` (mono) or NULL (one leg).
 * @param n Samples per leg.
 * @param floor_lin The level, linear, below which the block counts as silent; 0 makes nothing silent.
 * @return true when omx_meter_block_peak() is strictly below `floor_lin`.
 * @note RT-safe: one pass, no allocation. Thread-safe: reads only.
 */
static inline bool omx_mix_source_silent(const float *a, const float *b, uint32_t n, float floor_lin) {
  return omx_meter_block_peak(a, b, n) < floor_lin;
}

/**
 * @brief Whether a strip has settled into silence: both ramp targets AND both current ramp values
 *        are exactly 0.
 *
 * Only then may a caller skip the strip for the block: a target of 0 with a current value that is
 * not is a ramp down still owed, and skipping it would drop it (a click). Moved from the engine's
 * `omx_strip_silent` (OpenMixer's mix_dsp.h), bit for bit.
 * @param tgtL The left target.
 * @param tgtR The right target.
 * @param curL The left ramp's current value.
 * @param curR The right ramp's current value.
 * @return true when all four are 0 (either sign).
 * @note RT-safe: four compares. Thread-safe: pure.
 */
static inline bool omx_mix_settled_silent(float tgtL, float tgtR, float curL, float curR) {
  return tgtL == 0.0f && tgtR == 0.0f && curL == 0.0f && curR == 0.0f;
}

#endif /* OMX_MIX_COEF_H */
