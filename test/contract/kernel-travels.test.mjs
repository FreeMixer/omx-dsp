// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * 2.14.0 (#44): the kernel travels and constants the console engine still typed as literals, declared with
 * the values it uses. MACHINE_BANDS_RANGE and HRP_TIER_CROSSOVER_HZ, typed there too, have no reader in the
 * console or in omx-dsp, so they are not declared. CONSUMER is what the console engine's declarations hold, value for value, and the C
 * lines its generated limits header carries for them; where omx-dsp states the same number in a header of
 * its own, the citation says where. A declared value that drifts from CONSUMER turns this file red.
 */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { renderTs } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, scratchData } from './helpers.mjs';

const json = (dir = UNION) => JSON.parse(renderJson(resolveData(loadData(dir)), '0.0.0')[0].text);
const header = (dir = UNION) => renderC(resolveData(loadData(dir))).find((f) => f.path === 'omxcontract/omx_contract_limits.h').text;

/** name -> [the file declaring it, the value the console engine uses]. */
const CONSUMER = {
  FBS_AUTO_BANDS_RANGE: ['data/kernels/fbs.json', { min: 1, max: 24, step: 1, unit: '' }],
  FBS_SENSITIVITY_RANGE: ['data/kernels/fbs.json', { min: 0, max: 1, step: 0.05, unit: '' }],
  FBS_MERGE_CENTS_RANGE: ['data/kernels/fbs.json', { min: 0, max: 600, step: 10, unit: 'cents' }],
  FBS_COOLDOWN_MINUTES_RANGE: ['data/kernels/fbs.json', { min: 0, max: 120, step: 1, unit: 'min' }],
  FBS_HOLD_SECONDS_RANGE: ['data/kernels/fbs.json', { min: 1, max: 300, step: 1, unit: 's' }],
  FBS_DEFAULT_HOLD_SECONDS: ['data/kernels/fbs.json', 5],
  FBS_DEFAULT_SENSITIVITY: ['data/kernels/fbs.json', 0],
  HRP_AUTO_BANDS_RANGE: ['data/kernels/hrp.json', { min: 1, max: 24, step: 1, unit: '' }],
  // omx-dsp include/omxdsp/analysis/omx_hrp_pitch.h OMX_HRP_HARMONICS 12
  HRP_HARMONIC_COUNT: ['data/kernels/hrp.json', 12],
  // omx-dsp include/omxdsp/omx_oversampler.h OMX_OVS_LATENCY_4X 72
  DETECTOR_OVERSAMPLE_LATENCY_SAMPLES: ['data/kernels/comp.json', 72],
  DIVERGENCE_RANGE: ['data/kernels/balance.json', { min: 0, max: 1, step: 0, unit: '' }],
  OSCILLATOR_FREQ_RANGE: ['data/kernels/generator.json', { min: 20, max: 20000, step: 10, unit: 'Hz' }],
  // omx-dsp include/omxdsp/omx_align.h OMX_ALIGN_MAX_MS 100
  INPUT_ALIGN_DELAY_RANGE: ['data/kernels/align.json', { min: 0, max: 100, step: 0.0001, unit: 'ms' }],
  OUTPUT_DELAY_RANGE: ['data/kernels/idelay.json', { min: 0, max: 1000, step: 1, unit: 'ms' }],
};

