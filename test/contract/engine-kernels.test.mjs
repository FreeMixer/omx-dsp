// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// 2.6.0: the sample-rate converter's design constants (the src kernel) and the generators' kind
// and waveform sets (the generator kernel), the kernels omx-dsp takes over from the engine. The engine's own
// defines at 6ee8a9bde101 are frozen under test/fixtures/openmixer-6ee8a9bde101/; their values are the render's.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, scratchData } from './helpers.mjs';

const resolved = (dir = UNION) => resolveData(loadData(dir));
const header = (dir) => renderC(resolved(dir))[0].text;
const json = (dir) => JSON.parse(renderJson(resolved(dir), '0.0.0')[0].text);
const engine = new Map([...readFileSync(join(ROOT, 'test/fixtures/openmixer-6ee8a9bde101/engine_kernel_defines.h'), 'utf8')
  .matchAll(/^#define (OMX_\w+) ([\d.]+)u?$/gm)].map((m) => [m[1], Number(m[2])]));
/** The value the c render gives a name: an enumerator's or a define's. */
const cValue = (h, name) => {
  const m = h.match(new RegExp(`^(?:#define ${name} |  ${name} = )([\\d.]+)[uf]?,?$`, 'm'));
  return m ? Number(m[1]) : undefined;
};
const PAIRS = [
  ['OMX_SRC_TAPS', 'OMX_SRC_TAPS'],
  ['OMX_SRC_STOPBAND_DB', 'OMX_SRC_STOPBAND_DB'],
  ['OMX_SRC_ROLLOFF', 'OMX_SRC_ROLLOFF_DOUBLE'],
  ['OMX_SRC_MAX_L', 'OMX_SRC_MAX_L'],
  ['OMX_GEN_OSC', 'OMX_GENERATOR_KINDS_OSCILLATOR'],
  ['OMX_GEN_WHITE', 'OMX_GENERATOR_KINDS_WHITE'],
  ['OMX_GEN_PINK', 'OMX_GENERATOR_KINDS_PINK'],
  ['OMX_WAVE_SINE', 'OMX_WAVEFORMS_SINE'],
  ['OMX_WAVE_SQUARE', 'OMX_WAVEFORMS_SQUARE'],
  ['OMX_WAVE_SAW', 'OMX_WAVEFORMS_SAW'],
  ['OMX_WAVE_TRIANGLE', 'OMX_WAVEFORMS_TRIANGLE'],
];
/** The engine's names whose value the render does not give its contract spelling. */
const disagree = (h) => PAIRS.filter(([e, c]) => engine.get(e) !== cValue(h, c)).map(([e, c]) => `${e} ${engine.get(e)}, ${c} ${cValue(h, c)}`);

test('src: four constants in data/kernels/src.json, the design the converter runs at', () => {
  const items = json().items;
  for (const n of ['SRC_TAPS', 'SRC_STOPBAND_DB', 'SRC_ROLLOFF', 'SRC_MAX_L']) assert.equal(items[n].rel, 'data/kernels/src.json', n);
  assert.deepEqual(json().kernels.src.controls, []);
});

test('generator: two choices, kind (GENERATOR_KINDS) and waveform (WAVEFORMS), in surface order, then the oscillator\'s frequency travel (2.14.0)', () => {
  assert.deepEqual(json().kernels.generator.controls, [
    { name: 'kind', kind: 'choice', global: 'GENERATOR_KINDS' },
    { name: 'waveform', kind: 'choice', global: 'WAVEFORMS' },
    { name: 'oscillatorFreq', kind: 'travel', global: 'OSCILLATOR_FREQ_RANGE' },
  ]);
  assert.deepEqual(json().items.GENERATOR_KINDS.value, ['oscillator', 'white', 'pink']);
  assert.deepEqual(json().items.WAVEFORMS.value, ['sine', 'square', 'saw', 'triangle']);
});

test('every value the engine\'s converter and generators define is the c render\'s', () => {
  assert.equal(engine.size, PAIRS.length, 'the frozen engine definitions: four src constants, three kinds, four waveforms');
  assert.deepEqual(disagree(header()), []);
});

test('the 2.6.0 C render keeps every 2.5.0 line, byte for byte and in order, and adds only the src constants and the generator sets', () => {
  const old = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.5.0/omx_contract_limits.h'), 'utf8').split('\n');
  // the released 2.6.0 header: the release these kernels came in (later releases add their own, test/gate-key.test.mjs and test/drive-oversample.test.mjs)
  const now = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.6.0/omx_contract_limits.h'), 'utf8').split('\n');
  let i = 0;
  const added = [];
  for (const line of now) {
    if (i < old.length && line === old[i]) i++;
    else added.push(line);
  }
  assert.equal(i, old.length, `2.5.0 line ${i + 1} is not in the render: ${old[i]}`);
  assert.deepEqual(added, [
    '#define OMX_SRC_MAX_L 1024',
    '#define OMX_SRC_ROLLOFF 0.92f',
    '#define OMX_SRC_ROLLOFF_DOUBLE 0.92',
    '#define OMX_SRC_STOPBAND_DB 96',
    '#define OMX_SRC_TAPS 64',
    'enum omx_generator_kinds {',
    '  OMX_GENERATOR_KINDS_OSCILLATOR = 0,',
    '  OMX_GENERATOR_KINDS_WHITE = 1,',
    '  OMX_GENERATOR_KINDS_PINK = 2,',
    '};',
    '#define OMX_GENERATOR_KINDS_COUNT 3u',
    'enum omx_waveforms {',
    '  OMX_WAVEFORMS_SINE = 0,',
    '  OMX_WAVEFORMS_SQUARE = 1,',
    '  OMX_WAVEFORMS_SAW = 2,',
    '  OMX_WAVEFORMS_TRIANGLE = 3,',
    '};',
    '#define OMX_WAVEFORMS_COUNT 4u',
  ]);
});

test('perturbation: a moved tap count or a reordered waveform set disagrees with the engine, and --check goes red', () => {
  const taps = scratchData();
  editData(taps, 'kernels/src.json', (d, at) => { at('SRC_TAPS').value = 32; });
  assert.deepEqual(disagree(header(taps)), ['OMX_SRC_TAPS 64, OMX_SRC_TAPS 32']);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', taps]), 1);
  const order = scratchData();
  editData(order, 'kernels/generator.json', (d, at) => { at('WAVEFORMS').ids.reverse(); });
  assert.deepEqual(disagree(header(order)), [
    'OMX_WAVE_SINE 0, OMX_WAVEFORMS_SINE 3',
    'OMX_WAVE_SQUARE 1, OMX_WAVEFORMS_SQUARE 2',
    'OMX_WAVE_SAW 2, OMX_WAVEFORMS_SAW 1',
    'OMX_WAVE_TRIANGLE 3, OMX_WAVEFORMS_TRIANGLE 0',
  ]);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', order]), 1);
});
