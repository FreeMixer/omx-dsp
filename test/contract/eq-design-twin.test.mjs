// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The EQ section design's shared data (#86): the biquad kinds BIQUAD_KINDS (primitives.json) and the low-pass
 * hand-over table LOWPASS_HANDOVER (eq.json). omx-dsp holds both today in include/omxdsp/omx_eq_design.h
 * (enum omx_eq_kind, :24-33, and OMX_EQ_LOWPASS_HANDOVER_*, :44-60) and the console engine's core holds the same
 * in eq.ts (BiquadKind, :425-434, and the hand-over constants and band ends, :496-511). The values below are the
 * ones both sides hold; a drift in the data turns these red, and a moved value moves every render and turns
 * `render --check` red.
 */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, relOf, scratchData } from './helpers.mjs';

const json = (dir = UNION) => JSON.parse(renderJson(resolveData(loadData(dir)), '0.0.0')[0].text);
const header = (dir = UNION) => renderC(resolveData(loadData(dir)))[0].text;

// omx-dsp's enum omx_eq_kind order, which is core's BiquadKind order.
const KINDS = ['peaking', 'lowShelf', 'highShelf', 'highpass', 'lowpass', 'notch', 'bandpass', 'allpass1', 'allpass2'];
// omx-dsp's OMX_EQ_LOWPASS_HANDOVER_* as written there, parsed as doubles.
const HANDOVER = {
  qHi: 0.70710678118654752, qLo: 0.54119610014619698, qPeak: 0.9,
  loAtHiQ: 0.6, hiAtHiQ: 0.85, loAtLoQ: 0.8, hiAtLoQ: 0.9, hiAtPeakQ: 0.8,
};

test('the biquad kinds are the set BIQUAD_KINDS, in the order omx-dsp numbers them, with no come-up id', () => {
  const k = json().items.BIQUAD_KINDS;
  assert.deepEqual(k.value, KINDS);
  assert.equal(k.default, undefined);
  assert.match(header(), /^  OMX_BIQUAD_KINDS_PEAKING = 0,$/m);
  assert.match(header(), /^  OMX_BIQUAD_KINDS_ALLPASS2 = 8,$/m);
  assert.match(header(), /^#define OMX_BIQUAD_KINDS_COUNT 9u$/m);
});

test('the hand-over table holds omx-dsp\'s eight numbers, the high Q end being the Butterworth Q', () => {
  const j = json();
  assert.deepEqual(j.items.LOWPASS_HANDOVER.value, HANDOVER);
  assert.equal(j.items.LOWPASS_HANDOVER.value.qHi, j.items.BUTTERWORTH_Q.value);
  assert.match(header(), /^#define OMX_LOWPASS_HANDOVER_Q_LO_DOUBLE 0\.541196100146197$/m);
  assert.match(header(), /^#define OMX_LOWPASS_HANDOVER_HI_AT_PEAK_Q_DOUBLE 0\.8$/m);
});

test('the renders never spell the OMX_EQ_* names omx-dsp defines today', () => {
  assert.doesNotMatch(header(), /OMX_EQ_LOWPASS_HANDOVER_|OMX_EQ_(PEAKING|LOWSHELF|HIGHSHELF|HIGHPASS|LOWPASS|NOTCH|BANDPASS|ALLPASS1|ALLPASS2)\b/);
});

test('perturbation: move a hand-over corner, the header and the json follow and --check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/eq.json', (d, at) => { at('LOWPASS_HANDOVER').value.hiAtHiQ = 0.86; });
  assert.match(header(dir), /^#define OMX_LOWPASS_HANDOVER_HI_AT_HI_Q_DOUBLE 0\.86$/m);
  assert.equal(json(dir).items.LOWPASS_HANDOVER.value.hiAtHiQ, 0.86);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
  assert.equal(main(['render', '--target', 'json', '--out', `${RENDERS}/share/omx-contract`, '--check', '--data', dir]), 1);
});

test('perturbation: swap two kinds, the integers move and --check goes red', () => {
  const dir = scratchData();
  editData(dir, relOf('BIQUAD_KINDS').slice('data/'.length), (d) => { const ids = d.BIQUAD_KINDS.ids; [ids[3], ids[4]] = [ids[4], ids[3]]; });
  assert.match(header(dir), /^  OMX_BIQUAD_KINDS_LOWPASS = 3,$/m);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
});
