// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_rta_tap.h>

/* ---- one analyser tap's reading (mix/omx_rta_tap.h, FreeMixer/omx-dsp#98). At each of the nine
 * oracle rates, at the rate's own window: a bin-aligned 0.5 sine on both legs reads 0.5 in its bin;
 * the same sine against its own inverse on the other leg sums to silence and reads 0 in every bin;
 * a 0.5 sine bin-aligned on the low zoom's grid reads 0.5 in its low bin after the three ÷2
 * stages; asking for the zoom never moves the main spectrum, and not asking for it leaves the low
 * bins untouched. ------------------------------------------------------------------------------ */

static const float MIX_RTA_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

static void arm_mix_rta_tap(void) {
  g_arm = "mix_rta_tap";
  static float l[OMX_RTA_LOW_WINDOW], r[OMX_RTA_LOW_WINDOW], keep[OMX_RTA_FFT_SIZE_MAX / 2];
  static struct omx_rta_tap_window w;
  static struct omx_rta_tap_work scr;
  static struct omx_rta_tap_reading out;
  const uint32_t lw = OMX_RTA_LOW_WINDOW, lowk = 64u;
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const uint32_t n = omx_rta_fft_size_for((uint32_t)MIX_RTA_RATES[ri]);
    const uint32_t k0 = n / 8u + 3u;

    /* a bin-aligned sine on both legs */
    for (uint32_t i = 0; i < n; i++) l[i] = r[i] = 0.5f * (float)sin(2.0 * M_PI * (double)k0 * (double)i / (double)n);
    omx_rta_tap_mono_sum(w.block, l, r, n);
    for (uint32_t k = 0; k < OMX_RTA_LOW_FFT_SIZE / 2; k++) out.low[k] = -1.0f;
    omx_rta_tap_read(&w, &scr, &out, n, false);
    ok(fabsf(out.mags[k0] - 0.5f) < 1e-4f, "a bin-aligned 0.5 sine reads 0.5 in its bin", out.mags[k0], 0.5);
    uint32_t touched = 0;
    for (uint32_t k = 0; k < OMX_RTA_LOW_FFT_SIZE / 2; k++) if (out.low[k] != -1.0f) touched++;
    ok(touched == 0, "without the zoom the low bins are left untouched", touched, 0);
    memcpy(keep, out.mags, (n / 2u) * sizeof(float));

    /* the zoom: a sine bin-aligned on the low grid, (sr/8)/8192 per bin */
    for (uint32_t i = 0; i < lw; i++) l[i] = r[i] = 0.5f * (float)sin(2.0 * M_PI * (double)lowk * (double)i / (double)lw);
    omx_rta_tap_mono_sum(w.block_low, l, r, lw);
    omx_rta_tap_read(&w, &scr, &out, n, true);
    ok(memcmp(keep, out.mags, (n / 2u) * sizeof(float)) == 0, "asking for the zoom never moves the main spectrum", 0, 0);
    ok(fabsf(out.low[lowk] - 0.5f) < 1e-3f, "a 0.5 sine on the low grid reads 0.5 in its low bin", out.low[lowk], 0.5);
    float far = 0.0f;
    for (uint32_t k = 0; k < OMX_RTA_LOW_FFT_SIZE / 2; k++)
      if ((k + 2u < lowk || k > lowk + 2u) && out.low[k] > far) far = out.low[k];
    ok(far < 1e-3f, "and nothing outside the sine's bin and its window's neighbours", far, 1e-3);

    /* the inverse on the other leg sums to silence */
    for (uint32_t i = 0; i < n; i++) {
      l[i] = 0.5f * (float)sin(2.0 * M_PI * (double)k0 * (double)i / (double)n);
      r[i] = -l[i];
    }
    omx_rta_tap_mono_sum(w.block, l, r, n);
    omx_rta_tap_read(&w, &scr, &out, n, false);
    float loud = 0.0f;
    for (uint32_t k = 0; k < n / 2u; k++) if (out.mags[k] > loud) loud = out.mags[k];
    ok(loud == 0.0f, "a leg against its own inverse sums to silence", loud, 0.0);

    /* in place, as the engine sums: the window already holds the left leg */
    for (uint32_t i = 0; i < n; i++) w.block[i] = l[i];
    omx_rta_tap_mono_sum(w.block, w.block, l, n);
    uint32_t moved = 0;
    for (uint32_t i = 0; i < n; i++) if (w.block[i] != l[i]) moved++;
    ok(moved == 0, "a leg summed with itself in place is the leg", moved, 0);
  }
  expect_clean();
}
