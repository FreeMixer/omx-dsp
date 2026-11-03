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
  EQ_NOTCH_Q_RANGE,
  EQ_Q_RANGE,
  biquadMagnitudeDb,
  clampEqQFor,
  dbToLinearGain,
  fftSizeForRate,
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
    for (const n of ['lin_to_db', 'lin_to_db_d', 'db_to_lin', 'eq_octaves_q', 'rta_fft_size_for', 'eq_design', 'eq_lv2_clamp_q', 'eq_band_magnitude_db', 'eq_magnitude_sum_db']) assert.ok(k.includes(n), n);
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
