// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_wetdry.h
 * @brief The convex wet/dry, `y = (1 − mix)·x + mix·w`, and the loop stages' per-sample tail
 *        that flushes each leg's feedback word before it mixes.
 *
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 rows 22 and 22a.
 */
#ifndef OMX_WETDRY_H
#define OMX_WETDRY_H

#include "omx_contract.h"
#include "omx_denormal.h"

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "wetdry/mix"

/**
 * @brief One stereo frame of the convex wet/dry: each leg `y = dry·x + mix·w`.
 * @param l Receives the left output sample.
 * @param r Receives the right output sample.
 * @param xl The left dry sample.
 * @param xr The right dry sample.
 * @param wl The left wet sample.
 * @param wr The right wet sample.
 * @param dry `1 − mix`, as the caller resolved it.
 * @param mix The wet share, in [0, 1].
 * @pre `mix-in-unit-range`, `dry-is-the-complement`.
 * @post `finite-out`.
 * @note RT-safe: four multiplies, no state. Reentrant.
 */
static inline void omx_wetdry_mix(float *l, float *r, float xl, float xr, float wl, float wr,
                                  float dry, float mix) {
  OMX_PRE(mix >= 0.0f && mix <= 1.0f, "mix-in-unit-range");
  OMX_PRE(dry == 1.0f - mix, "dry-is-the-complement");
  *l = dry * xl + mix * wl;
  *r = dry * xr + mix * wr;
  OMX_POST(*l - *l == 0.0f && *r - *r == 0.0f, "finite-out");
}

#undef OMX_CONTRACT_STAGE

/**
 * @brief One stereo frame: hold each leg's wet sample as its flushed loop word, write the mix.
 * @param l Receives the left output sample.
 * @param r Receives the right output sample.
 * @param fl The left loop word, replaced by `omx_flush(wl)`.
 * @param fr The right loop word, replaced by `omx_flush(wr)`.
 * @param xl The left dry sample.
 * @param xr The right dry sample.
 * @param wl The left wet sample.
 * @param wr The right wet sample.
 * @param dry `1 − mix`.
 * @param mix The wet share, in [0, 1].
 * @note RT-safe: two flushes and omx_wetdry_mix(). Thread-safe on distinct state.
 */
static inline void omx_wetdry_loop(float *l, float *r, float *fl, float *fr, float xl, float xr,
                                   float wl, float wr, float dry, float mix) {
  *fl = omx_flush(wl);
  *fr = omx_flush(wr);
  omx_wetdry_mix(l, r, xl, xr, wl, wr, dry, mix);
}

#endif /* OMX_WETDRY_H */
