// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// 2.9.0 (#26): an EQ band comes up off (decided 2026-10-13). The contract declares bandOn's come-up value,
// EQ_BAND_ONS defaults to off, and every strip reads it from here: an omx-eq plugin already brings its bands up off,
// and the console's channel EQ follows. The released 2.9.0 header keeps every line of the released 2.8.0 header and adds only the
// set's default.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { renderTs } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, scratchData } from './helpers.mjs';

const resolved = (dir = UNION) => resolveData(loadData(dir));
const header = (dir) => renderC(resolved(dir))[0].text;
const json = (dir) => JSON.parse(renderJson(resolved(dir), '0.0.0')[0].text);

test('eq: bandOn is a choice of EQ_BAND_ONS (off, on) that comes up off', () => {
  const c = json().kernels.eq.controls.find((x) => x.name === 'bandOn');
  assert.deepEqual(c, { name: 'bandOn', kind: 'choice', global: 'EQ_BAND_ONS', count: 'EQ_BAND_COUNTS' });
  assert.deepEqual(json().items.EQ_BAND_ONS.value, ['off', 'on']);
  assert.equal(json().items.EQ_BAND_ONS.default, 'off');
  assert.match(header(), /^#define OMX_EQ_BAND_ONS_DEFAULT OMX_EQ_BAND_ONS_OFF$/m);
  assert.match(renderTs(resolved())[0].text, /^export const EQ_BAND_ONS = \[\n {2}"off",\n {2}"on"\n\];$/m);
});

test('the released 2.9.0 header keeps every 2.8.0 line, byte for byte and in order, and adds only the band switch\'s default', () => {
  const old = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.8.0/omx_contract_limits.h'), 'utf8').split('\n');
  const now = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.9.0/omx_contract_limits.h'), 'utf8').split('\n');
  let i = 0;
  const added = [];
  for (const line of now) {
    if (i < old.length && line === old[i]) i++;
    else added.push(line);
  }
  assert.equal(i, old.length, `2.8.0 line ${i + 1} is not in the 2.9.0 header: ${old[i]}`);
  assert.deepEqual(added, ['#define OMX_EQ_BAND_ONS_DEFAULT OMX_EQ_BAND_ONS_OFF']);
});

test('perturbation: the band switch\'s default removed, the C default goes and --check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/eq.json', (d, at) => { delete at('EQ_BAND_ONS').default; });
  assert.doesNotMatch(header(dir), /OMX_EQ_BAND_ONS_DEFAULT/);
  assert.equal(json(dir).items.EQ_BAND_ONS.default, undefined);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
});
