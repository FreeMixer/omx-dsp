// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The EQ twins: the Q relation and clamp, the section design and the magnitude a surface draws. Each is the
 * TypeScript form of an omx_eq_design.h / omx_magnitude.h / omx_eq_instance.h kernel, pinned to it by
 * test/agreement.test.mjs. A browser draws the curve with these; the engine runs the C.
 */
import { EQ_NOTCH_Q_RANGE, EQ_Q_RANGE, LOWPASS_HANDOVER } from './definitions.js';
import { linearToDbFloored } from './units.js';

const clamp = (x: number, lo: number, hi: number): number => (x < lo ? lo : x > hi ? hi : x);

/** Bandwidth in octaves → Q (the cookbook relation). The twin of omx_eq_octaves_q. */
export function octavesToQ(octaves: number): number {
  const bw = Math.max(octaves, 0.05);
  const t = Math.pow(2, bw);
  return Math.sqrt(t) / (t - 1);
}

/**
 * A band's Q clamped into the extent its shape allows: a notch may go ultra-narrow ({@link EQ_NOTCH_Q_RANGE}),
 * every other shape keeps {@link EQ_Q_RANGE}. The twin of omx_eq_lv2_clamp_q, which clamps a float.
 * @param type The band shape; only `'notch'` takes the notch range.
 * @param q The asked Q.
 */
export function clampEqQFor(type: string, q: number): number {
  const r = type === 'notch' ? EQ_NOTCH_Q_RANGE : EQ_Q_RANGE;
  return clamp(q, r.min, r.max);
}

/** The section kinds of omx_eq_design, by the twin's word (omx_eq_kind 0..8 in this order). */
export type BiquadKind =
  | 'peaking'
  | 'lowShelf'
  | 'highShelf'
  | 'highpass'
  | 'lowpass'
  | 'notch'
  | 'bandpass'
  | 'allpass1'
  | 'allpass2';

/** One designed section, `a0` carried explicitly (every design here normalises, so it is 1). */
export interface BiquadSection {
  readonly b0: number;
  readonly b1: number;
  readonly b2: number;
  readonly a0: number;
  readonly a1: number;
  readonly a2: number;
}

/** How far a peaked low-pass hands its Nyquist target over: 0 at or below qHi, 1 at or above qPeak. */
function lowpassNyquistHandover(q: number): number {
  const v = (q - LOWPASS_HANDOVER.qHi) / (LOWPASS_HANDOVER.qPeak - LOWPASS_HANDOVER.qHi);
  return v <= 0 ? 0 : v >= 1 ? 1 : v;
}

/** How far a low-Q low-pass raises its Nyquist target across the hand-over: 0 at or above qHi, 1 at or below qLo. */
function lowpassNyquistRaise(q: number): number {
  const u = (LOWPASS_HANDOVER.qHi - q) / (LOWPASS_HANDOVER.qHi - LOWPASS_HANDOVER.qLo);
  return u <= 0 ? 0 : u >= 1 ? 1 : u;
}

/**
 * The low-pass hand-over band for a Q, as fractions of Nyquist: the matched poles below the first, the bilinear
 * pair from the second. The twin of omx_eq_lowpass_handover; the table is {@link LOWPASS_HANDOVER}.
 */
export function lowpassHandover(q: number): readonly [number, number] {
  const h = LOWPASS_HANDOVER;
  let u = (h.qHi - q) / (h.qHi - h.qLo);
  u = u < 0 ? 0 : u > 1 ? 1 : u;
  const lo = h.loAtHiQ + (h.loAtLoQ - h.loAtHiQ) * u;
  let hi = h.hiAtHiQ + (h.hiAtLoQ - h.hiAtHiQ) * u;
  hi += (h.hiAtPeakQ - h.hiAtHiQ) * lowpassNyquistHandover(q);
  return [lo, hi];
}

/**
 * One conjugate pair placed by the matched-Z map `z = exp(sT)`: the digital pair `1 + p1 z^-1 + p2 z^-2` of the
 * analogue `s^2 + 2 zeta wn s + wn^2`, and `dc`, that polynomial at `z = 1` in a factored form that does not
 * cancel for a low section. `wn` (radians per sample) is held at or below pi.
 * @param wn The pair's centre, `2 pi f / fs`.
 * @param zeta The damping, `1/(2Q)` for a plain resonator.
 */
export function matchedPair(wn: number, zeta: number): { p1: number; p2: number; dc: number } {
  const w = wn > Math.PI ? Math.PI : wn;
  const e = Math.exp(-zeta * w);
  if (zeta < 1) {
    const th = Math.sqrt(1 - zeta * zeta) * w;
    const ec = e * Math.cos(th);
    const es = e * Math.sin(th);
    return { p1: -2 * ec, p2: e * e, dc: (1 - ec) * (1 - ec) + es * es };
  }
  // real roots; zeta - sqrt(zeta^2-1) written as 1/(zeta + sqrt(zeta^2-1)), which keeps its digits as Q -> 0
  const sq = Math.sqrt(zeta * zeta - 1);
  const z1 = Math.exp(-w / (zeta + sq));
  const z2 = Math.exp(-w * (zeta + sq));
  return { p1: -(z1 + z2), p2: z1 * z2, dc: (1 - z1) * (1 - z2) };
}

