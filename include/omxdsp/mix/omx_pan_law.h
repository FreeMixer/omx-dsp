// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_pan_law.h
 * @brief The pan law for a MONO source, driven by a speaker layout: pairwise constant power, the
 *        source's position mapped onto the adjacent pair of speakers and split between them by
 *        cosine and sine.
 *
 * Stereo and LCR are two layouts of this one law (openmixer docs/design/specs/
 * 2026-10-14-lane-output-path.md §4). A layout is the speakers on the pan axis, each at its pan
 * position (its angle mapped onto [-1, +1], first at -1, last at +1), plus an optional discrete
 * centre speaker that takes a share of the source near the middle (the centre window) scaled down
 * by the divergence; whatever power the centre does not take goes pairwise over the axis. The
 * gains, one per speaker, go straight into G (omx_mixmatrix.h).
 *
 * For the stereo layout this is the engine's `omx_pan_law` (openmixer
 * packages/pipewire-native/src/mix_dsp.h) bit for bit: theta = (pan + 1)·π/4, left cos, right sin.
 * For the LCR layout it is the engine's `omx_lcr_pan` (mix_lcr.h) bit for bit. A stereo SOURCE is
 * not panned but balanced: omx_balance_law.h.
 *
 * Stateless, pure, header-only. Not in omxdsp.h: include <omxdsp/mix/omx_pan_law.h>.
 */
#ifndef OMX_PAN_LAW_H
#define OMX_PAN_LAW_H

#include <math.h>
#include <stdint.h>

#include <omxdsp/omx_balance_law.h>
#include <omxdsp/omx_contract.h>

/** @brief The most speakers a layout puts on the pan axis. */
#define OMX_PAN_AXIS_MAX 8u

/** @brief The most gains one layout produces: the axis speakers and a discrete centre. */
#define OMX_PAN_SPEAKERS_MAX (OMX_PAN_AXIS_MAX + 1u)

/** @brief A quarter turn, float: the angle one pair of adjacent speakers spans. */
#define OMX_PAN_HALF_PI 1.57079632679489661923f

/**
 * @brief A speaker layout for the pan law.
 *
 * Gains are numbered axis speakers first, left to right (`0 .. n_axis-1`), then the discrete
 * centre when the layout has one (`n_axis`). Stereo is {L, R}; LCR is {L, R, C}, which is the
 * engine's leg order (L = 0, R = 1, C = 2).
 */
struct omx_pan_layout {
  uint32_t n_axis;               /**< Speakers on the pan axis, 2 .. OMX_PAN_AXIS_MAX. */
  float pos[OMX_PAN_AXIS_MAX];   /**< Their pan positions, strictly ascending, -1 first, +1 last. */
  int centre;                    /**< Non-zero: a discrete centre speaker follows the axis. */
  float centre_half_width;       /**< The centre window's half width in pan units, > 0. */
};

/**
 * @brief The stereo layout: two speakers, at -1 and +1, no discrete centre.
 * @param l Out: the layout.
 * @note RT-safe: stores only. Thread-safe: writes only `*l`.
 */
static inline void omx_pan_layout_stereo(struct omx_pan_layout *l) {
  l->n_axis = 2u;
  for (uint32_t i = 0; i < OMX_PAN_AXIS_MAX; i++) l->pos[i] = 0.0f;
  l->pos[0] = -1.0f;
  l->pos[1] = 1.0f;
  l->centre = 0;
  l->centre_half_width = 1.0f;
}

/**
 * @brief The LCR layout: the stereo pair on the axis and a discrete centre speaker.
 * @param l Out: the layout.
 * @param centre_half_width The centre window's half width in pan units: the centre's share is 1
 *        at pan 0 and falls linearly to 0 at |pan| = this (the console's
 *        OMX_CENTRE_WINDOW_HALF_WIDTH).
 * @note RT-safe: stores only. Thread-safe: writes only `*l`.
 */
static inline void omx_pan_layout_lcr(struct omx_pan_layout *l, float centre_half_width) {
  omx_pan_layout_stereo(l);
  l->centre = 1;
  l->centre_half_width = centre_half_width;
}

/**
 * @brief The number of gains a layout produces: its axis speakers, plus one for a centre.
 * @param l The layout.
 * @return `n_axis`, plus 1 when the layout has a discrete centre.
 * @note RT-safe: one compare. Thread-safe: pure.
 */
static inline uint32_t omx_pan_layout_speakers(const struct omx_pan_layout *l) {
  return l->n_axis + (l->centre ? 1u : 0u);
}

/**
 * @brief The discrete centre's window: 1 at pan 0, falling linearly to 0 at |pan| = `half_width`.
 *
 * The engine's `omx_lcr_centre_window`. It reads the pan as given, not clamped (past ±1 the
 * window is already 0 for any half width up to 1).
 * @param pan The pan position.
 * @param half_width The window's half width in pan units, > 0.
 * @return The window, in [0, 1].
 * @note RT-safe: a divide and two compares. Thread-safe: pure.
 */
