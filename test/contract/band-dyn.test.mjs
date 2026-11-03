// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The band-dynamics kernel: its travels hold their defaults, the travels it shares with the de-esser
// and the compressor follow them, and the C limits header and the json render carry every unit of it.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { loadData, resolveData } from './oc.mjs';
import { validateData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { limitUnits } from './oc.mjs';
import { DATA, UNION, editData, scratchData } from './helpers.mjs';

const TRAVELS = ['BAND_DYN_THRESHOLD_RANGE', 'BAND_DYN_RATIO_RANGE', 'BAND_DYN_RANGE_RANGE', 'BAND_DYN_ATTACK_RANGE', 'BAND_DYN_RELEASE_RANGE'];
const resolved = (dir = UNION) => resolveData(loadData(dir));
const header = (r) => renderC(r).find((f) => f.path === 'omxcontract/omx_contract_limits.h').text;

test('every band-dynamics travel holds its default inside its range, and its range is not empty', () => {
  const r = resolved();
  for (const name of TRAVELS) {
    const t = r.get(name).value;
    assert.ok(t.min < t.max, `${name}: min ${t.min} is below max ${t.max}`);
    assert.ok(t.default >= t.min && t.default <= t.max, `${name}: default ${t.default} is inside [${t.min}, ${t.max}]`);
  }
});

test('the band reads the de-esser and the compressor where the spec says, and a move of theirs moves the band', () => {
  const dir = scratchData();
  editData(dir, 'kernels/deesser.json', (d, at) => { at('DEESS_RANGE_RANGE').travel.min = -20; });
  editData(dir, 'kernels/comp.json', (d, at) => { at('COMP_LIMITS.releaseMs').max = 2000; });
  const r = resolved(dir);
  assert.equal(r.get('BAND_DYN_RANGE_RANGE').value.min, -20, 'the range floor is the de-esser\'s');
  assert.equal(r.get('BAND_DYN_RELEASE_RANGE').value.max, 2000, 'the release ceiling is the compressor\'s');
  assert.equal(r.get('BAND_DYN_RELEASE_RANGE').value.default, 100, 'the default is the band\'s own, not read');
});

test('the attack floor is the detector constant, 0.5 ms, and the knee is 6 dB as omx-dsp\'s test spells it', () => {
  const r = resolved();
  assert.equal(r.get('DETECTOR_OVERSAMPLE_AUTO_MS').value, 0.5);
  assert.equal(r.get('BAND_DYN_ATTACK_RANGE').value.min, 0.5);
  assert.equal(r.get('BAND_DYN_KNEE_DB').value, 6);
});

test('the c limits header defines every band-dynamics unit with its value, and the limit units carry them', () => {
  const text = header(resolved());
  for (const line of [
    '#define OMX_BAND_DYN_KNEE_DB 6',
    '#define OMX_BAND_DYN_MAX_BANDS 9',
    '#define OMX_BAND_DYN_RANGE_RANGE_MIN -24',
    '#define OMX_BAND_DYN_RANGE_RANGE_MAX 12',
    '#define OMX_BAND_DYN_ATTACK_RANGE_MIN 0.5f',
    '#define OMX_BAND_DYN_RELEASE_RANGE_MAX 3000',
    '#define OMX_BAND_DYN_RATIO_RANGE_DEFAULT 2',
    '#define OMX_BAND_DYN_MODES_DEFAULT OMX_BAND_DYN_MODES_ABOVE',
  ]) assert.ok(text.includes(`${line}\n`), `the header carries ${line}`);
  const keys = new Set(limitUnits(text).filter((u) => u.frame === undefined).map((u) => u.key));
  for (const k of ['OMX_BAND_DYN_KNEE_DB', 'OMX_BAND_DYN_RANGE_RANGE_MIN', 'OMX_BAND_DYN_ATTACK_RANGE_MIN_DOUBLE', 'OMX_BAND_DYN_MODES_COUNT']) {
    assert.ok(keys.has(k), `${k} is a limit unit of its own`);
  }
});

test('the json render carries every band-dynamics item with its resolved value', () => {
  const items = JSON.parse(renderJson(resolved(), '0.0.0')[0].text).items;
  assert.deepEqual(items.BAND_DYN_RATIO_RANGE.value, { min: 1, max: 20, step: 0.1, unit: '', default: 2, defaultFrom: 'desk' });
  assert.equal(items.BAND_DYN_MAX_BANDS.value, 9);
  assert.deepEqual(items.BAND_DYN_MODES.value, ['above', 'below']);
});

test('a band default moved outside its range is refused, naming the travel', () => {
  const dir = scratchData();
  editData(dir, 'kernels/band_dyn.json', (d, at) => { at('BAND_DYN_RANGE_RANGE').travel.default = 20; });
  const p = validateData(loadData(dir));
  assert.ok(p.some((x) => x.item === 'BAND_DYN_RANGE_RANGE' && x.rule === 'default-inside'), 'default-inside names the travel');
});
