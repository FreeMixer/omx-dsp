// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * Every twin's agreement tolerance, in ONE place, each with its reason (moved here with the tests). The decision puts these in omx-contract as data; until omx-contract carries a tolerance vocabulary
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
  // The same closed forms in double, -ffp-contract=off on the C side; libm against V8 is a few ulp, amplified
  // where p1 -> -2, p2 -> 1 (a low centre at a high rate). The worst gap is 2e-13; the bound is 500 times that and
  // 400 times under the float32 half-ulp the engine narrows every coefficient to.
  rbjSection: { abs: 1e-10, rel: 1e-10 },
  // The kernel clamps a float: its input is narrowed first, at most 2^-24 relative.
  clampEqQFor: { rel: F32 },
  // dB of a magnitude quotient from the same design in double; the worst gap is 1.8e-10 dB (a hair off a notch's
  // null, where the numerator cancels). 1e-8 dB is 55 times that and a millionth of a 0.01 dB meter step.
  biquadMagnitudeDb: { abs: 1e-8 },
  // A sum of up to four of those: 1e-8 dB holds with 13 times to spare.
  magnitudeSumDb: { abs: 1e-8 },
  // The gain computer is float arithmetic and one sqrtf, -ffp-contract=off; the twin rounds each step with
  // Math.fround, and one rounding of the double result is the float result of +, -, *, / and sqrt: bit for bit.
  gaincompDb: { abs: 0 },
  gaincompDbShifted: { abs: 0 },
  gateKnee: { abs: 0 },
  // A transfer curve is x + that gain (+ make-up) in double on both sides: bit for bit.
  curvePointsTransfer: { abs: 0 },
  // An EQ curve is magnitudeSumDb over up to ten sections at x from Math.pow against libm pow (a few ulp of x):
  // the worst gap measured is 2.7e-10 dB; 1e-8 dB is the sum tolerance above, and a Q moved by less than the
  // engine's float narrowing (2^-25 relative) moves the curve by more.
  curvePointsEq: { abs: 1e-8 },
  // A gain given as a linear factor goes through omx_lin_to_db, which is log10f: V8's log10 rounded to float differs
  // from it by up to one float step of the dB value (1e-6 dB at 12 dB); measured 1.3e-6 dB at worst.
  curvePointsEqCoef: { abs: 4e-6 },
  // The meter view law is double arithmetic on both sides in the same order: a fill from dB, the marker, the ladder,
  // the clip word and the decimation are bit for bit. A fill from a linear peak goes through log10, libm against V8's:
  // a few ulp of a value at most 1.
  meterView: { abs: 0 },
  meterFill: { abs: 1e-15 },
  // The gain-reduction reading is float arithmetic: powf and log10f against V8's pow and log10 rounded to float differ by
  // a float step of the dB value, and the recovery carries it from read to read; measured below 2e-6 dB, and a hold one
  // read off is a whole dB.
  meterGr: { abs: 1e-4 },
};
