// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// 2.3.0: the input trim is a kernel, `trim`, with the one control `trimDb`, so omx-dsp's trim face takes it by name
// (face-conformance) and reads its travel from the C render as OMX_TRIM_RANGE_*. TRIM_TRAVELS, that control's aggregate,
// was retired in dfbc289: nothing read it.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { DATA, ROOT, UNION, editData, scratchData } from './helpers.mjs';

const header = (dir = UNION) => renderC(resolveData(loadData(dir))).find((f) => f.path === 'omxcontract/omx_contract_limits.h').text;
const json = (dir = UNION) => JSON.parse(renderJson(resolveData(loadData(dir)), '0.0.0')[0].text);

test('trim: one control, trimDb, a travel whose item is TRIM_RANGE', () => {
  assert.deepEqual(json().kernels.trim.controls, [{ name: 'trimDb', kind: 'travel', global: 'TRIM_RANGE' }]);
});

test('trim: the C render keeps every 2.2.0 line, byte for byte and in order, and adds only OMX_TRIM_RANGE_*', () => {
  const old = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.2.0/omx_contract_limits.h'), 'utf8').split('\n');
  // the released 2.3.0 header: the release the trim kernel came in (later releases add their own, test/readouts.test.mjs)
  const now = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.3.0/omx_contract_limits.h'), 'utf8').split('\n');
  let i = 0;
  const added = [];
  for (const line of now) {
    if (i < old.length && line === old[i]) i++;
    else added.push(line);
  }
  assert.equal(i, old.length, `2.2.0 line ${i + 1} is not in the render: ${old[i]}`);
  assert.deepEqual(added, [
    '#define OMX_TRIM_RANGE_DEFAULT 0',
    '#define OMX_TRIM_RANGE_MAX 24',
    '#define OMX_TRIM_RANGE_MIN -24',
    '#define OMX_TRIM_RANGE_STEP 0.1f',
    '#define OMX_TRIM_RANGE_STEP_DOUBLE 0.1',
  ]);
});

test('trim: a moved come-up value moves the C define (perturbation; TRIM_TRAVELS, the table that read it, was retired in dfbc289)', () => {
  const moved = scratchData();
  editData(moved, 'kernels/trim.json', (d, at) => { at('TRIM_RANGE').travel.default = -6; });
  assert.match(header(moved), /^#define OMX_TRIM_RANGE_DEFAULT -6$/m);
});
