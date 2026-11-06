// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * curvePoints, the twin of omx_curve_points (omx_curve.h): a panel description's display, bound to the current
 * values and a rate, to its points. The same words (the description's display, family, role, unit and shape
 * words) and the same codes (CURVE_CODES), checked in the same order. Pinned by test/agreement.test.mjs at the
 * nine oracle rates: the EQ through the magnitude twins, the transfer curves through the gain computer twins.
 */
import {
  COMP_KNEE_DB_RANGE,
  COMP_RATIO_RANGE,
  CURVE_CODES,
  CURVE_DISPLAYS,
  CURVE_EXTENTS,
  CURVE_FAMILIES,
  CURVE_ORIGINS,
  CURVE_POINTS_MAX,
  CURVE_ROLES,
  CURVE_SHAPES,
  CURVE_UNITS,
  EQ_BAND_DEFAULTS,
  GATE_RANGE_DB_RANGE,
  GATE_RATIO_RANGE,
  LIN_TO_DB_FLOOR,
} from './definitions.js';
import { clampEqQFor, magnitudeSumDb, octavesToQ, type BiquadBand, type BiquadType } from './eq.js';
import { gaincompDb, gaincompDbShifted, gateKnee } from './gaincomp.js';

/** A set id as the description spells it: `eqResponse` is `eq_response`; the unshaped band is `-`. */
const wordOf = (id: string): string => (id === 'unshaped' ? '-' : id.replace(/[A-Z]/g, (c) => `_${c.toLowerCase()}`));

/** The wire words of each set, in value order. */
export const CURVE_WORDS = {
  displays: CURVE_DISPLAYS.map(wordOf),
  families: CURVE_FAMILIES.map(wordOf),
  roles: CURVE_ROLES.map(wordOf),
  units: CURVE_UNITS.map(wordOf),
  shapes: CURVE_SHAPES.map(wordOf),
  /** fbs-fixed and fbs-live: the origins are spelt with a hyphen */
  origins: CURVE_ORIGINS.map((id) => id.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)),
} as const;

/** What curvePoints answers when it draws nothing: a CURVE_CODES id other than `ok`. */
export type CurveCode = Exclude<(typeof CURVE_CODES)[number], 'ok'>;

/** One bound term: a role of the description, its value now, in the unit the description gives it. */
export interface CurveTerm {
  /** The role word: `band_freq`, `threshold`, `knee_start`… */
  readonly role: string;
  /** The band's index for a `band_*` role; 0 (or absent) for any other. */
  readonly index?: number;
  /** The control's plain value. */
  readonly value: number;
  /** The unit token: `hz`, `khz`, `db`, `coef`, `oct`, `none`… */
  readonly unit: string;
  /** For `band_type`, the shape its value selects (from the role's `shapes`); for `band_freq`, the fixed shape. */
  readonly shape?: string;
  /** Where the band comes from: `user` (the default), `fbs-fixed` or `fbs-live`. A band's is its band_freq term's. */
  readonly origin?: string;
}

/** A curve request: one display of the description, bound. */
export interface CurveRequest {
  /** The display's kind: `eq_response` or `transfer_curve`. */
  readonly kind: string;
  /** The family: `eq`, `dynamics`, `limiter`, `gate` or `deesser`. */
  readonly family: string;
  /** The instance's rate, Hz. */
  readonly sampleRate: number;
  /** The grid points, 2 to CURVE_POINTS_MAX (an EQ's anchors on top). */
  readonly points: number;
  /** The x extents; CURVE_EXTENTS when absent. */
  readonly extents?: { readonly min: number; readonly max: number };
  /** The terms; a later term of one role and index replaces an earlier. */
  readonly terms: readonly CurveTerm[];
  /** Also answer the curve over the `user` bands alone, at the same x. */
  readonly userOnly?: boolean;
}

/** A band whose origin is not `user`: its centre (Hz), its own magnitude there (dB), its Q and its origin. */
export interface CurveMark {
  readonly freq: number;
  readonly depthDb: number;
  readonly q: number;
  readonly origin: string;
}

