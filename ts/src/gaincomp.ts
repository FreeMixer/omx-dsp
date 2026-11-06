// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The gain computer twins: omx_gaincomp_db, omx_gaincomp_db_shifted and omx_gate_knee (omx_gaincomp.h, omx_gate.h),
 * the static characteristic a dynamics curve draws. The kernel is float; each step here is rounded to float by
 * Math.fround as the kernel rounds it (every operation is one IEEE single operation, and a double result rounded
 * once to single is the single result for +, -, *, / and sqrt), so the twin is the kernel's value, bit for bit,
 * where the kernel is built without contraction (-ffp-contract=off, as omxdsp.pc says). Pinned by
 * test/agreement.test.mjs.
 */

const f = Math.fround;

/** The gain computer's mode: act ABOVE the threshold (comp, limiter), BELOW it (gate, expander), or raise the
 *  signal below it (UPWARD). omx_gaincomp.h's OMX_DYN_ABOVE, OMX_DYN_BELOW and OMX_DYN_UPWARD. */
export type GaincompMode = 'above' | 'below' | 'upward';

/** The gain computer's parameters, omx_gaincomp_params: each value is narrowed to float as the kernel stores it. */
export interface GaincompParams {
  readonly mode: GaincompMode;
  /** The threshold, dB. */
  readonly threshDb: number;
  /** The ratio, at least the mode's minimum. */
  readonly ratio: number;
  /** The knee width, dB, not negative; 0 is a hard knee. */
  readonly kneeDb: number;
  /** BELOW: the attenuation floor, dB, at most 0. UPWARD: the largest boost, dB. Not read in ABOVE. */
  readonly rangeDb: number;
}

/**
 * The static gain, dB, at detector level `levelDb`: the twin of omx_gaincomp_db (its per-sample form).
 * @param p The parameters.
 * @param levelDb The detector level, dB; finite.
 */
export function gaincompDb(p: GaincompParams, levelDb: number): number {
  const thresh = f(p.threshDb), ratio = f(p.ratio), knee = f(p.kneeDb), range = f(p.rangeDb);
  const x = f(f(levelDb) - thresh);
  const half = f(0.5 * knee);
  const soft = knee > 0;
  const k2 = soft ? f(2 * knee) : 1;
  const eb = f(x - half);
  const kb = f(f(f(f(1 - ratio) * eb) * eb) / k2);
  let gb = soft && x > -half ? kb : f(f(ratio - 1) * x);
  gb = x >= half ? 0 : gb;
  gb = gb < range ? range : gb;
  const slope = f(f(1 / ratio) - 1);
  const ea = f(x + half);
  const ka = f(f(f(slope * ea) * ea) / k2);
  let ga = soft && x < half ? ka : f(slope * x);
  ga = x <= -half ? 0 : ga;
  const eu = f(half - x);
  const ku = f(f(f(slope * eu) * eu) / k2);
  let gu = soft && -x < half ? ku : f(slope * -x);
  gu = -x <= -half ? 0 : -gu;
  gu = gu > range ? range : gu;
  return p.mode === 'below' ? gb : p.mode === 'upward' ? gu : ga;
}

/** The gate's off-centre knee, omx_gaincomp_knee_shifted: the quadratic Bezier between the expander's line and
 *  0 dB at `x`, for `xl < x < xh`, every value already a float. */
function kneeShifted(ratio: number, xl: number, xh: number, x: number): number {
  const a = f(xl + xh), b = f(-2 * xl), d = f(x - xl);
  const disc0 = f(f(b * b) + f(f(4 * a) * d));
  const disc = disc0 > 0 ? disc0 : 0;
  const den0 = f(b + f(Math.sqrt(disc)));
  const den = den0 > 0 ? den0 : 1;
  const u = f(f(2 * d) / den);
  const w = f(1 - u);
  return f(f(f(w * w) * f(ratio - 1)) * xl);
}

/**
 * The BELOW static gain, dB, with the knee's centre moved `shiftDb` off the threshold: the twin of
 * omx_gaincomp_db_shifted. A shift of 0 is {@link gaincompDb} itself.
 * @param p The parameters, `mode` 'below'.
 * @param shiftDb The knee centre's offset from the threshold, dB, inside half the knee.
 * @param levelDb The detector level, dB; finite.
 */
export function gaincompDbShifted(p: GaincompParams, shiftDb: number, levelDb: number): number {
  const shift = f(shiftDb);
  if (shift === 0) return gaincompDb(p, levelDb);
  const thresh = f(p.threshDb), ratio = f(p.ratio), range = f(p.rangeDb);
  const half = f(0.5 * f(p.kneeDb));
  const x = f(f(levelDb) - thresh);
  const xl = f(shift - half), xh = f(shift + half);
  let g: number;
  if (x <= xl) g = f(f(ratio - 1) * x);
  else if (x >= xh) g = 0;
  else g = kneeShifted(ratio, xl, xh, x);
  return g < range ? range : g;
}

/**
 * The knee range of a gate as the kernel resolves it, omx_gate_knee: the width of the range and its centre's offset
 * from the threshold, held inside half the width; equal ends are the hard knee wherever they sit.
 * @param threshDb The threshold, dB.
 * @param kneeStartDb One end of the range, dB.
 * @param kneeEndDb The other end, dB.
 */
export function gateKnee(threshDb: number, kneeStartDb: number, kneeEndDb: number): { kneeDb: number; shiftDb: number } {
  const s = f(kneeStartDb), e = f(kneeEndDb);
  const lo = s < e ? s : e, hi = s < e ? e : s;
  const width = f(hi - lo);
  const half = f(0.5 * width);
  let shift = width > 0 ? f(f(lo + half) - f(threshDb)) : 0;
  shift = shift < -half ? -half : shift > half ? half : shift;
  return { kneeDb: width, shiftDb: shift };
}
