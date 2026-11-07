// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * EVERY TWIN AGREES WITH ITS KERNEL: the twin (dist/, the published code) and the omx-dsp kernel
 * (oracle/ts-twin-oracle.c) run one grid and must agree inside the tolerance test/tolerances.mjs states. A twin
 * whose SABOTAGE arm stays green would be decoration, so each has one. Moved here from the console with the twins.
 */
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { against, ask, oracle, oracleKernels, oracleRates } from '../oracle/ts-twin-oracle.mjs';
import {
  CURVE_CODES,
  CURVE_POINTS_MAX,
  CURVE_WORDS,
  EQ_NOTCH_Q_RANGE,
  EQ_Q_RANGE,
  biquadMagnitudeDb,
  butterworthQs,
  clampEqQFor,
  curvePoints,
  dbToLinearGain,
  fftSizeForRate,
  gaincompDb,
  gaincompDbShifted,
  gateKnee,
  linearGainToDb,
  linearToDbFloored,
  magnitudeSumDb,
  octavesToQ,
  rbjSection,
} from '../dist/index.js';
import { TOLERANCES as T } from './tolerances.mjs';

const red = (asked, twin, tol) => against(asked, twin, tol).failures.length > 0;

describe('the oracle is here', () => {
  it('answers every kernel these twins need', () => {
    const k = oracleKernels();
    for (const n of ['lin_to_db', 'lin_to_db_d', 'db_to_lin', 'eq_octaves_q', 'rta_fft_size_for', 'eq_design', 'eq_lv2_clamp_q', 'eq_band_magnitude_db', 'eq_magnitude_sum_db', 'curve_points', 'curve_points_marked', 'gaincomp_db', 'gaincomp_db_shifted', 'gate_knee']) assert.ok(k.includes(n), n);
  });
});

describe('the rate grid is the library\'s', () => {
  it('is the contract\'s full oracle grid: at least nine rates, 32 to 192 kHz', () => {
    const r = oracleRates();
    assert.ok(r.length >= 9, String(r));
    assert.equal(Math.min(...r), 32000);
    assert.equal(Math.max(...r), 192000);
  });
});

describe('the dB and linear twins agree with omx_units.h', () => {
  const LIN = [1e-12, 1e-10, 1e-9, 3e-9, 1e-7, 1e-5, 1e-3, 0.01, 0.1, 0.25, 0.5, 0.7071, 1, 1.5, 2, 10, 1000];
  it('linearToDbFloored is omx_lin_to_db_d for silence, negatives, every floor and loud values', () => {
    const rows = [-240, -120, -100, -60, -20].flatMap((f) => [0, -1, -1e-30, ...LIN].map((x) => [x, 10 ** (f / 20)]));
    const asked = ask('lin_to_db_d', rows);
    const floorOf = (lin) => 20 * Math.log10(lin);
    assert.deepEqual(against(asked, (r) => [linearToDbFloored(r[0], Math.round(floorOf(r[1])))], T.linearToDbFloored).failures, []);
    assert.ok(red(asked, (r) => [Math.min(Math.round(floorOf(r[1])), linearGainToDb(r[0]))], T.linearToDbFloored), 'SABOTAGE min for max stayed green');
  });
  it('linearGainToDb is omx_lin_to_db wherever the kernel does not floor (>= 1e-9)', () => {
    const asked = ask('lin_to_db', LIN.filter((x) => x >= 1e-9).map((x) => [x]));
    assert.deepEqual(against(asked, (r) => [linearGainToDb(r[0])], T.linearGainToDb).failures, []);
    assert.ok(red(asked, (r) => [10 * Math.log10(r[0])], T.linearGainToDb), 'SABOTAGE 10 log10 stayed green');
  });
  it('DESIGN DIFFERENCE: below 1e-9 the kernel floors at -180 dB, the twin reads -Infinity for silence', () => {
    const [silent, tiny] = oracle('lin_to_db', [[0], [1e-12]]).map((r) => r[0]);
    assert.equal(silent, -180);
    assert.equal(tiny, -180);
    assert.equal(linearGainToDb(0), -Infinity);
  });
  it('dbToLinearGain is omx_db_to_lin to a float32 power across -120..+120 dB', () => {
    const dbs = [-120, -96, -60, -24, -12, -6, -3, -0.1, 0, 0.1, 3, 6, 12, 24, 60, 96, 120];
    const asked = ask('db_to_lin', dbs.map((d) => [d]));
    assert.deepEqual(against(asked, (r) => [dbToLinearGain(r[0])], T.dbToLinearGain).failures, []);
    assert.ok(red(asked, (r) => [10 ** (r[0] / 10)], T.dbToLinearGain), 'SABOTAGE 10^(db/10) stayed green');
    assert.deepEqual(oracle('db_to_lin', [[-Infinity]])[0], [0]);
    assert.equal(dbToLinearGain(-Infinity), 0);
  });
});