/** One point: Hz and dB for an EQ, input dB and output dB for a transfer curve. */
export interface CurvePoint {
  readonly x: number;
  readonly y: number;
}

/** The answer: the points, ascending in x, or the code of what is missing or refused. */
export type CurveAnswer =
  | {
      readonly points: readonly CurvePoint[];
      /** One per drawn band whose origin is not `user`, in band order; none for a transfer curve. */
      readonly marks: readonly CurveMark[];
      /** With `userOnly`: the curve over the user bands (and the head filters) alone, at the same x. */
      readonly userPoints?: readonly CurvePoint[];
    }
  | { readonly code: CurveCode };

/** The units each role converts from (omx_curve_unit_accepted). */
const ACCEPTS: Readonly<Record<string, readonly string[]>> = {
  band_freq: ['hz', 'khz'], hpf_freq: ['hz', 'khz'], lpf_freq: ['hz', 'khz'],
  band_gain: ['db', 'coef'],
  band_q: ['none', 'coef', 'oct'],
  band_type: ['none'], band_on: ['none'], hpf_on: ['none'], lpf_on: ['none'],
  hpf_slope: ['none', 'db'], lpf_slope: ['none', 'db'],
  threshold: ['db', 'dbfs', 'dbu', 'lufs'],
  ratio: ['ratio', 'coef'],
  knee: ['db'], makeup: ['db'], range: ['db'], knee_start: ['db'], knee_end: ['db'],
};

/** omx_lin_to_db: a linear factor to dB, floored at LIN_TO_DB_FLOOR. */
const linToDb = (lin: number): number => {
  const v = Math.fround(lin);
  return Math.fround(20 * Math.log10(v < Math.fround(LIN_TO_DB_FLOOR.lin) ? Math.fround(LIN_TO_DB_FLOOR.lin) : v));
};

/** A term's value in the model's unit (omx_curve_term_value). */
const valueOf = (t: CurveTerm): number => {
  switch (t.unit) {
    case 'khz': return t.value * 1000;
    case 'coef': return t.role === 'band_gain' ? linToDb(t.value) : t.value;
    case 'oct': return octavesToQ(t.value);
    default: return t.value;
  }
};

const SHAPE_TYPE: Readonly<Record<string, BiquadType>> = {
  bell: 'bell', lowshelf: 'lowshelf', highshelf: 'highshelf', highpass: 'highpass', lowpass: 'lowpass', notch: 'notch',
  allpass1: 'allpass1', allpass2: 'allpass2',
};

/** The Q of each section of a Butterworth head filter (omx_eq_butterworth_qs): 12 dB/oct one at 1/sqrt(2), 24 dB/oct
 *  two whose product is maximally flat, the kernel's own digits. */
export function butterworthQs(slope24: boolean): readonly number[] {
  return slope24 ? [0.5411961, 1.306563] : [Math.SQRT1_2];
}

/** The frequencies a drawn curve is sampled at (omx_eq_curve_freqs): the log grid and the bands' anchors inside it,
 *  ascending. */
function curveFreqs(bands: readonly BiquadBand[], fMin: number, fMax: number, points: number): number[] {
  const g = Math.max(points, 2);
  const out: number[] = [];
  for (let i = 0; i < g; i++) out.push(fMin * Math.pow(fMax / fMin, i / (g - 1)));
  for (const b of bands) {
    const half = b.freq / (2 * (b.q > 0.1 ? b.q : 0.1));
    for (const a of [b.freq - half, b.freq, b.freq + half]) if (a > fMin && a < fMax) out.push(a);
  }
  for (let i = 1; i < out.length; i++) {
    const v = out[i];
    let j = i;
    for (; j > 0 && out[j - 1] > v; j--) out[j] = out[j - 1];
    out[j] = v;
  }
  return out;
}

/**
 * The points of one display of a panel description: the twin of omx_curve_points, the same checks in the same
 * order, the same codes.
 * @param rq The request.
 */
