// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * EVERY TWIN AGREES WITH ITS KERNEL: the twin (dist/, the published code) and the omx-dsp kernel
 * (oracle/ts-twin-oracle.c) run one grid and must agree inside the tolerance test/tolerances.mjs states. A twin
 * whose SABOTAGE arm stays green would be decoration, so each has one. Moved here from the console with the twins.
 */
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { against, ask, oracle, oracleKernels } from '../oracle/ts-twin-oracle.mjs';
import { dbToLinearGain, fftSizeForRate, linearGainToDb, linearToDbFloored, octavesToQ } from '../dist/index.js';
import { TOLERANCES as T } from './tolerances.mjs';

const red = (asked, twin, tol) => against(asked, twin, tol).failures.length > 0;

describe('the oracle is here', () => {
  it('answers every kernel these twins need', () => {
    const k = oracleKernels();
    for (const n of ['lin_to_db', 'lin_to_db_d', 'db_to_lin', 'eq_octaves_q', 'rta_fft_size_for']) assert.ok(k.includes(n), n);
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
