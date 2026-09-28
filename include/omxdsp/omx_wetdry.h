// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_wetdry.h
 * @brief The loop stages' per-sample tail: each leg's feedback word flushed and its convex
 *        wet/dry mixed, `fb ← flush(w); y = (1 − mix)·x + mix·w`.
 *
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 row 22.
 */
#ifndef OMX_WETDRY_H
#define OMX_WETDRY_H

#include "omx_denormal.h"

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
 * @note RT-safe: four multiplies, no call beyond the flush. Thread-safe on distinct state.
 */
static inline void omx_wetdry_loop(float *l, float *r, float *fl, float *fr, float xl, float xr,
                                   float wl, float wr, float dry, float mix) {
  *fl = omx_flush(wl);
  *fr = omx_flush(wr);
  *l = dry * xl + mix * wl;
  *r = dry * xr + mix * wr;
}

#endif /* OMX_WETDRY_H */