export function curvePoints(rq: CurveRequest): CurveAnswer {
  const eq = rq.kind === 'eq_response';
  if (!(eq ? rq.family === 'eq' : rq.kind === 'transfer_curve' && rq.family !== 'eq' && CURVE_WORDS.families.includes(rq.family)))
    return { code: 'kindUnknown' };
  if (!Number.isInteger(rq.points) || rq.points < 2 || rq.points > CURVE_POINTS_MAX) return { code: 'pointsRange' };
  if (!Number.isFinite(rq.sampleRate) || !(rq.sampleRate > 0) || (rq.extents && !(Number.isFinite(rq.extents.min) && Number.isFinite(rq.extents.max))))
    return { code: 'valueNotFinite' };
  if (rq.terms.some((t) => !Number.isFinite(t.value))) return { code: 'valueNotFinite' };
  if (rq.terms.some((t) => !ACCEPTS[t.role]?.includes(t.unit) || !CURVE_WORDS.origins.includes(t.origin ?? 'user')))
    return { code: 'unitRefused' };
  let lo: number, hi: number;
  if (rq.extents) [lo, hi] = [rq.extents.min, rq.extents.max];
  else if (eq) [lo, hi] = [CURVE_EXTENTS.freqMinHz, Math.min(CURVE_EXTENTS.freqMaxHz, (CURVE_EXTENTS.nyquistFraction * rq.sampleRate) / 2)];
  else [lo, hi] = [CURVE_EXTENTS.levelMinDb, CURVE_EXTENTS.levelMaxDb];
  if (!(hi > lo) || (eq && !(lo > 0))) return { code: 'pointsRange' };
  const find = (role: string, index = 0): CurveTerm | undefined => {
    let found: CurveTerm | undefined;
    for (const t of rq.terms) if (t.role === role && (t.index ?? 0) === index) found = t;
    return found;
  };
  return eq ? eqPoints(rq, find, lo, hi) : transferPoints(rq, find, lo, hi);
}

type Find = (role: string, index?: number) => CurveTerm | undefined;

const BANDS_MAX = Math.floor((CURVE_POINTS_MAX - 2) / 3);
const BAND_ROLES = ['band_type', 'band_freq', 'band_gain', 'band_q', 'band_on'];