describe('octavesToQ agrees with omx_eq_octaves_q', () => {
  const asked = ask('eq_octaves_q', [-3, 0, 0.001, 0.049, 0.05, 0.051, 0.1, 0.25, 0.5, 1, 1.5, 2, 3, 4, 6, 8, 12].map((b) => [b]));
  it('agrees, floor included; the floor removed and sqrt(t) for t are each red', () => {
    assert.deepEqual(against(asked, (r) => [octavesToQ(r[0])], T.octavesToQ).failures, []);
    assert.ok(red(asked, (r) => [Math.sqrt(2 ** r[0]) / (2 ** r[0] - 1)], T.octavesToQ), 'SABOTAGE no floor stayed green');
    assert.ok(red(asked, (r) => [2 ** Math.max(r[0], 0.05) / (2 ** Math.max(r[0], 0.05) - 1)], T.octavesToQ), 'SABOTAGE t for sqrt(t) stayed green');
  });
});

describe('fftSizeForRate agrees with omx_rta_fft_size_for', () => {
  // Every whole-hertz rate from 1 kHz to 400 kHz: the kernel divides in float and the twin in double.
  const rows = [];
  for (let r = 1000; r <= 400000; r++) rows.push([r]);
  const asked = ask('rta_fft_size_for', rows);
  it('agrees at every whole rate; a doubled window is red', () => {
    assert.deepEqual(against(asked, (r) => [fftSizeForRate(r[0])], T.fftSizeForRate).failures.slice(0, 5), []);
    assert.ok(against(asked, (r) => [fftSizeForRate(r[0]) * 2], T.fftSizeForRate).failures.length > 1000);
  });
});

const RATES = oracleRates();
/** omx_eq_kind by the twin's word, and the gain-carrying kinds. */
const KIND = { peaking: 0, lowShelf: 1, highShelf: 2, highpass: 3, lowpass: 4, notch: 5, bandpass: 6, allpass1: 7, allpass2: 8 };
const KINDS = Object.keys(KIND);
const HAS_GAIN = new Set([0, 1, 2]);
const kindOf = (n) => KINDS.find((k) => KIND[k] === n);

describe('rbjSection (and matchedPair, lowpassHandover inside it) agrees with omx_eq_design', () => {
  const QS = [-1, 0.05, 0.3, 0.5, Math.SQRT1_2, 0.9, 1, 4, 20, 116];
  const GAINS = [-24, -12, -6, -0.5, 0, 0.5, 6, 12, 24];
  const centres = (sr) => [0, 1, 20, 63, 200, 1000, 4000, 10000, 0.45 * sr, 0.499 * sr, 0.5 * sr, 0.6 * sr];
  const rows = [];
  for (const k of KINDS) for (const sr of RATES) for (const f of centres(sr)) for (const q of QS)
    for (const g of HAS_GAIN.has(KIND[k]) ? GAINS : [0]) rows.push([KIND[k], f, q, g, sr]);
  const asked = ask('eq_design', rows);
  const coeffsOf = (r) => {
    const s = rbjSection(kindOf(r[0]), r[1], r[2], r[3], r[4]);
    return [s.b0 / s.a0, s.b1 / s.a0, s.b2 / s.a0, s.a1 / s.a0, s.a2 / s.a0];
  };
  it('spans all nine kinds at every rate of the grid', () => {
    assert.equal(new Set(asked.rows.map((r) => r[0])).size, 9);
    assert.ok(asked.rows.length > 30000);
  });
  it('agrees within T.rbjSection; the worst gap is under 1e-12', () => {
    const r = against(asked, coeffsOf, T.rbjSection);
    assert.deepEqual(r.failures.slice(0, 5), []);
    assert.ok(r.worst < 1e-12, String(r.worst));
  });
  it('SABOTAGE: a coefficient moved by 1e-8 (under float32), the shelves swapped, the gain law 10^(g/39): each red', () => {
    assert.ok(against(asked, (r) => coeffsOf(r).map((c, i) => (i === 3 ? c + 1e-8 : c)), T.rbjSection).failures.length > asked.rows.length / 2);
    const swap = (r) => coeffsOf([r[0] === 1 ? 2 : r[0] === 2 ? 1 : r[0], ...r.slice(1)]);
    assert.ok(against(asked, swap, T.rbjSection).failures.length > 1000);
    const skew = (r) => coeffsOf([r[0], r[1], r[2], r[3] * (40 / 39), r[4]]);
    assert.ok(against(asked, skew, T.rbjSection).failures.length > 1000);
  });
});

describe('clampEqQFor agrees with omx_eq_lv2_clamp_q', () => {
  const LV2 = { bell: 0, lowShelf: 1, highShelf: 2, notch: 3, allpass1: 4, allpass2: 5 };
  const types = Object.keys(LV2);
  const qs = [-5, 0, 0.01, EQ_Q_RANGE.min, EQ_Q_RANGE.min * 1.0000001, 0.1, 0.299, EQ_NOTCH_Q_RANGE.min, Math.SQRT1_2, 1, 2.5, 50,
    EQ_Q_RANGE.max, EQ_Q_RANGE.max + 0.0001, 115.9, 116, 116.1, 500];
  const asked = ask('eq_lv2_clamp_q', types.flatMap((t) => qs.map((q) => [LV2[t], q])));
  const typeOf = (n) => types.find((t) => LV2[t] === n);
  it('agrees for every band type, inside and outside both ranges', () => {
    assert.deepEqual(against(asked, (r) => [clampEqQFor(typeOf(r[0]), r[1])], T.clampEqQFor).failures, []);
  });
  it('SABOTAGE: the notch given the general range is red', () => {
    assert.ok(against(asked, (r) => [clampEqQFor('bell', r[1])], T.clampEqQFor).failures.length > 0);
  });
});

