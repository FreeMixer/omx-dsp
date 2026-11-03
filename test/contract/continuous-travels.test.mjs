// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The plugin faces' travels in data/primitives.json hold plain numbers, because omx-plugins reads the
// data as written; these tests hold each number to the travel it restates.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { loadData, resolveData } from './oc.mjs';
import { ROOT, UNION, editData, relOf, scratchData } from './helpers.mjs';

/** Each continuous field that is not its source's field with step 0, as a sentence. */
function continuousProblems(resolved) {
  const bad = [];
  for (const [face, source] of [['GATE_CONTINUOUS_TRAVELS', 'GATE_LIMITS'], ['COMP_CONTINUOUS_TRAVELS', 'COMP_LIMITS']]) {
    const f = resolved.get(face).value;
    const s = resolved.get(source).value;
    for (const [field, t] of Object.entries(f)) {
      if (!s[field]) bad.push(`${face}.${field}: ${source} has no such field`);
      else if (JSON.stringify(t) !== JSON.stringify({ ...s[field], step: 0 })) bad.push(`${face}.${field} is not ${source}.${field} with step 0`);
    }
  }
  return bad;
}

test('each continuous travel is its stepped source with step 0, every other number equal', () => {
  const resolved = resolveData(loadData(UNION));
  assert.deepEqual(continuousProblems(resolved), []);
  assert.deepEqual(Object.keys(resolved.get('GATE_CONTINUOUS_TRAVELS').value), ['thresholdDb', 'rangeDb', 'releaseMs', 'ratio']);
  assert.deepEqual(Object.keys(resolved.get('COMP_CONTINUOUS_TRAVELS').value), ['thresholdDb', 'releaseMs']);
});

test('a source moved without its continuous form, or a continuous form given a step, is caught', () => {
  const moved = scratchData();
  editData(moved, 'kernels/gate.json', (d, at) => { at('GATE_LIMITS.thresholdDb').default = -30; });
  assert.deepEqual(continuousProblems(resolveData(loadData(moved))), ['GATE_CONTINUOUS_TRAVELS.thresholdDb is not GATE_LIMITS.thresholdDb with step 0']);
  const stepped = scratchData();
  editData(stepped, relOf('COMP_CONTINUOUS_TRAVELS').slice('data/'.length), (d) => { d.COMP_CONTINUOUS_TRAVELS.fields.releaseMs.step = 10; });
  assert.deepEqual(continuousProblems(resolveData(loadData(stepped))), ['COMP_CONTINUOUS_TRAVELS.releaseMs is not COMP_LIMITS.releaseMs with step 0']);
});

test('the trim travel is the TRIM_RANGE omx-dsp\'s header spells, coming up at 0 dB', () => {
  const header = readFileSync(join(ROOT, 'test/fixtures/limits-0.1.5-8/omx_contract_limits.h'), 'utf8');
  const define = (name) => Number(new RegExp(`^#define ${name} (\\S+?)f?$`, 'm').exec(header)[1]);
  const t = resolveData(loadData(UNION)).get('TRIM_TRAVELS').value.trimDb;
  assert.deepEqual(t, {
    min: define('OMX_TRIM_RANGE_MIN_DB'), max: define('OMX_TRIM_RANGE_MAX_DB'), step: define('OMX_TRIM_RANGE_STEP_DB'),
    unit: 'dB', default: 0, defaultFrom: 'desk',
  });
});