function eqPoints(rq: CurveRequest, find: Find, lo: number, hi: number): CurveAnswer {
  let nb = 0;
  for (const t of rq.terms) {
    if (!BAND_ROLES.includes(t.role)) continue;
    const i = t.index ?? 0;
    if (i >= BANDS_MAX) return { code: 'pointsRange' };
    nb = Math.max(nb, i + 1);
  }
  const bands: BiquadBand[] = [];
  const origin: string[] = [];
  for (let b = 0; b < nb; b++) {
    const type = find('band_type', b), freq = find('band_freq', b), gain = find('band_gain', b), q = find('band_q', b), on = find('band_on', b);
    if (!type && !freq && !gain && !q && !on) continue;
    if (!freq) return { code: 'termMissing' };
    const shape = type?.shape ?? freq.shape;
    if (shape === undefined) return { code: 'termMissing' };
    const kind = SHAPE_TYPE[shape];
    if (!kind) continue; // off, or a choice with no shape (`-`): not drawn
    const gains = kind === 'bell' || kind === 'lowshelf' || kind === 'highshelf';
    if (gains && !gain) return { code: 'termMissing' };
    if (on && valueOf(on) === 0) continue;
    bands.push({ type: kind, freq: valueOf(freq), gainDb: gains && gain ? valueOf(gain) : 0,
      // no Q control: the declared default (EQ_BAND_DEFAULTS), clamped for the shape as the kernel clamps it
      q: q ? valueOf(q) : Math.fround(clampEqQFor(kind === 'notch' ? 'notch' : 'bell', EQ_BAND_DEFAULTS.q)) });
    origin.push(freq.origin ?? 'user');
  }
  const pass: BiquadBand[] = [];
  let present = false;
  for (const [type, pre] of [['highpass', 'hpf'], ['lowpass', 'lpf']] as const) {
    const fr = find(`${pre}_freq`), sl = find(`${pre}_slope`), on = find(`${pre}_on`);
    if (!fr && !sl && !on) continue;
    present = true;
    // the slope in dB per octave, from the role's declared enumeration or a dB control; a bare choice value is none
    if (!fr || !sl || sl.unit !== 'db') return { code: 'termMissing' };
    const slope = valueOf(sl);
    if (slope !== 12 && slope !== 24) return { code: 'kindUnknown' };
    if (on && valueOf(on) === 0) continue;
    const corner = valueOf(fr);
    if (corner > 0) for (const q of butterworthQs(slope === 24)) pass.push({ type, freq: corner, gainDb: 0, q });
  }
  if (nb === 0 && !present) return { code: 'termMissing' };
  const drawn = [...bands, ...pass];
  if (rq.points + 3 * drawn.length > CURVE_POINTS_MAX) return { code: 'pointsRange' };
  const top = (rq.sampleRate / 2) * 0.999;
  const xs = curveFreqs(drawn, lo, hi, rq.points);
  const sum = (set: readonly BiquadBand[]) =>
    xs.map((x) => ({ x, y: magnitudeSumDb(set, x < top ? x : top, rq.sampleRate, CURVE_EXTENTS.eqFloorDb) }));
  const marks = bands.flatMap((b, i) =>
    origin[i] === 'user' ? [] : [{ freq: b.freq, depthDb: magnitudeSumDb([b], b.freq, rq.sampleRate, CURVE_EXTENTS.eqFloorDb), q: b.q, origin: origin[i] }]);
  const points = sum(drawn);
  if (!rq.userOnly) return { points, marks };
  return { points, marks, userPoints: sum([...bands.filter((_, i) => origin[i] === 'user'), ...pass]) };
}

function transferPoints(rq: CurveRequest, find: Find, lo: number, hi: number): CurveAnswer {
  const below = rq.family === 'gate';
  const thr = find('threshold'), ratio = find('ratio'), knee = find('knee'), makeup = find('makeup'), range = find('range');
  const ks = find('knee_start'), ke = find('knee_end');
  // a limiter has no ratio role: the dynamics form at ratio infinity, a hard corner at the threshold
  const limiter = rq.family === 'limiter';
  if (!thr || (!ratio && !limiter) || (below && !range) || (below && !ks !== !ke)) return { code: 'termMissing' };
  const f = Math.fround;
  const r = ratio ? f(valueOf(ratio)) : Infinity, k = knee ? f(valueOf(knee)) : 0;
  const rmin = f(below ? GATE_RATIO_RANGE.min : COMP_RATIO_RANGE.min);
  const thresh = f(valueOf(thr));
  let kneeDb = k < COMP_KNEE_DB_RANGE.min ? COMP_KNEE_DB_RANGE.min : k;
  let shiftDb = 0;
  let rangeDb = 0;
  if (below && range) {
    const rg = f(valueOf(range));
    rangeDb = rg > GATE_RANGE_DB_RANGE.max ? GATE_RANGE_DB_RANGE.max : rg;
    if (ks && ke) ({ kneeDb, shiftDb } = gateKnee(thresh, valueOf(ks), valueOf(ke)));
  }
  const p = { mode: below ? ('below' as const) : ('above' as const), threshDb: thresh, ratio: r < rmin ? rmin : r, kneeDb, rangeDb };
  const mk = makeup ? valueOf(makeup) : 0;
  const n = rq.points;
  const points: CurvePoint[] = [];
  for (let i = 0; i < n; i++) {
    const lv = i + 1 === n ? hi : lo + ((hi - lo) * i) / (n - 1);
    const g = below ? gaincompDbShifted(p, shiftDb, lv) : gaincompDb(p, lv);
    points.push({ x: lv, y: lv + g + mk });
  }
  return rq.userOnly ? { points, marks: [], userPoints: points } : { points, marks: [] };
}