/** The drawn shapes by the kernel's kind number. */
const SHAPES = [
  { type: 'bell', kind: 0 }, { type: 'lowshelf', kind: 1 }, { type: 'highshelf', kind: 2 }, { type: 'highpass', kind: 3 },
  { type: 'lowpass', kind: 4 }, { type: 'notch', kind: 5 }, { type: 'allpass1', kind: 7 }, { type: 'allpass2', kind: 8 },
];
const shapeOf = (kind) => SHAPES.find((s) => s.kind === kind).type;
/** A null's floor: the console draws to -120 dB; the kernel takes it as an argument, so any floor would do. */
const FLOOR_DB = -120;

describe('biquadMagnitudeDb agrees with omx_eq_band_magnitude_db', () => {
  const rows = [];
  for (const sr of RATES) for (const s of SHAPES) for (const f0 of [60, 1000, 8000, 0.4 * sr]) for (const q of [0.5, Math.SQRT1_2, 2, 20])
    for (const g of HAS_GAIN.has(s.kind) ? [-12, -3, 0, 3, 12] : [0])
      for (const at of [-5, 0, 20, 100, f0 * 0.5, f0, f0 * 1.01, f0 * 2, 0.45 * sr, 0.5 * sr, 0.6 * sr]) rows.push([s.kind, f0, g, q, at, sr, FLOOR_DB]);
  const asked = ask('eq_band_magnitude_db', rows);
  const twin = (r) => [biquadMagnitudeDb({ type: shapeOf(r[0]), freq: r[1], gainDb: r[2], q: r[3] }, r[4], r[5], r[6])];
  it('agrees across kinds, centres, Qs, gains and probe points, at and above Nyquist included', () => {
    const r = against(asked, twin, T.biquadMagnitudeDb);
    assert.deepEqual(r.failures.slice(0, 5), []);
    assert.ok(asked.rows.length > 20000);
    assert.ok(r.worst < 1e-9, String(r.worst));
  });
  it('SABOTAGE: a millionth of a dB anywhere, and the Nyquist guard dropped: each red', () => {
    assert.ok(against(asked, (r) => twin(r).map((x) => x + 1e-6), T.biquadMagnitudeDb).failures.length > asked.rows.length * 0.9);
    assert.ok(against(asked, (r) => (r[4] >= r[5] / 2 ? [1] : twin(r)), T.biquadMagnitudeDb).failures.length > 0);
  });
  it('SABOTAGE: a different floor is red on a null', () => {
    const null0 = ask('eq_band_magnitude_db', [[5, 1000, 0, 2, 1000, 48000, FLOOR_DB]]);
    assert.equal(null0.answers[0][0], FLOOR_DB);
    assert.ok(against(null0, (r) => [biquadMagnitudeDb({ type: 'notch', freq: r[1], gainDb: 0, q: r[3] }, r[4], r[5], -100)], T.biquadMagnitudeDb).failures.length > 0);
  });
});

describe('magnitudeSumDb agrees with omx_eq_magnitude_sum_db', () => {
  const six = SHAPES.slice(0, 6);
  const rows = [];
  for (const sr of RATES) for (let seed = 0; seed < 12; seed++) {
    const nb = seed % 5;
    const bands = [];
    for (let i = 0; i < nb; i++) {
      const s = six[(seed + i) % six.length];
      bands.push(s.kind, 60 * (1 + ((seed * 5 + i * 11) % 120)), s.kind <= 2 ? ((seed + i * 3) % 25) - 12 : 0, 0.4 + ((seed + i) % 6));
    }
    for (const at of [10, 80, 500, 3000, 12000, 0.45 * sr, 0.5 * sr]) rows.push([at, sr, FLOOR_DB, nb, ...bands]);
  }
  const asked = ask('eq_magnitude_sum_db', rows);
  const bandsOf = (r) => Array.from({ length: r[3] }, (_, i) => ({ type: shapeOf(r[4 + 4 * i]), freq: r[5 + 4 * i], gainDb: r[6 + 4 * i], q: r[7 + 4 * i] }));
  it('agrees at every rate, the empty set included', () => {
    assert.deepEqual(against(asked, (r) => [magnitudeSumDb(bandsOf(r), r[0], r[1], r[2])], T.magnitudeSumDb).failures.slice(0, 5), []);
    assert.ok(asked.answers.some((a) => a[0] !== 0));
  });
  it('SABOTAGE: the last band left out of the sum is red', () => {
    assert.ok(against(asked, (r) => [magnitudeSumDb(bandsOf(r).slice(0, -1), r[0], r[1], r[2])], T.magnitudeSumDb).failures.length > 0);
  });
});