static inline float omx_pan_centre_window(float pan, float half_width) {
  float w = 1.0f - fabsf(pan) / half_width;
  if (w < 0.0f) w = 0.0f;
  if (w > 1.0f) w = 1.0f;
  return w;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "mix/pan-pair"
/**
 * @brief Pairwise constant power over the layout's axis: one gain per axis speaker.
 *
 * The pan, clamped to [-1, +1], falls between two adjacent axis speakers k and k+1;
 * theta = (pan − pos[k])·(π/2 / (pos[k+1] − pos[k])) gives g[k] = cos(theta), g[k+1] = sin(theta),
 * every other axis gain 0. For two speakers at ±1 the scale is exactly π/4 in float, so this is
 * the engine's (pan + 1)·π/4 bit for bit.
 * @param l The layout.
 * @param pan The pan position; clamped by omx_clamp_pan().
 * @param g Out: `l->n_axis` gains.
 * @pre `layout-axis-in-range`: 2 <= n_axis <= OMX_PAN_AXIS_MAX.
 * @post `legs-not-inverted`: no gain below -1e-6 (at the pair's far end cosf(π/2) is -4.4e-8, a
 *       rounding of zero); `constant-power`: the gains' squares sum to 1 within 1e-5.
 * @note RT-safe: one cosf, one sinf, a walk of at most OMX_PAN_AXIS_MAX positions. Thread-safe: pure.
 */
static inline void omx_pan_pair(const struct omx_pan_layout *l, float pan, float *g) {
  OMX_PRE(l->n_axis >= 2u && l->n_axis <= OMX_PAN_AXIS_MAX, "layout-axis-in-range");
  const float p = omx_clamp_pan(pan);
  uint32_t k = 0;
  while (k + 2u < l->n_axis && p > l->pos[k + 1u]) k++;
  for (uint32_t i = 0; i < l->n_axis; i++) g[i] = 0.0f;
  const float theta = (p - l->pos[k]) * (OMX_PAN_HALF_PI / (l->pos[k + 1u] - l->pos[k]));
  g[k] = cosf(theta);
  g[k + 1u] = sinf(theta);
  OMX_POST(g[k] >= -1e-6f && g[k + 1u] >= -1e-6f, "legs-not-inverted");
  OMX_POST(fabsf(g[k] * g[k] + g[k + 1u] * g[k + 1u] - 1.0f) < 1e-5f, "constant-power");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "mix/pan-gains"
/**
 * @brief The pan law over a layout: one gain per speaker for a mono source at `pan`.
 *
 * Without a centre this is omx_pan_pair(). With one, the centre takes
 * cW = (1 − divergence)·window(pan), and the axis gains are scaled by sqrt(1 − cW²), so the power
 * still sums to 1: divergence 1 is the plain pairwise law, divergence 0 a hard discrete centre at
 * pan 0. This is the engine's `omx_lcr_pan` bit for bit.
 * @param l The layout.
 * @param pan The pan position; the pair clamps it, the centre window reads it as given.
 * @param divergence The centre's divergence, clamped to [0, 1]; ignored without a centre.
 * @param g Out: omx_pan_layout_speakers(l) gains.
 * @pre `layout-axis-in-range` (omx_pan_pair()).
 * @post `constant-power`: the gains' squares sum to 1 within 1e-4.
 * @note RT-safe: one cosf, one sinf, one sqrtf. Thread-safe: pure.
 */
static inline void omx_pan_gains(const struct omx_pan_layout *l, float pan, float divergence,
                                 float *g) {
  omx_pan_pair(l, pan, g);
  if (!l->centre) return;
  float div = divergence;
  if (div < 0.0f) div = 0.0f;
  if (div > 1.0f) div = 1.0f;
  const float cW = (1.0f - div) * omx_pan_centre_window(pan, l->centre_half_width);
  float phantom_sq = 1.0f - cW * cW;
  if (phantom_sq < 0.0f) phantom_sq = 0.0f;
  const float phantom = sqrtf(phantom_sq);
  for (uint32_t i = 0; i < l->n_axis; i++) g[i] = g[i] * phantom;
  g[l->n_axis] = cW;
#ifdef OMX_CONTRACTS
  double power = 0.0;
  for (uint32_t i = 0; i <= l->n_axis; i++) power += (double)g[i] * g[i];
  OMX_POST(fabs(power - 1.0) < 1e-4, "constant-power");
#endif
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The stereo pan law for a mono source: omx_pan_gains() over omx_pan_layout_stereo().
 *
 * pan -1 → (1, 0), 0 → (0.7071, 0.7071) (−3 dB per leg), +1 → (0, 1).
 * @param pan The pan position; clamped by omx_clamp_pan().
 * @param gL Out: the left gain.
 * @param gR Out: the right gain.
 * @note RT-safe: one cosf, one sinf. Thread-safe: pure.
 */
static inline void omx_pan_stereo(float pan, float *gL, float *gR) {
  struct omx_pan_layout l;
  omx_pan_layout_stereo(&l);
  float g[2];
  omx_pan_pair(&l, pan, g);
  *gL = g[0];
  *gR = g[1];
}

#endif /* OMX_PAN_LAW_H */
