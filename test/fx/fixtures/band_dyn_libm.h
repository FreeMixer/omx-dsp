// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * band_dyn_libm.h — the band-dynamics loop as it ran on the libm pair (omx_lin_to_db() and
 * omx_db_to_lin(), one log10f and one powf per sample), frozen verbatim from omx_band_dyn.h at
 * omx-dsp 0.20 with its words renamed `ref_*`. test/fx/band_dyn_poly.test.c holds the library's
 * libm-free loop to it within the declared bound. Never edit the arithmetic: this file is the
 * reference, not a second implementation.
 */
#ifndef OMXDSP_TEST_FX_BAND_DYN_LIBM_H
#define OMXDSP_TEST_FX_BAND_DYN_LIBM_H

#include <math.h>
#include <stdint.h>

#include <omxdsp/omx_band_dyn.h>
#include <omxdsp/omx_units.h>

static inline float ref_band_dyn_offset_db(const struct omx_gaincomp_params *gc, float level) {
  struct omx_gaincomp_params c = *gc;
  const float span = -fabsf(gc->range_db);
  c.range_db = span;
  float gr = omx_gaincomp_db(&c, omx_lin_to_db(level));
  gr = gr < span ? span : gr;
  gr = gr > 0.0f ? 0.0f : gr;
  const float d = gc->range_db > 0.0f ? -gr : gr;
  return d;
}

static inline float ref_band_dyn_detect(const float c[5], struct omx_band_dyn_state *st,
                                        const struct omx_gaincomp_params *gc,
                                        const struct omx_band_dyn_block *bk, float xl, float xr,
                                        int stereo, float *bl, float *br) {
  *bl = omx_biquad(xl, c, st->b_l);
  *br = stereo ? omx_biquad(xr, c, st->b_r) : 0.0f;
  const float k = stereo ? 0.5f * (fabsf(*bl) + fabsf(*br)) : fabsf(*bl);
  const float level = omx_env_step(&st->env, &bk->ep, k, bk->ac, bk->rc);
  const float g = omx_db_to_lin(ref_band_dyn_offset_db(gc, level));
  return g;
}

/* The loop, with the per-sample detector level, `g` and `B` written out so the oracle can state
 * the bound per sample: `lv_out[i]`, `g_out[i]`, `bl_out[i]`, `br_out[i]` (each may be NULL). The
 * level is read back from the cascade after the step, which is the value the step returned. */
static inline void ref_band_dyn_run(float *l, float *r, uint32_t n, const struct omx_dyn *dyn, const float c[5],
                                    struct omx_band_dyn_state *st, int wideband, int enabled, float *lv_out,
                                    float *g_out, float *bl_out, float *br_out) {
  if (!enabled || n == 0u) return;
  const struct omx_band_dyn_block bk = omx_band_dyn_begin(dyn);
  const int stereo = r != NULL;
  float gmin = 1.0f;
  for (uint32_t i = 0; i < n; i++) {
    float bl, br;
    const float g = ref_band_dyn_detect(c, st, &dyn->gc, &bk, l[i], stereo ? r[i] : 0.0f, stereo, &bl, &br);
    if (lv_out) lv_out[i] = omx_env_level(&st->env, &bk.ep);
    if (g_out) g_out[i] = g;
    if (bl_out) bl_out[i] = bl;
    if (br_out) br_out[i] = br;
    gmin = g < gmin ? g : gmin;
    l[i] = wideband ? l[i] * g : omx_band_dyn_split(l[i], g, bl);
    if (stereo) r[i] = wideband ? r[i] * g : omx_band_dyn_split(r[i], g, br);
  }
  st->gr_min_db = omx_gr_block_db(gmin);
  omx_band_dyn_flush(st);
}

#endif /* OMXDSP_TEST_FX_BAND_DYN_LIBM_H */