/** The next float above `v`: one step of the float the kernel stores. */
const nextFloat = (v) => {
  const b = new Float32Array([v]);
  const i = new Int32Array(b.buffer);
  i[0] += v >= 0 ? 1 : -1;
  return b[0];
};
const MODE = { above: 0, below: 1, upward: 2 };
const MODES = Object.keys(MODE);

describe('gaincompDb agrees with omx_gaincomp_db, bit for bit', () => {
  const rows = [];
  for (const m of MODES) for (const t of [-60, -40, -20.5, -6, 0]) for (const r of [1, 1.5, 4, 20, 100]) for (const k of [0, 0.5, 3, 6, 12, 24])
    for (const rg of m === 'upward' ? [0, 6, 24] : m === 'below' ? [-90, -60, -20, 0] : [0])
      for (let lv = -100; lv <= 12; lv += 0.75) rows.push([MODE[m], t, r, k, rg, lv]);
  const asked = ask('gaincomp_db', rows);
  const twin = (r) => [gaincompDb({ mode: MODES[r[0]], threshDb: r[1], ratio: r[2], kneeDb: r[3], rangeDb: r[4] }, r[5])];
  it('agrees in every mode, inside and outside the knee', () => {
    assert.ok(asked.rows.length > 40000);
    assert.deepEqual(against(asked, twin, T.gaincompDb).failures.slice(0, 5), []);
  });
  it('SABOTAGE: the knee one float step wider is red', () => {
    const wide = (r) => [gaincompDb({ mode: MODES[r[0]], threshDb: r[1], ratio: r[2], kneeDb: nextFloat(r[3]), rangeDb: r[4] }, r[5])];
    assert.ok(against(asked, wide, T.gaincompDb).failures.length > 100);
  });
});

describe('gaincompDbShifted and gateKnee agree with omx_gaincomp_db_shifted and omx_gate_knee, bit for bit', () => {
  const pairs = [[-46, -38], [-40, -40], [-43, -37], [-38, -46], [-60, -20], [-41, -40], [-80, 0]];
  const krows = [];
  for (const t of [-60, -40, -33.3, -10]) for (const [a, b] of pairs) krows.push([t, a, b]);
  const kasked = ask('gate_knee', krows);
  it('gateKnee: the width and the clamped shift', () => {
    assert.deepEqual(against(kasked, (r) => { const k = gateKnee(r[0], r[1], r[2]); return [k.kneeDb, k.shiftDb]; }, T.gateKnee).failures, []);
    assert.ok(kasked.answers.some((a) => a[1] !== 0) && kasked.answers.some((a) => Math.abs(a[1]) === a[0] / 2 && a[0] > 0), 'shifted and clamped shifts are both in the grid');
  });
  const rows = [];
  for (const [i, [t, a, b]] of krows.entries()) for (const r of [1, 2, 4, 16, 100]) for (const rg of [-90, -60, -20, 0])
    for (let lv = -100; lv <= 6; lv += 0.5) rows.push([t, r, kasked.answers[i][0], rg, kasked.answers[i][1], lv]);
  const asked = ask('gaincomp_db_shifted', rows);
  const twin = (r) => [gaincompDbShifted({ mode: 'below', threshDb: r[0], ratio: r[1], kneeDb: r[2], rangeDb: r[3] }, r[4], r[5])];
  it('gaincompDbShifted agrees across shifts, ratios and ranges', () => {
    assert.deepEqual(against(asked, twin, T.gaincompDbShifted).failures.slice(0, 5), []);
  });
  it('SABOTAGE: the shift one float step off, and the centred knee for the shifted one: each red', () => {
    assert.ok(against(asked, (r) => [gaincompDbShifted({ mode: 'below', threshDb: r[0], ratio: r[1], kneeDb: r[2], rangeDb: r[3] }, r[4] === 0 ? 0 : nextFloat(r[4]), r[5])], T.gaincompDbShifted).failures.length > 50);
    assert.ok(against(asked, (r) => [gaincompDb({ mode: 'below', threshDb: r[0], ratio: r[1], kneeDb: r[2], rangeDb: r[3] }, r[5])], T.gaincompDbShifted).failures.length > 50);
  });
});

describe('butterworthQs agrees with omx_eq_butterworth_qs', () => {
  it('one section at 12 dB/oct, two at 24, the kernel\'s digits; the 24 dB pair swapped is red', () => {
    const asked = ask('eq_butterworth_qs', [[0], [1]]);
    const twin = (r) => { const q = butterworthQs(r[0] === 1); return [q.length, q[0], q[1] ?? 0]; };
    assert.deepEqual(against(asked, twin, { abs: 0 }).failures, []);
    assert.ok(against(asked, (r) => { const q = [...butterworthQs(r[0] === 1)].reverse(); return [q.length, q[0], q[1] ?? 0]; }, { abs: 0 }).failures.length > 0);
  });
});