/** The C lines the console engine's generated limits header carries for these items, byte for byte. */
const CONSUMER_C = `#define OMX_DETECTOR_OVERSAMPLE_LATENCY_SAMPLES 72
#define OMX_DIVERGENCE_RANGE_MAX 1
#define OMX_DIVERGENCE_RANGE_MIN 0
#define OMX_DIVERGENCE_RANGE_STEP 0
#define OMX_FBS_AUTO_BANDS_RANGE_MAX 24
#define OMX_FBS_AUTO_BANDS_RANGE_MIN 1
#define OMX_FBS_AUTO_BANDS_RANGE_STEP 1
#define OMX_FBS_COOLDOWN_MINUTES_RANGE_MAX 120
#define OMX_FBS_COOLDOWN_MINUTES_RANGE_MIN 0
#define OMX_FBS_COOLDOWN_MINUTES_RANGE_STEP 1
#define OMX_FBS_DEFAULT_HOLD_SECONDS 5
#define OMX_FBS_DEFAULT_SENSITIVITY 0
#define OMX_FBS_HOLD_SECONDS_RANGE_MAX 300
#define OMX_FBS_HOLD_SECONDS_RANGE_MIN 1
#define OMX_FBS_HOLD_SECONDS_RANGE_STEP 1
#define OMX_FBS_MERGE_CENTS_RANGE_MAX 600
#define OMX_FBS_MERGE_CENTS_RANGE_MIN 0
#define OMX_FBS_MERGE_CENTS_RANGE_STEP 10
#define OMX_FBS_SENSITIVITY_RANGE_MAX 1
#define OMX_FBS_SENSITIVITY_RANGE_MIN 0
#define OMX_FBS_SENSITIVITY_RANGE_STEP 0.05f
#define OMX_FBS_SENSITIVITY_RANGE_STEP_DOUBLE 0.05
#define OMX_HRP_AUTO_BANDS_RANGE_MAX 24
#define OMX_HRP_AUTO_BANDS_RANGE_MIN 1
#define OMX_HRP_AUTO_BANDS_RANGE_STEP 1
#define OMX_HRP_HARMONIC_COUNT 12
#define OMX_INPUT_ALIGN_DELAY_RANGE_MAX 100
#define OMX_INPUT_ALIGN_DELAY_RANGE_MIN 0
#define OMX_INPUT_ALIGN_DELAY_RANGE_STEP 0.0001f
#define OMX_INPUT_ALIGN_DELAY_RANGE_STEP_DOUBLE 0.0001
#define OMX_OSCILLATOR_FREQ_RANGE_MAX 20000
#define OMX_OSCILLATOR_FREQ_RANGE_MIN 20
#define OMX_OSCILLATOR_FREQ_RANGE_STEP 10
#define OMX_OUTPUT_DELAY_RANGE_MAX 1000
#define OMX_OUTPUT_DELAY_RANGE_MIN 0
#define OMX_OUTPUT_DELAY_RANGE_STEP 1`.split('\n');

test('each item is declared once, where it belongs, with the value the console engine uses', () => {
  const items = json().items;
  for (const [name, [rel, value]] of Object.entries(CONSUMER)) {
    assert.ok(items[name], `${name} is declared`);
    assert.equal(items[name].rel, rel, `${name} lives in ${rel}`);
    assert.deepEqual(items[name].value, value, name);
  }
});

test('the two items the console declares for no reader are not declared here', () => {
  const items = json().items;
  for (const n of ['MACHINE_BANDS_RANGE', 'HRP_TIER_CROSSOVER_HZ']) assert.equal(items[n], undefined, n);
});

test('the travels carry no come-up value: the console engine states each come-up as its own item', () => {
  const items = json().items;
  for (const [name, [, value]] of Object.entries(CONSUMER)) if (typeof value === 'object') assert.equal('default' in items[name].value, false, name);
  assert.equal(items.FBS_DEFAULT_MAX_AUTO_BANDS.value, 6);
  assert.equal(items.HRP_DEFAULT_MAX_AUTO_BANDS.value, 8);
});

test('the two band caps read their ceiling from EQ_MAX_BANDS', () => {
  const d = loadData(UNION);
  for (const n of ['FBS_AUTO_BANDS_RANGE', 'HRP_AUTO_BANDS_RANGE']) assert.deepEqual(d.items.get(n).item.travel.max, { ref: 'EQ_MAX_BANDS' }, n);
});

