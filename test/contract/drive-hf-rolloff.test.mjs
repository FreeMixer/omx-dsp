// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// 2.9.0 (#29): the drive's hf roll-off ids are the console's, off, 12000, 16000 (decided 2026-10-13), each
// carrying its corner in Hz as its value and off none, so no 0 Hz doubles as "no roll-off". The console re-exports
// DRIVE_HF_ROLLOFFS; a kernel reads the corner from the id with omx_drive_hf_rolloffs_value(). The C render keeps
// every line of the released 2.10.0 header but the roll-off set's, the hosted stage's numbers (2.12.0) and the EQ curve grid's (2.13.0).
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

test('drive: DRIVE_HF_ROLLOFFS is off, 12000, 16000, carrying none, 12000 and 16000 Hz, and comes up off', () => {
  const it = json().items.DRIVE_HF_ROLLOFFS;
  assert.deepEqual([it.value, it.values, it.unit, it.default], [['off', '12000', '16000'], [null, 12000, 16000], 'Hz', 'off']);
  const js = renderTs(resolved())[0].text;
  assert.match(js, /^export const DRIVE_HF_ROLLOFFS = \[\n {2}"off",\n {2}"12000",\n {2}"16000"\n\];$/m, 'the console\'s wire vocabulary, as it re-exports it');
  // an object's integer-like keys come first in JavaScript, so the map is read by id, not in order
  const m = /^export const DRIVE_HF_ROLLOFFS_VALUES = (\{[^;]*\});$/m.exec(js);
  assert.ok(m, 'the ts render exports DRIVE_HF_ROLLOFFS_VALUES');
  assert.deepEqual(JSON.parse(m[1]), { off: null, 12000: 12000, 16000: 16000 });
});

test('the C render replaces the 2.10.0 roll-off block with one that gives the kernel each corner from its id', () => {
  const old = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.10.0/omx_contract_limits.h'), 'utf8');
  const was = [
    '  OMX_DRIVE_HF_ROLLOFFS_0 = 0,',
    '  OMX_DRIVE_HF_ROLLOFFS_12000 = 12000,',
    '  OMX_DRIVE_HF_ROLLOFFS_16000 = 16000,',
    '};',
    '#define OMX_DRIVE_HF_ROLLOFFS_COUNT 3u',
    '#define OMX_DRIVE_HF_ROLLOFFS_DEFAULT OMX_DRIVE_HF_ROLLOFFS_0',
  ].join('\n');
  const is = [
    '  OMX_DRIVE_HF_ROLLOFFS_OFF = 0,',
    '  OMX_DRIVE_HF_ROLLOFFS_12000 = 1,',
    '  OMX_DRIVE_HF_ROLLOFFS_16000 = 2,',
    '};',
    '#define OMX_DRIVE_HF_ROLLOFFS_COUNT 3u',
    '#define OMX_DRIVE_HF_ROLLOFFS_DEFAULT OMX_DRIVE_HF_ROLLOFFS_OFF',
    '#define OMX_DRIVE_HF_ROLLOFFS_UNIT "Hz"',
    '#define OMX_DRIVE_HF_ROLLOFFS_12000_VALUE 12000.0f',
    '#define OMX_DRIVE_HF_ROLLOFFS_16000_VALUE 16000.0f',
    'static inline int omx_drive_hf_rolloffs_value(int id, float *out) {',
    '  switch (id) {',
    '  case OMX_DRIVE_HF_ROLLOFFS_12000: *out = OMX_DRIVE_HF_ROLLOFFS_12000_VALUE; return 1;',
    '  case OMX_DRIVE_HF_ROLLOFFS_16000: *out = OMX_DRIVE_HF_ROLLOFFS_16000_VALUE; return 1;',
    '  default: return 0;',
    '  }',
    '}',
  ].join('\n');
  assert.equal(old.split(was).length, 2, 'the 2.10.0 header holds the old block once');
  // the rest of the header has grown with every kernel since 2.10.0; the block of the roll-off set is what this holds
  assert.equal(header().split(is).length, 2, 'the render holds the new block once, each corner its id\'s value');
});

test('perturbation: a moved corner moves the C value, and --check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/drive.json', (d, at) => { at('DRIVE_HF_ROLLOFFS').values[1] = 10000; });
  assert.match(header(dir), /^#define OMX_DRIVE_HF_ROLLOFFS_12000_VALUE 10000\.0f$/m);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
});
