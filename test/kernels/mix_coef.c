// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_mix_coef.h>

/* ---- the coefficients of G (mix/omx_mix_coef.h, FreeMixer/omx-dsp#98). A mono strip's gains are
 * its level times the pan law, bit for bit; a stereo strip's are its level times the linear
 * balance, which never boosts and is unity per leg at centre; an unassigned or muted strip is 0.
 * The fold's gain is level × coefficient, 0 when not finite. At each of the nine RME rates a
 * one-second 997 Hz tone goes through omx_mixmatrix_sparse() over the entries the fillers write:
 * a mono strip lands on each speaker as gain × x exactly, and a correlated stereo source folded at
 * 0.5 lands on the mono output at unity, bit for bit. The silent-source test reads the block
 * peak against the floor. ------------------------------------------------------------------- */

static const float MIX_COEF_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;

static void arm_mix_coef(void) {
  g_arm = "mix_coef";
  struct omx_pan_layout st, lcr;
  omx_pan_layout_stereo(&st);
  omx_pan_layout_lcr(&lcr, 0.5f);

  ok(omx_mix_strip_level(0.5f, 0.25f, 0) == 0.125f, "level = gain × fader", omx_mix_strip_level(0.5f, 0.25f, 0), 0.125);
  ok(omx_mix_strip_level(0.5f, 0.25f, 1) == 0.0f, "a muted strip's level is 0", omx_mix_strip_level(0.5f, 0.25f, 1), 0.0);

  float g[OMX_PAN_SPEAKERS_MAX];
  omx_mix_strip_gains(&st, 1, 0, 1.0f, 1.0f, 0, 0.3f, 0.0f, g);
  ok(g[0] == 0.0f && g[1] == 0.0f, "an unassigned strip puts nothing on the bus", g[0], 0.0);
  omx_mix_strip_gains(&lcr, 2, 1, 1.0f, 0.8f, 0, 0.0f, 0.0f, g);
  ok(g[0] == 0.8f && g[1] == 0.8f && g[2] == 0.0f, "a stereo strip at centre: unity per leg, nothing in C", g[0], 0.8);

  ok(omx_mix_fold_gain(0.8f, 0.5f) == 0.4f, "fold gain = level × coefficient", omx_mix_fold_gain(0.8f, 0.5f), 0.4);
  ok(omx_mix_fold_gain(NAN, 0.5f) == 0.0f, "a NaN fold gain reads as 0", omx_mix_fold_gain(NAN, 0.5f), 0.0);
  ok(omx_mix_fold_gain(INFINITY, 0.5f) == 0.0f, "an infinite fold gain reads as 0", omx_mix_fold_gain(INFINITY, 0.5f), 0.0);

  struct omx_mixmatrix_entry e[OMX_PAN_SPEAKERS_MAX];
  const uint32_t rows[3] = {0, 0, 0}, outs[3] = {0, 1, 2};
  const float zero[3] = {0, 0, 0}, half[3] = {0.5f, 0.0f, 0.0f}, falling[3] = {0.0f, 0.25f, 0.0f};
  ok(omx_mix_fill_entries(e, rows, outs, zero, zero, 3) == 0u, "settled zeros write no entry", 0, 0);
  ok(omx_mix_fill_entries(e, rows, outs, falling, half, 3) == 2u, "an entry still ramping to 0 stays in", 2, 2);
  ok(omx_mix_fill_fold(e, 0, 1, 0, 0.0f, 0.0f) == 0u, "a settled-silent fold writes no entry", 0, 0);

  for (uint32_t ri = 0; ri < OMX_RME_RATES_COUNT; ri++) {
    const float sr = MIX_COEF_RATES[ri];
    const uint32_t n = (uint32_t)sr;
    static float x[192000], y[3][192000];
    for (uint32_t i = 0; i < n; i++) x[i] = 0.9f * sinf((float)(2.0 * M_PI * 997.0 * (double)i / sr));
    const float *in[2] = {x, x};
    float *out[3] = {y[0], y[1], y[2]};

    uint32_t mono_bits = 0, boost = 0, centre_unity = 0;
    for (uint32_t s = 0; s < 63; s++) {
      const float pan = -1.0f + (float)s / 31.0f;
      /* a mono strip: level × the pan law, then the matrix lands gain × x on each speaker */
      float p[OMX_PAN_SPEAKERS_MAX], want[OMX_PAN_SPEAKERS_MAX];
      omx_pan_gains(&lcr, pan, 0.25f, p);
      const float eff = omx_mix_strip_level(0.9f, 0.7f, 0);
      omx_mix_strip_gains(&lcr, 1, 1, 0.9f, 0.7f, 0, pan, 0.25f, g);
      for (uint32_t k = 0; k < 3; k++) {
        want[k] = eff * p[k];
        if (g[k] != want[k]) mono_bits++;
      }
      uint32_t r3[3], o3[3] = {0, 1, 2};
      omx_mix_strip_rows(&lcr, 1, 0, 1, r3);
      const uint32_t ne = omx_mix_fill_entries(e, r3, o3, g, g, 3);
      omx_mixmatrix_sparse(out, in, e, ne, 2, 3, n);
      for (uint32_t k = 0; k < 3; k++)
        for (uint32_t i = 0; i < n; i += 97)
          if (y[k][i] != g[k] * x[i]) mono_bits++;
      /* a stereo strip: the balance never boosts */
      omx_mix_strip_gains(&st, 2, 1, 1.0f, 0.7f, 0, pan, 0.0f, g);
      if (g[0] > 0.7f || g[1] > 0.7f || g[0] < 0.0f || g[1] < 0.0f) boost++;
      if (pan == 0.0f && (g[0] != 0.7f || g[1] != 0.7f)) centre_unity++;
    }
    ok(mono_bits == 0, "a mono strip lands as level × pan × x, bit for bit", mono_bits, 0);
    ok(boost == 0, "the linear balance never boosts at this rate", boost, 0);
    ok(centre_unity == 0, "a stereo strip at centre is unity per leg", centre_unity, 0);

    /* a correlated stereo source folded at 0.5 lands at unity */
    const float t = omx_mix_fold_gain(1.0f, 0.5f);
    const uint32_t nf = omx_mix_fill_fold(e, 0, 1, 0, t, t);
    omx_mixmatrix_sparse(out, in, e, nf, 2, 1, n);
    uint32_t fold_bits = 0;
    for (uint32_t i = 0; i < n; i++)
      if (y[0][i] != x[i]) fold_bits++;
    ok(nf == 2u && fold_bits == 0, "a correlated source folded at 0.5 lands at unity, bit for bit", fold_bits, 0);

    /* the silent-source test: the tone is not silent under its own peak, it is under a higher floor */
    ok(!omx_mix_source_silent(x, x, n, 0.5f), "a 0.9 tone is not silent at a 0.5 floor", 0, 0);
    ok(omx_mix_source_silent(x, NULL, n, 0.95f), "a 0.9 tone is silent at a 0.95 floor", 0, 0);
    ok(!omx_mix_source_silent(x, x, n, 0.0f), "a floor of 0 makes nothing silent", 0, 0);
  }
  expect_clean();
}