/** A curve request as the oracle's row: kind family rate points has_extents x_min x_max nterms { role index value unit shape }. */
const idx = (list, w) => { const i = list.indexOf(w); if (i < 0) throw new Error(`no word ${w}`); return i; };
const rowOf = (rq) => [
  idx(CURVE_WORDS.displays, rq.kind), idx(CURVE_WORDS.families, rq.family), rq.sampleRate, rq.points, rq.extents ? 1 : 0,
  rq.extents?.min ?? 0, rq.extents?.max ?? 0, rq.terms.length,
  ...rq.terms.flatMap((t) => [idx(CURVE_WORDS.roles, t.role), t.index ?? 0, t.value, idx(CURVE_WORDS.units, t.unit), t.shape === undefined ? -1 : idx(CURVE_WORDS.shapes, t.shape)]),
];
/** The twin's answer as the oracle spells it: code count { x y }. */
const answerOf = (a) => ('code' in a ? [CURVE_CODES.indexOf(a.code), 0] : [0, a.points.length, ...a.points.flatMap((p) => [p.x, p.y])]);
const askCurves = (reqs) => ({ ...ask('curve_points', reqs.map(rowOf)), reqs });
const curveAgainst = (asked, twin, tol) => {
  const failures = [];
  let worst = 0;
  asked.reqs.forEach((rq, i) => {
    const t = answerOf(twin(rq)), k = asked.answers[i];
    if (t[0] !== k[0] || t[1] !== k[1]) { failures.push(`request ${i}: twin code ${t[0]} count ${t[1]}, kernel code ${k[0]} count ${k[1]}`); return; }
    for (let j = 2; j < t.length; j++) {
      const d = Math.abs(t[j] - k[j]), allowed = j % 2 === 0 ? Math.max(tol.abs ?? 0, 1e-15 * Math.abs(k[j])) : tol.abs ?? 0;
      if (!(d <= allowed)) { failures.push(`request ${i} ${j % 2 ? 'y' : 'x'}[${(j - 2) >> 1}]: twin ${t[j]}, kernel ${k[j]}`); break; }
      if (j % 2) worst = Math.max(worst, d);
    }
  });
  return { failures, worst };
};
const withTerm = (rq, f) => ({ ...rq, terms: rq.terms.map(f) });

