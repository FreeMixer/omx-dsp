// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * Every twin's agreement tolerance, in ONE place, each with its reason (moved here with the tests). The ruling puts these in omx-contract as data; until omx-contract carries a tolerance vocabulary
 * they live here (a recorded debt of the move).
 */
/** Float32 rounds a number to a relative 2^-24 = 6e-8 at worst. */
export const F32 = 6e-8;
export const TOLERANCES = {
  // The kernel is float: the input is narrowed (6e-8 relative = 5.2e-7 dB) and log10f is rounded
  // (6e-8 relative of up to 180 dB = 1.1e-5 dB). 1e-6 dB absolute + 6e-8 relative covers both.
  linearGainToDb: { abs: 1e-6, rel: F32 },
  // Both are 20 log10(max(x, floor)) in double. The gap is libm log10 against V8's, 1e-13 dB, plus the
  // floor taken back through 10^(f/20) and 20 log10 (a round trip): 1e-12 dB absolute.
  linearToDbFloored: { abs: 1e-12 },
  // powf of a float argument: db x 0.05f rounded to float (up to 8.3e-7 relative in the result for
  // |db| <= 120) and powf within an ulp: 1e-6 relative.
  dbToLinearGain: { rel: 1e-6 },
  // sqrt(t)/(t-1) with t = 2^bw in double on both sides; libm pow against V8's is a few ulp.
  octavesToQ: { rel: 1e-12 },
  // The answer is a power of two.
  fftSizeForRate: { abs: 0 },
};
