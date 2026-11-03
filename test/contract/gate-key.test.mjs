// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// 2.7.0 (#33): the keyed gate's detector filter edges, hpHz and lpHz, are travel controls of the gate in their own
// table, GATE_KEY_LIMITS, with the console's values (its GATE_KEY_LIMITS: 0..20000 Hz, step 1,
// no come-up value). The released 2.7.0 header keeps every 2.6.0 line and adds only their MIN and MAX.
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

const CONSOLE = { min: 0, max: 20000, step: 1, unit: 'Hz' };

test('gate: hpHz and lpHz are the last controls, the fields of GATE_KEY_LIMITS, with the console\'s travel', () => {
  assert.deepEqual(json().kernels.gate.controls.slice(-2), [
    { name: 'hpHz', kind: 'travel', global: 'GATE_KEY_LIMITS', table: 'GATE_KEY_LIMITS' },
    { name: 'lpHz', kind: 'travel', global: 'GATE_KEY_LIMITS', table: 'GATE_KEY_LIMITS' },
  ]);
  assert.deepEqual(json().items.GATE_KEY_LIMITS.value, { hpHz: CONSOLE, lpHz: CONSOLE });
});

test('the released 2.7.0 header keeps every 2.6.0 line, byte for byte and in order, and adds only the key filter travels', () => {
  const old = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.6.0/omx_contract_limits.h'), 'utf8').split('\n');
  const now = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.7.0/omx_contract_limits.h'), 'utf8').split('\n');
  let i = 0;
  const added = [];
  for (const line of now) {
    if (i < old.length && line === old[i]) i++;
    else added.push(line);
  }
  assert.equal(i, old.length, `2.6.0 line ${i + 1} is not in the 2.7.0 header: ${old[i]}`);
  assert.deepEqual(added, [
    '#define OMX_GATE_KEY_HP_HZ_MIN 0.0f',
    '#define OMX_GATE_KEY_HP_HZ_MAX 20000.0f',
    '#define OMX_GATE_KEY_LP_HZ_MIN 0.0f',
    '#define OMX_GATE_KEY_LP_HZ_MAX 20000.0f',
    '',
  ]);
});

test('perturbation: a moved key filter edge leaves the console\'s travel, and --check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/gate.json', (d, at) => { at('GATE_KEY_LIMITS.lpHz').max = 22000; });
  assert.notDeepEqual(json(dir).items.GATE_KEY_LIMITS.value.lpHz, CONSOLE);
  assert.match(header(dir), /^#define OMX_GATE_KEY_LP_HZ_MAX 22000\.0f$/m);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
});