describe('curvePoints agrees with omx_curve_points at every oracle rate', () => {
  const SH = ['bell', 'lowshelf', 'highshelf', 'highpass', 'lowpass', 'notch'];
  const eqReqs = (coef) => {
    const out = [];
    for (const sr of RATES) for (let seed = 0; seed < 14; seed++) {
      const nb = 1 + (seed % 8), terms = [];
      for (let b = 0; b < nb; b++) {
        const shape = SH[(seed + b) % SH.length], f0 = 30 * (1 + ((seed * 7 + b * 13) % 600));
        const viaType = (seed + b) % 2 === 0;
        if (viaType) terms.push({ role: 'band_type', index: b, value: b % 4, unit: 'none', shape });
        terms.push({ role: 'band_freq', index: b, value: (seed + b) % 3 === 0 ? f0 / 1000 : f0, unit: (seed + b) % 3 === 0 ? 'khz' : 'hz', shape: viaType ? undefined : shape });
        const g = ((seed * 5 + b * 3) % 25) - 12;
        terms.push(coef ? { role: 'band_gain', index: b, value: 10 ** (g / 20), unit: 'coef' } : { role: 'band_gain', index: b, value: g, unit: 'db' });
        terms.push((seed + b) % 4 === 1 ? { role: 'band_q', index: b, value: 0.25 + ((seed + b) % 7) / 3, unit: 'oct' } : { role: 'band_q', index: b, value: 0.4 + ((seed + 2 * b) % 9), unit: 'none' });
        if (b === 2) terms.push({ role: 'band_on', index: b, value: seed % 3 === 0 ? 0 : 1, unit: 'none' });
      }
      if (seed % 3 !== 2) terms.push({ role: 'hpf_freq', value: 20 + seed * 7, unit: 'hz' }, { role: 'hpf_slope', value: seed % 2 ? 24 : 12, unit: 'db' });
      if (seed % 4 === 1) terms.push({ role: 'lpf_freq', value: 9 + seed / 4, unit: 'khz' }, { role: 'lpf_slope', value: 12, unit: 'db' }, { role: 'lpf_on', value: seed % 8 === 1 ? 0 : 1, unit: 'none' });
      out.push({ kind: 'eq_response', family: 'eq', sampleRate: sr, points: 16 + ((seed * 37) % 200) % (CURVE_POINTS_MAX - 3 * nb - 12 - 16), terms, ...(seed % 5 === 4 ? { extents: { min: 40, max: 0.45 * sr } } : {}) });
    }
    return out;
  };
  const eqAsked = askCurves(eqReqs(false));
  const coefAsked = askCurves(eqReqs(true));
  it('eq_response: every request draws, 14 at each of the nine rates, and agrees', () => {
    assert.equal(eqAsked.reqs.length, 14 * RATES.length);
    assert.ok(eqAsked.answers.every((a) => a[0] === 0 && a[1] > 16), 'every EQ request draws');
    const r = curveAgainst(eqAsked, curvePoints, T.curvePointsEq);
    assert.deepEqual(r.failures.slice(0, 5), []);
    process.stderr.write(`curvePoints eq_response: largest gap ${r.worst} dB\n`);
  });
  it('eq_response with gains as linear factors agrees within the log10f bound', () => {
    const r = curveAgainst(coefAsked, curvePoints, T.curvePointsEqCoef);
    assert.deepEqual(r.failures.slice(0, 5), []);
    process.stderr.write(`curvePoints eq_response (coef gains): largest gap ${r.worst} dB\n`);
  });
  it('SABOTAGE eq_response: a Q off by less than the engine float narrowing (2^-25 relative) is red', () => {
    const q = (rq) => curvePoints(withTerm(rq, (t) => (t.role === 'band_q' && t.unit === 'none' ? { ...t, value: t.value * (1 + 2 ** -25) } : t)));
    assert.ok(curveAgainst(eqAsked, q, T.curvePointsEq).failures.length > eqAsked.reqs.length / 2);
  });
  it('SABOTAGE eq_response: a switched-off band drawn is red', () => {
    const on = (rq) => curvePoints({ ...rq, terms: rq.terms.filter((t) => t.role !== 'band_on' && t.role !== 'lpf_on') });
    assert.ok(curveAgainst(eqAsked, on, T.curvePointsEq).failures.length > 10);
  });

  const transferReqs = [];
  for (const sr of RATES) {
    for (const family of ['dynamics', 'limiter', 'deesser']) for (const [t, r, k, mk] of [[-20, 4, 6, 3], [-40.5, 1.5, 0, undefined], [-6, 20, 12, 0], [-30, 100, 24, -2], [-60, 1, 3, undefined]])
      transferReqs.push({ kind: 'transfer_curve', family, sampleRate: sr, points: [41, 97, 256, 2, 13][transferReqs.length % 5], terms: [
        { role: 'threshold', value: t, unit: ['dbfs', 'db', 'dbu', 'lufs'][transferReqs.length % 4] }, { role: 'ratio', value: r, unit: transferReqs.length % 2 ? 'coef' : 'ratio' },
        ...(k ? [{ role: 'knee', value: k, unit: 'db' }] : []), ...(mk === undefined ? [] : [{ role: 'makeup', value: mk, unit: 'db' }])] });
    for (const [t, r, rg, ks, ke] of [[-40, 4, -60, -46, -38], [-40, 16, -90, undefined, undefined], [-30, 2, -20, -33, -27], [-50, 100, 0, -50, -50], [-20, 1.2, -40, -10, -30], [-40, 4, -60, -45, -44]])
      transferReqs.push({ kind: 'transfer_curve', family: 'gate', sampleRate: sr, points: [41, 160, 256][transferReqs.length % 3], ...(transferReqs.length % 4 === 0 ? { extents: { min: -90, max: 6 } } : {}), terms: [
        { role: 'threshold', value: t, unit: 'db' }, { role: 'ratio', value: r, unit: 'ratio' }, { role: 'range', value: rg, unit: 'db' },
        ...(ks === undefined ? [] : [{ role: 'knee_start', value: ks, unit: 'db' }, { role: 'knee_end', value: ke, unit: 'db' }])] });
  }
  const trAsked = askCurves(transferReqs);
  it('transfer_curve: every family at every rate draws and agrees bit for bit', () => {
    assert.ok(trAsked.answers.every((a) => a[0] === 0 && a[1] >= 2));
    assert.deepEqual(curveAgainst(trAsked, curvePoints, T.curvePointsTransfer).failures.slice(0, 5), []);
  });
  it('SABOTAGE transfer_curve: a knee one float step off, and a gate knee range end one step off: each red', () => {
    const knee = (rq) => curvePoints(withTerm(rq, (t) => (t.role === 'knee' ? { ...t, value: nextFloat(t.value) } : t)));
    assert.ok(curveAgainst(trAsked, knee, T.curvePointsTransfer).failures.length > 10);
    const ks = (rq) => curvePoints(withTerm(rq, (t) => (t.role === 'knee_start' ? { ...t, value: nextFloat(t.value) } : t)));
    assert.ok(curveAgainst(trAsked, ks, T.curvePointsTransfer).failures.length > 10);
  });

  it('the ruled defaults agree: a band with no Q, the allpasses, a `-` band, a limiter with no ratio', () => {
    const reqs = [];
    for (const sr of RATES) {
      reqs.push({ kind: 'eq_response', family: 'eq', sampleRate: sr, points: 48, terms: [
        { role: 'band_freq', index: 0, value: 900, unit: 'hz', shape: 'bell' }, { role: 'band_gain', index: 0, value: 5, unit: 'db' },
        { role: 'band_freq', index: 1, value: 4000, unit: 'hz', shape: 'notch' },
        { role: 'band_type', index: 2, value: 3, unit: 'none', shape: 'allpass1' }, { role: 'band_freq', index: 2, value: 300, unit: 'hz' },
        { role: 'band_freq', index: 3, value: 7000, unit: 'hz', shape: 'allpass2' },
        { role: 'band_type', index: 4, value: 5, unit: 'none', shape: '-' }, { role: 'band_freq', index: 4, value: 60, unit: 'hz' },
        { role: 'band_gain', index: 4, value: 9, unit: 'db' }] });
      reqs.push({ kind: 'transfer_curve', family: 'limiter', sampleRate: sr, points: 41, terms: [
        { role: 'threshold', value: -6, unit: 'dbfs' }, { role: 'knee', value: 2, unit: 'db' }] });
    }
    const asked = askCurves(reqs);
    assert.ok(asked.answers.every((a) => a[0] === 0), 'every ruled request draws');
    assert.deepEqual(curveAgainst(asked, curvePoints, T.curvePointsEq).failures.slice(0, 5), []);
    const noDefault = (rq) => curvePoints(rq.family === 'eq' ? rq : { ...rq, terms: [...rq.terms, { role: 'ratio', value: 4, unit: 'ratio' }] });
    assert.ok(curveAgainst(asked, noDefault, T.curvePointsEq).failures.length > 0, 'SABOTAGE: a limiter at ratio 4 is red');
  });
  it('every refusal is the same code on both sides', () => {
    const bell = [{ role: 'band_freq', value: 1000, unit: 'hz', shape: 'bell' }, { role: 'band_gain', value: 3, unit: 'db' }, { role: 'band_q', value: 1, unit: 'none' }];
    const eq = { kind: 'eq_response', family: 'eq', sampleRate: 48000, points: 64, terms: bell };
    const comp = { kind: 'transfer_curve', family: 'dynamics', sampleRate: 48000, points: 41, terms: [{ role: 'threshold', value: -20, unit: 'db' }, { role: 'ratio', value: 4, unit: 'ratio' }] };
    const bad = [
      [{ ...eq, family: 'gate' }, 'kindUnknown'], [{ ...comp, family: 'eq' }, 'kindUnknown'],
      [{ ...eq, terms: [{ role: 'hpf_freq', value: 80, unit: 'hz' }, { role: 'hpf_slope', value: 1, unit: 'none' }] }, 'termMissing'],
      [{ ...comp, family: 'limiter', terms: [{ role: 'ratio', value: 4, unit: 'ratio' }] }, 'termMissing'],
      [{ ...eq, terms: [{ role: 'hpf_freq', value: 80, unit: 'hz' }, { role: 'hpf_slope', value: 18, unit: 'db' }] }, 'kindUnknown'],
      [{ ...eq, terms: bell.slice(1) }, 'termMissing'], [{ ...eq, terms: [{ ...bell[0], shape: undefined }, ...bell.slice(1)] }, 'termMissing'],
[{ ...eq, terms: [] }, 'termMissing'], [{ ...comp, terms: comp.terms.slice(0, 1) }, 'termMissing'],
      [{ ...comp, family: 'gate' }, 'termMissing'],
      [{ ...comp, family: 'gate', terms: [...comp.terms, { role: 'range', value: -60, unit: 'db' }, { role: 'knee_end', value: -30, unit: 'db' }] }, 'termMissing'],
      [{ ...eq, terms: [{ ...bell[0], unit: 'db' }] }, 'unitRefused'], [{ ...comp, terms: [...comp.terms, { role: 'makeup', value: 2, unit: 'coef' }] }, 'unitRefused'],
      [{ ...comp, terms: [{ role: 'threshold', value: NaN, unit: 'db' }, comp.terms[1]] }, 'valueNotFinite'], [{ ...comp, sampleRate: 0 }, 'valueNotFinite'],
      [{ ...comp, extents: { min: -80, max: Infinity } }, 'valueNotFinite'],
      [{ ...comp, points: 1 }, 'pointsRange'], [{ ...comp, points: CURVE_POINTS_MAX + 1 }, 'pointsRange'], [{ ...comp, extents: { min: -10, max: -10 } }, 'pointsRange'],
      [{ ...eq, extents: { min: 0, max: 20000 } }, 'pointsRange'], [{ ...eq, points: CURVE_POINTS_MAX - 2 }, 'pointsRange'],
      [{ ...eq, terms: [{ ...bell[0], index: Math.floor((CURVE_POINTS_MAX - 2) / 3) }] }, 'pointsRange'],
    ];
    const asked = askCurves(bad.map(([rq]) => rq));
    bad.forEach(([rq, code], i) => {
      assert.equal(CURVE_CODES[asked.answers[i][0]], code, `kernel, refusal ${i}`);
      assert.deepEqual(curvePoints(rq), { code }, `twin, refusal ${i}`);
    });
  });
});