test('the kernels list the new controls, appended; align and idelay are kernels of one travel each', () => {
  const k = json().kernels;
  assert.deepEqual(k.fbs.controls.map((c) => `${c.name}:${c.global}`), ['autoBands:FBS_AUTO_BANDS_RANGE', 'sensitivity:FBS_SENSITIVITY_RANGE',
    'mergeCents:FBS_MERGE_CENTS_RANGE', 'cooldownMinutes:FBS_COOLDOWN_MINUTES_RANGE', 'holdSeconds:FBS_HOLD_SECONDS_RANGE']);
  assert.deepEqual(k.hrp.controls.map((c) => `${c.name}:${c.global}`), ['amount:HRP_AMOUNT_RANGE', 'autoBands:HRP_AUTO_BANDS_RANGE']);
  assert.deepEqual(k.balance.controls.map((c) => `${c.name}:${c.global}`), ['pan:PAN_RANGE', 'divergence:DIVERGENCE_RANGE']);
  assert.deepEqual(k.align.controls, [{ name: 'inputAlignDelay', kind: 'travel', global: 'INPUT_ALIGN_DELAY_RANGE' }]);
  assert.deepEqual(k.idelay.controls, [{ name: 'outputDelay', kind: 'travel', global: 'OUTPUT_DELAY_RANGE' }]);
});

test('the C render carries the console engine\'s lines for these items, byte for byte', () => {
  const lines = new Set(header().split('\n'));
  assert.deepEqual(CONSUMER_C.filter((l) => !lines.has(l)), []);
  assert.equal(CONSUMER_C.length, 36);
});

test('perturbation: move each value, the json and C renders follow and render --check goes red', () => {
  const arms = [
    ['FBS_SENSITIVITY_RANGE', 'kernels/fbs.json', (it) => { it.travel.step = 0.1; }, /^#define OMX_FBS_SENSITIVITY_RANGE_STEP 0\.1f$/m],
    ['FBS_DEFAULT_HOLD_SECONDS', 'kernels/fbs.json', (it) => { it.value = 6; }, /^#define OMX_FBS_DEFAULT_HOLD_SECONDS 6$/m],
    ['DETECTOR_OVERSAMPLE_LATENCY_SAMPLES', 'kernels/comp.json', (it) => { it.value = 71; }, /^#define OMX_DETECTOR_OVERSAMPLE_LATENCY_SAMPLES 71$/m],
    ['INPUT_ALIGN_DELAY_RANGE', 'kernels/align.json', (it) => { it.travel.max = 99; }, /^#define OMX_INPUT_ALIGN_DELAY_RANGE_MAX 99$/m],
    ['OUTPUT_DELAY_RANGE', 'kernels/idelay.json', (it) => { it.travel.max = 999; }, /^#define OMX_OUTPUT_DELAY_RANGE_MAX 999$/m],
    ['OSCILLATOR_FREQ_RANGE', 'kernels/generator.json', (it) => { it.travel.step = 5; }, /^#define OMX_OSCILLATOR_FREQ_RANGE_STEP 5$/m],
    ['DIVERGENCE_RANGE', 'kernels/balance.json', (it) => { it.travel.max = 2; }, /^#define OMX_DIVERGENCE_RANGE_MAX 2$/m],
  ];
  for (const [name, file, move, line] of arms) {
    const dir = scratchData();
    editData(dir, file, (d, at) => move(at(name)));
    assert.notDeepEqual(json(dir).items[name].value, CONSUMER[name][1], `${name}: the moved value reads back`);
    assert.match(header(dir), line, name);
    assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1, `${name}: c --check`);
    assert.equal(main(['render', '--target', 'json', '--out', `${RENDERS}/share/omx-contract`, '--check', '--data', dir]), 1, `${name}: json --check`);
  }
});

test('perturbation: EQ_MAX_BANDS moves the two ceilings that read it, in every render', () => {
  const dir = scratchData();
  editData(dir, 'kernels/eq.json', (d, at) => { at('EQ_MAX_BANDS').value = 20; });
  const items = json(dir).items;
  for (const n of ['FBS_AUTO_BANDS_RANGE', 'HRP_AUTO_BANDS_RANGE']) assert.equal(items[n].value.max, 20, n);
  assert.match(renderTs(resolveData(loadData(dir)))[0].text, /^export const HRP_AUTO_BANDS_RANGE = \{[^}]*"max": 20/m);
});
