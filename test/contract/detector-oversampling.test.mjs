// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// 2.9.0 (#28): the compressor's detector oversampling ids are the console's, auto, off, 4x (decided
// 2026-10-13, option a), in omx-dsp's index order, so the console re-exports DETECTOR_OVERSAMPLINGS and its wire and
// stored sessions are unchanged. The released 2.10.0 header keeps every line of the released 2.9.0 header but the third
// enumerator, which is spelled from the id: OMX_DETECTOR_OVERSAMPLINGS_4X, still 2.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { renderTs } from './oc.mjs';
import { DATA, ROOT, UNION } from './helpers.mjs';

const resolved = (dir = UNION) => resolveData(loadData(dir));
const header = (dir) => renderC(resolved(dir))[0].text;
const json = (dir) => JSON.parse(renderJson(resolved(dir), '0.0.0')[0].text);

test('comp: DETECTOR_OVERSAMPLINGS is auto, off, 4x, in that order, and comes up auto', () => {
  assert.deepEqual(json().items.DETECTOR_OVERSAMPLINGS.value, ['auto', 'off', '4x']);
  assert.equal(json().items.DETECTOR_OVERSAMPLINGS.default, 'auto');
  assert.deepEqual(json().items.DETECTOR_OVERSAMPLINGS.labels, ['Auto', 'Off', '4x']);
  assert.match(renderTs(resolved())[0].text, /^export const DETECTOR_OVERSAMPLINGS = \[\n {2}"auto",\n {2}"off",\n {2}"4x"\n\];$/m);
});

test('the released 2.10.0 header keeps every 2.9.0 line but the 4x enumerator, which keeps its value', () => {
  const old = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.9.0/omx_contract_limits.h'), 'utf8').split('\n');
  const now = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.10.0/omx_contract_limits.h'), 'utf8').split('\n');
  assert.equal(now.length, old.length);
  const differ = old.map((l, i) => [l, now[i]]).filter(([a, b]) => a !== b);
  assert.deepEqual(differ, [['  OMX_DETECTOR_OVERSAMPLINGS_X4 = 2,', '  OMX_DETECTOR_OVERSAMPLINGS_4X = 2,']]);
});
