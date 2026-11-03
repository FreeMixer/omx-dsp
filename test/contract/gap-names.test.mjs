// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The 38 limits omx-dsp's tools/contract-gap.txt (omx-dsp 6d6fcda) listed as missing from omx-contract
// 1.2.1 are in the c render with omx-dsp's own literal. omx-dsp's tools/contract-agree.sh is the same
// check against its committed header and its sources; this holds it without the omx-dsp checkout.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { renderC } from './oc.mjs';
import { ROOT, editData, resolvedOf, scratchData, texts } from './helpers.mjs';
import { loadData, resolveData } from './oc.mjs';

const gap = readFileSync(join(ROOT, 'test/fixtures/omx-dsp-6d6fcda/gap-names.tsv'), 'utf8').split('\n').filter((l) => l && !l.startsWith('#')).map((l) => l.split('\t'));
const defines = (text) => new Map([...text.matchAll(/^#define (OMX_\w+) (.+)$/gm)].map((m) => [m[1], m[2].replace(/\s*\/\*.*\*\/\s*$/, '').trim()]));

test('every one of the 38 names omx-dsp reads is defined, with its literal', () => {
  assert.equal(gap.length, 38);
  const d = defines(renderC(resolvedOf(texts()))[0].text);
  assert.deepEqual(gap.filter(([n, v]) => d.get(n) !== v).map(([n, v]) => `${n}: omx-dsp ${v}, contract ${d.get(n)}`), []);
});

test('perturbation: move one of them in its kernel file and its define follows, and the check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/fbs.json', (x, at) => { at('FBS_TRACK_SLOTS').value = 256; });
  const d = defines(renderC(resolveData(loadData(dir)))[0].text);
  assert.equal(d.get('OMX_FBS_TRACK_SLOTS'), '256');
  assert.deepEqual(gap.filter(([n, v]) => d.get(n) !== v).map(([n]) => n), ['OMX_FBS_TRACK_SLOTS']);
});
