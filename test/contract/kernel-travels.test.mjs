// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * 2.14.0 (#44): the kernel travels and constants the console engine still typed as literals, declared with
 * the values it uses. MACHINE_BANDS_RANGE and HRP_TIER_CROSSOVER_HZ, typed there too, have no reader in the
 * console or in omx-dsp, so they are not declared. CONSUMER is what the console engine's declarations hold, value for value, and the C
 * lines its generated limits header carries for them; where omx-dsp states the same number in a header of
 * its own, the citation says where. A declared value that drifts from CONSUMER turns this file red. FBS_AUTO_BANDS_RANGE
 * and HRP_AUTO_BANDS_RANGE, the two band caps that read EQ_MAX_BANDS, went to the console's declarations with the other
 * rows no omx-dsp source reads (f8b816e), and the arms over them with them.
 */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, scratchData } from './helpers.mjs';

const json = (dir = UNION) => JSON.parse(renderJson(resolveData(loadData(dir)), '0.0.0')[0].text);
const header = (dir = UNION) => renderC(resolveData(loadData(dir))).find((f) => f.path === 'omxcontract/omx_contract_limits.h').text;

/**
 * name -> [the file declaring it, the value the console engine uses]. The five fbs travels, FBS_DEFAULT_HOLD_SECONDS,
 * FBS_DEFAULT_SENSITIVITY and HRP_AUTO_BANDS_RANGE are not here: no omx-dsp source reads them, and f8b816e left them
 * to the console's own declarations when the fbs and hrp files moved here.
 */
const CONSUMER = {
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
  assert.equal(items.HRP_DEFAULT_MAX_AUTO_BANDS.value, 8);
});

test('the kernels list their controls; align and idelay are kernels of one travel each', () => {
  const k = json().kernels;
  // hrp's autoBands went to the console (f8b816e); hrp's family and band budget came in fd858d5 (#80); the live shells'
  // mode (both) and omx-fbs's arming came with the shells (FreeMixer/omx-plugins#330)
  assert.deepEqual(k.fbs.controls.map((c) => `${c.name}:${c.kind}`), ['mode:choice', 'pool:choice', 'hold:travel', 'bands:travel', 'merge:travel', 'cooldown:travel']);
  assert.deepEqual(k.hrp.controls.map((c) => c.name), ['mode', 'family', 'amount', 'bands']);
  assert.deepEqual(k.balance.controls.map((c) => `${c.name}:${c.global}`), ['pan:PAN_RANGE', 'divergence:DIVERGENCE_RANGE']);
  assert.deepEqual(k.align.controls, [{ name: 'inputAlignDelay', kind: 'travel', global: 'INPUT_ALIGN_DELAY_RANGE' }]);
  assert.deepEqual(k.idelay.controls, [{ name: 'outputDelay', kind: 'travel', global: 'OUTPUT_DELAY_RANGE' }]);
});

test('the C render carries the console engine\'s values for these items', () => {
  // the render's own numbers, read off its defines: the bytes are omx-contract's renderer's to hold, and this tree
  // restates no define of the render (tools/contract-single-source.sh)
  const defines = new Map([...header().matchAll(/^#define OMX_(\w+) (-?[0-9.eE+-]+)f?$/gm)].map((m) => [m[1], Number(m[2])]));
  let n = 0;
  for (const [name, [, value]] of Object.entries(CONSUMER)) {
    const want = typeof value === 'object' ? Object.entries(value).filter(([k]) => k !== 'unit').map(([k, v]) => [`${name}_${k.toUpperCase()}`, v]) : [[name, value]];
    for (const [macro, v] of want) { assert.equal(defines.get(macro), v, macro); n++; }
  }
  assert.equal(n, 14, 'every value the console engine uses is read off the render');
});

test('perturbation: move each value, the json and C renders follow and render --check goes red', () => {
  const arms = [
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
