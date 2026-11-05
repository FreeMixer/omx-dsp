// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The 38 limits omx-dsp's tools/contract-gap.txt (omx-dsp e5267a4) listed as missing from omx-contract
// 1.2.1 are in the c render with omx-dsp's own literal, both read from e5267a4 itself: the names from its
// tools/contract-gap.txt, the literals from its committed include/omxdsp/omx_contract_limits.h.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { renderC } from './oc.mjs';
import { DROPPED_AFTER_PIN } from '../../tools/after-pin.mjs';
import { atCommit, editData, resolvedOf, scratchData, texts } from './helpers.mjs';
import { loadData, resolveData } from './oc.mjs';

const defines = (text) => new Map([...text.matchAll(/^#define (OMX_\w+) (.+)$/gm)].map((m) => [m[1], m[2].replace(/\s*\/\*.*\*\/\s*$/, '').trim()]));
const pinned = defines(atCommit('e5267a4', 'include/omxdsp/omx_contract_limits.h'));
const gap = atCommit('e5267a4', 'tools/contract-gap.txt').split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('#')).map((n) => [n, pinned.get(n)]);
/** The names a later commit stopped carrying, with its reason (tools/after-pin.mjs): left out, and the list is exact. */
const carried = gap.filter(([n]) => !DROPPED_AFTER_PIN.has(n));

test('every one of the 38 names omx-dsp read is defined with its literal, or named as dropped', () => {
  assert.equal(gap.length, 38);
  const d = defines(renderC(resolvedOf(texts()))[0].text);
  assert.deepEqual(gap.filter(([n]) => DROPPED_AFTER_PIN.has(n) && d.has(n)).map(([n]) => n), [], 'a name named as dropped is not carried');
  assert.deepEqual(carried.filter(([n, v]) => d.get(n) !== v).map(([n, v]) => `${n}: omx-dsp ${v}, contract ${d.get(n)}`), []);
});

test('perturbation: move one of them in its kernel file and its define follows, and the check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/fbs.json', (x, at) => { at('FBS_TRACK_SLOTS').value = 256; });
  const d = defines(renderC(resolveData(loadData(dir)))[0].text);
  assert.equal(d.get('OMX_FBS_TRACK_SLOTS'), '256');
  assert.deepEqual(carried.filter(([n, v]) => d.get(n) !== v).map(([n]) => n), ['OMX_FBS_TRACK_SLOTS']);
});