describe('curvePoints marks the feedback corrector\'s bands and draws the user-only curve as omx_curve_points_marked', () => {
  const rowOfMarked = (rq) => [
    idx(CURVE_WORDS.displays, rq.kind), idx(CURVE_WORDS.families, rq.family), rq.sampleRate, rq.points, rq.extents ? 1 : 0,
    rq.extents?.min ?? 0, rq.extents?.max ?? 0, rq.userOnly ? 1 : 0, rq.terms.length,
    ...rq.terms.flatMap((t) => [idx(CURVE_WORDS.roles, t.role), t.index ?? 0, t.value, idx(CURVE_WORDS.units, t.unit),
      t.shape === undefined ? -1 : idx(CURVE_WORDS.shapes, t.shape), idx(CURVE_WORDS.origins, t.origin ?? 'user')]),
  ];
  const answerOfMarked = (a, userOnly) => {
    if ('code' in a) return [CURVE_CODES.indexOf(a.code), 0, 0];
    const out = [0, a.points.length, ...a.points.flatMap((p) => [p.x, p.y]), a.marks.length,
      ...a.marks.flatMap((m) => [m.freq, m.depthDb, m.q, idx(CURVE_WORDS.origins, m.origin)])];
    return userOnly ? [...out, ...a.userPoints.map((p) => p.y)] : out;
  };
  const reqs = [];
  for (const sr of RATES) for (let seed = 0; seed < 6; seed++) {
    const terms = [];
    const nb = 3 + (seed % 3);
    for (let b = 0; b < nb; b++) {
      const origin = b === 0 ? 'user' : ['fbs-fixed', 'fbs-live', 'hrp', 'room-mode', 'user'][(seed + b) % 5];
      const notch = origin !== 'user' && (seed + b) % 2 === 0;
      const f0 = 200 * (1 + ((seed * 3 + b * 7) % 40));
      terms.push({ role: 'band_freq', index: b, value: f0, unit: 'hz', shape: notch ? 'notch' : 'bell', origin });
      if (!notch) terms.push({ role: 'band_gain', index: b, value: origin === 'user' ? 4 - b : -6 - b, unit: 'db' });
      terms.push({ role: 'band_q', index: b, value: origin === 'user' ? 0.9 : 12 + b, unit: 'none' });
    }
    if (seed % 2) terms.push({ role: 'hpf_freq', value: 50, unit: 'hz' }, { role: 'hpf_slope', value: 24, unit: 'db' });
    reqs.push({ kind: 'eq_response', family: 'eq', sampleRate: sr, points: 40 + seed * 10, terms, userOnly: seed % 3 !== 2 });
  }
  const asked = ask('curve_points_marked', reqs.map(rowOfMarked));
  const agree = (twin) => {
    const failures = [];
    reqs.forEach((rq, i) => {
      const t = answerOfMarked(twin(rq), rq.userOnly), k = asked.answers[i];
      if (t.length !== k.length) return failures.push(`request ${i}: ${t.length} numbers, the kernel ${k.length}`);
      for (let j = 0; j < t.length; j++)
        if (!(Math.abs(t[j] - k[j]) <= Math.max(T.curvePointsEq.abs, 1e-15 * Math.abs(k[j])))) return failures.push(`request ${i} [${j}]: ${t[j]} vs ${k[j]}`);
    });
    return failures;
  };
  it('the points, every mark and the user-only curve agree at every rate', () => {
    assert.ok(asked.answers.every((a) => a[0] === 0));
    assert.ok(asked.answers.some((a) => a[2 + 2 * a[1]] >= 2), 'requests with two marks are in the grid');
    assert.deepEqual(agree(curvePoints).slice(0, 5), []);
  });
  it('the user-only curve is the curve over the user bands alone, and no request without origins has a mark', () => {
    for (const rq of reqs.filter((r) => r.userOnly)) {
      const a = curvePoints(rq);
      const mine = curvePoints({ ...rq, userOnly: false, terms: rq.terms.filter((t) => {
        const band = rq.terms.find((u) => u.role === 'band_freq' && (u.index ?? 0) === (t.index ?? 0));
        return !t.role.startsWith('band_') || (band?.origin ?? 'user') === 'user';
      }) });
      const at = new Map(mine.points.map((p) => [p.x, p.y]));
      for (const p of a.userPoints) if (at.has(p.x)) assert.ok(Math.abs(at.get(p.x) - p.y) <= 1e-12, `${p.x}`);
    }
    assert.deepEqual(curvePoints({ ...reqs[0], terms: reqs[0].terms.map(({ origin, ...t }) => t) }).marks, []);
  });
  it('every planter\'s origin is in the grid', () => {
    const seen = new Set(reqs.flatMap((rq) => rq.terms.map((t) => t.origin).filter(Boolean)));
    for (const o of CURVE_WORDS.origins) assert.ok(seen.has(o), o);
  });
  it('SABOTAGE: an origin dropped from the marks (fbs-live, then hrp) is red', () => {
    for (const gone of ['fbs-live', 'hrp']) {
      const drop = (rq) => { const a = curvePoints(rq); return 'code' in a ? a : { ...a, marks: a.marks.filter((m) => m.origin !== gone) }; };
      assert.ok(agree(drop).length > 0, gone);
    }
  });
});