/**
 * One section as omx_eq_design designs it: matched-Z peaking and shelves, a matched low-pass with its fitted
 * numerator and Nyquist hand-over, a matched notch under the cookbook numerator, the cookbook high-pass and
 * bandpass, and the first- and second-order allpasses. The centre is clamped to `[1, 0.999 Nyquist]` and a Q at
 * or under 0 is taken as 1e-3; `gainDb` is read by the peaking and shelf kinds only.
 * @param kind The section kind.
 * @param freqHz Centre or corner, Hz.
 * @param q Quality factor.
 * @param gainDb Gain, dB.
 * @param sampleRate Rate the section runs at, Hz.
 */
export function rbjSection(kind: BiquadKind, freqHz: number, q: number, gainDb: number, sampleRate: number): BiquadSection {
  const nyquist = sampleRate * 0.5;
  const f0 = clamp(freqHz, 1, nyquist * 0.999);
  const qq = q <= 0 ? 1e-3 : q;
  const w0 = (2 * Math.PI * f0) / sampleRate;
  const A = Math.pow(10, gainDb / 40);
  switch (kind) {
    case 'peaking': {
      const p = matchedPair(w0, 1 / (2 * A * qq));
      const z = matchedPair(w0, A / (2 * qq));
      const k = p.dc / z.dc;
      return { b0: k, b1: k * z.p1, b2: k * z.p2, a0: 1, a1: p.p1, a2: p.p2 };
    }
    case 'lowShelf': {
      const sa = Math.sqrt(A);
      const p = matchedPair(w0 / sa, 1 / (2 * qq));
      const z = matchedPair(w0 * sa, 1 / (2 * qq));
      const k = (A * A * p.dc) / z.dc;
      return { b0: k, b1: k * z.p1, b2: k * z.p2, a0: 1, a1: p.p1, a2: p.p2 };
    }
    case 'highShelf': {
      const sa = Math.sqrt(A);
      const p = matchedPair(w0 * sa, 1 / (2 * qq));
      const z = matchedPair(w0 / sa, 1 / (2 * qq));
      const k = p.dc / z.dc;
      return { b0: k, b1: k * z.p1, b2: k * z.p2, a0: 1, a1: p.p1, a2: p.p2 };
    }
    case 'highpass': {
      const cw = Math.cos(w0);
      const alpha = Math.sin(w0) / (2 * qq);
      const a0 = 1 + alpha;
      return { b0: (1 + cw) / 2 / a0, b1: -(1 + cw) / a0, b2: (1 + cw) / 2 / a0, a0: 1, a1: (-2 * cw) / a0, a2: (1 - alpha) / a0 };
    }
    case 'lowpass': {
      const m = matchedPair(w0, 1 / (2 * qq));
      const band = lowpassHandover(qq);
      const r = f0 / nyquist;
      const x = (r - band[0]) / (band[1] - band[0]);
      const t = x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x);
      const h = 2 * x - 1;
      const v = h <= 0 ? 0 : h >= 1 ? 1 : h * h * (3 - 2 * h);
      const p = { p1: m.p1, p2: m.p2, dc: m.dc };
      if (t > 0) {
        const cwh = Math.cos(w0);
        const alpha = Math.sin(w0) / (2 * qq);
        const a0 = 1 + alpha;
        p.p1 = m.p1 + ((-2 * cwh) / a0 - m.p1) * t;
        p.p2 = m.p2 + ((1 - alpha) / a0 - m.p2) * t;
        p.dc = 1 + p.p1 + p.p2;
      }
      const keep = (1 - t * lowpassNyquistHandover(qq)) * (1 + v * lowpassNyquistRaise(qq));
      if (keep === 0) {
        const g = (1 + p.p1 + p.p2) / 4;
        return { b0: g, b1: 2 * g, b2: g, a0: 1, a1: p.p1, a2: p.p2 };
      }
      // the prototype's |H|^2 at DC (1), at f0 (Q^2) and at Nyquist, and |D|^2 at the same three points
      const xn = nyquist / f0;
      const un = 1 - xn * xn;
      const tPi = 1 / (un * un + (xn / qq) * (xn / qq));
      const dPi = 1 - p.p1 + p.p2;
      const cw = Math.cos(w0);
      const dr = 1 + p.p1 * cw + p.p2 * Math.cos(2 * w0);
      const di = -(p.p1 * Math.sin(w0) + p.p2 * Math.sin(2 * w0));
      // |N|^2 = U - 4 V s + 16 W s^2 with s = sin^2(w/2), solved from the three points
      const s0 = Math.sin(w0 / 2) * Math.sin(w0 / 2);
      const U = p.dc * p.dc;
      const r1 = tPi * dPi * dPi * keep - U;
      const r2 = qq * qq * (dr * dr + di * di) - U;
      const W = (r2 - s0 * r1) / (16 * s0 * (s0 - 1));
      const n1 = Math.sqrt(U);
      const nm = Math.sqrt(U + r1 > 0 ? U + r1 : 0);
      const sum = (n1 + nm) / 2;
      const b1 = (n1 - nm) / 2;
      const disc = sum * sum - 4 * W;
      const b0 = (sum + Math.sqrt(disc > 0 ? disc : 0)) / 2;
      return { b0, b1, b2: sum - b0, a0: 1, a1: p.p1, a2: p.p2 };
    }
    case 'notch': {
      const b1 = -2 * Math.cos(w0);
      const p = matchedPair(w0, 1 / (2 * qq));
      const sw2 = Math.sin(w0 / 2);
      const k = p.dc / (4 * sw2 * sw2); // 4 sin^2(w0/2) is 2 + b1 without the cancellation
      return { b0: k, b1: k * b1, b2: k, a0: 1, a1: p.p1, a2: p.p2 };
    }
    case 'bandpass': {
      const cw = Math.cos(w0);
      const alpha = Math.sin(w0) / (2 * qq);
      const a0 = 1 + alpha;
      return { b0: alpha / a0, b1: 0, b2: -alpha / a0, a0: 1, a1: (-2 * cw) / a0, a2: (1 - alpha) / a0 };
    }
    case 'allpass1': {
      const t = Math.sin(w0) / (1 + Math.cos(w0));
      const k = (t - 1) / (t + 1);
      return { b0: k, b1: 1, b2: 0, a0: 1, a1: k, a2: 0 };
    }
    case 'allpass2': {
      const K = Math.tan((Math.PI * f0) / sampleRate);
      const norm = 1 / (1 + K / qq + K * K);
      const a1 = 2 * (K * K - 1) * norm;
      const a2 = (1 - K / qq + K * K) * norm;
      return { b0: a2, b1: a1, b2: 1, a0: 1, a1, a2 };
    }
  }
}

