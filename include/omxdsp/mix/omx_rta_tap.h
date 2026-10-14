// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_rta_tap.h
 * @brief One analyser tap's reading: the mono sum of its two legs, then one spectrum of the
 *        rate's window and, when the zoom is asked for, three half-band decimations and the low
 *        zoom's spectrum.
 *
 * The entry point over analysis/omx_rta.h (openmixer docs/design/specs/2026-10-14-lane-output-path.md
 * §4). The engine keeps the handover between threads: it reads the tap's rings, passes the legs to
 * omx_rta_tap_mono_sum() into a window it owns, and runs omx_rta_tap_read() on whichever thread it
 * chooses. Both are pure arithmetic over caller-owned buffers: no ring, no atomic, no allocation.
 * Moved from the engine's `omx_rta_tap_snapshot_from` (its sum) and `omx_rta_tap_compute`
 * (openmixer packages/pipewire-native/src/mix_dsp.h), bit for bit.
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_rta_tap.h>.
 */
#ifndef OMX_RTA_TAP_H
#define OMX_RTA_TAP_H

#include <stdbool.h>
#include <stdint.h>

#include <omxdsp/analysis/omx_rta.h>

/**
 * @brief One tap's mono-summed windows, caller-owned. Sized for the rate ceiling's window; a
 *        lower rate uses the front of `block`.
 */
struct omx_rta_tap_window {
  float block[OMX_RTA_FFT_SIZE_MAX];     /**< The latest omx_rta_fft_size_for() samples, mono. */
  float block_low[OMX_RTA_LOW_WINDOW];   /**< The low zoom's latest OMX_RTA_LOW_WINDOW samples, mono. */
};

/**
 * @brief Scratch for one reading, caller-owned. It carries nothing between calls: every field is
 *        written before it is read.
 */
struct omx_rta_tap_work {
  float work_re[OMX_RTA_FFT_SIZE_MAX];      /**< The transform's real part. */
  float work_im[OMX_RTA_FFT_SIZE_MAX];      /**< The transform's imaginary part. */
  float stage1[OMX_RTA_LOW_WINDOW / 2];     /**< After the first ÷2, at sr/2. */
  float stage2[OMX_RTA_LOW_WINDOW / 4];     /**< After the second ÷2, at sr/4. */
  float low_block[OMX_RTA_LOW_WINDOW / 8];  /**< After the third ÷2: OMX_RTA_LOW_FFT_SIZE samples at sr/8. */
};

/**
 * @brief One tap's finished magnitudes: the spectrum every consumer shares, and the display-only
 *        low zoom.
 */
struct omx_rta_tap_reading {
  float mags[OMX_RTA_FFT_SIZE_MAX / 2]; /**< n/2 bins, bin k at k·sr/n Hz, linear. */
  float low[OMX_RTA_LOW_FFT_SIZE / 2];  /**< OMX_RTA_LOW_FFT_SIZE/2 bins at (sr/8)/OMX_RTA_LOW_FFT_SIZE Hz each. */
};

/**
 * @brief The mono sum of a tap's two legs, `out[i] = 0.5·(l[i] + r[i])`.
 *
 * `out` may be `l` (the engine reads the left ring straight into the window and sums the right
 * leg into it). A mono lane passes its one leg twice and reads it unchanged.
 * @param out Out: `n` samples; may alias `l` or `r`.
 * @param l The left leg, `n` samples.
 * @param r The right leg, `n` samples.
 * @param n Samples.
 * @note RT-safe: one pass, no allocation. Thread-safe on disjoint outputs.
 */
static inline void omx_rta_tap_mono_sum(float *out, const float *l, const float *r, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) out[i] = 0.5f * (l[i] + r[i]);
}

/**
 * @brief One tap's reading: the spectrum of `w->block`, and with `low` the low zoom, three
 *        half-band ÷2 stages over `w->block_low` and the spectrum of what is left.
 *
 * `low` false leaves `out->low` untouched and never reads `w->block_low`: the zoom is computed
 * only when somebody displays it.
 * @param w The windows: `block` holds `n` mono samples; with `low`, `block_low` holds
 *        OMX_RTA_LOW_WINDOW.
 * @param scr Scratch.
 * @param out Out: `n/2` bins in `mags`, and with `low`, OMX_RTA_LOW_FFT_SIZE/2 bins in `low`.
 * @param n The window, omx_rta_fft_size_for() of the rate: a power of two, at most
 *        OMX_RTA_FFT_SIZE_MAX.
 * @param low Whether the low zoom is wanted.
 * @note Not for the audio thread: one FFT of `n`, and with `low` a second of
 *       OMX_RTA_LOW_FFT_SIZE plus the decimations. No allocation. Thread-safe on disjoint buffers.
 */
static inline void omx_rta_tap_read(const struct omx_rta_tap_window *w,
                                    struct omx_rta_tap_work *scr,
                                    struct omx_rta_tap_reading *out, uint32_t n, bool low) {
  omx_rta_spectrum(w->block, n, scr->work_re, scr->work_im, out->mags);
  if (!low) return;
  omx_decimate_halfband(w->block_low, OMX_RTA_LOW_WINDOW, scr->stage1);
  omx_decimate_halfband(scr->stage1, OMX_RTA_LOW_WINDOW / 2, scr->stage2);
  omx_decimate_halfband(scr->stage2, OMX_RTA_LOW_WINDOW / 4, scr->low_block);
  omx_rta_spectrum(scr->low_block, OMX_RTA_LOW_FFT_SIZE, scr->work_re, scr->work_im, out->low);
}

#endif /* OMX_RTA_TAP_H */
