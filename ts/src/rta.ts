// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/** The twin of omx_rta.h omx_rta_fft_size_for, moved verbatim from the console (its spectrum bands); the two limits are omx-dsp's own (data/kernels/rta.json). */

import { RTA_TARGET_BIN_HZ, RTA_FFT_SIZE_MAX } from './definitions.js';

/**
 * The RTA FFT length for `rate` — the TypeScript mirror of `mix_dsp.h`'s `omx_rta_fft_size_for`:
 * the smallest power of two whose bins are at or under {@link RTA_TARGET_BIN_HZ} (44.1/48 kHz →
 * 8192, 88.2/96 kHz → 16384, 176.4/192 kHz → 32768). Window TIME stays ~171 ms at every rate —
 * pinning the length instead of deriving it was how moving the rig to 96 kHz silently halved the
 * resolution (2026-09-17 declare-derive audit, F2). Every consumer of an offline/synth or a
 * server RTA frame derives its window from the graph rate through this function rather than
 * declaring one of its own.
 */
export function fftSizeForRate(rate: number): number {
  let n = 2048;
  while (n < RTA_FFT_SIZE_MAX && rate / n > RTA_TARGET_BIN_HZ) n *= 2;
  return n;
}