/**
 * The magnitude of one designed section at `freq`, dB, by the unit-circle arithmetic itself. A point at or above
 * Nyquist, or a degenerate denominator, is 0 dB; a full null floors at `floorDb`.
 * @param section The section.
 * @param freq Probe frequency, Hz.
 * @param fs Sample rate, Hz.
 * @param floorDb The floor of a null, dB.
 */
export function sectionMagnitudeDb(section: BiquadSection, freq: number, fs: number, floorDb: number): number {
  if (!(freq > 0) || !(fs > 0) || freq >= fs / 2) return 0;
  const { b0, b1, b2, a0, a1, a2 } = section;
  const w = (2 * Math.PI * freq) / fs;
  const cw = Math.cos(w);
  const sw = Math.sin(w);
  const cw2 = Math.cos(2 * w);
  const sw2 = Math.sin(2 * w);
  const num = Math.hypot(b0 + b1 * cw + b2 * cw2, -(b1 * sw + b2 * sw2));
  const den = Math.hypot(a0 + a1 * cw + a2 * cw2, -(a1 * sw + a2 * sw2));
  if (den === 0) return 0;
  return linearToDbFloored(num / den, floorDb);
}

/** The shapes a drawn band takes: the plugin and surface spelling. */
export type BiquadType = 'bell' | 'lowshelf' | 'highshelf' | 'highpass' | 'lowpass' | 'notch' | 'allpass1' | 'allpass2';

const SHAPE_KIND: Readonly<Record<BiquadType, BiquadKind>> = {
  bell: 'peaking',
  lowshelf: 'lowShelf',
  highshelf: 'highShelf',
  highpass: 'highpass',
  lowpass: 'lowpass',
  notch: 'notch',
  allpass1: 'allpass1',
  allpass2: 'allpass2',
};

/** One drawn band in engineering units. */
export interface BiquadBand {
  readonly type: BiquadType;
  /** Centre or corner, Hz. */
  readonly freq: number;
  /** Gain, dB (bell and shelves only). */
  readonly gainDb: number;
  /** Quality factor. */
  readonly q: number;
}

/**
 * One band's magnitude at `freq`, dB: {@link rbjSection} evaluated by {@link sectionMagnitudeDb}. The twin of
 * omx_eq_band_magnitude_db.
 * @param floorDb The floor of a null, dB (the kernel's `floor_db`).
 */
export function biquadMagnitudeDb(band: BiquadBand, freq: number, fs: number, floorDb: number): number {
  if (!(freq > 0) || !(fs > 0) || freq >= fs / 2) return 0;
  return sectionMagnitudeDb(rbjSection(SHAPE_KIND[band.type], band.freq, band.q, band.gainDb, fs), freq, fs, floorDb);
}

/**
 * The summed magnitude of a band set at one frequency, dB (cascaded sections multiply, so dB add). The twin of
 * omx_eq_magnitude_sum_db.
 * @param floorDb The floor of each band's null, dB.
 */
export function magnitudeSumDb(bands: readonly BiquadBand[], f: number, fs: number, floorDb: number): number {
  let db = 0;
  for (const b of bands) db += biquadMagnitudeDb(b, f, fs, floorDb);
  return db;
}
