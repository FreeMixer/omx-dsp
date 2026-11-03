// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// #46: the drive's curve and band choices declare the come-up a fresh strip is seeded at (curve soft,
// band full), the way autoGain, stereoLink and hfRolloff already declare theirs.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { renderTs } from './oc.mjs';
import { DATA, UNION } from './helpers.mjs';

const resolved = () => resolveData(loadData(UNION));
const items = () => JSON.parse(renderJson(resolved(), '0.0.0')[0].text).items;

test('drive: DRIVE_CURVES comes up soft and DRIVE_BANDS comes up full, as the JSON render declares them', () => {
  assert.equal(items().DRIVE_CURVES.default, 'soft');
  assert.equal(items().DRIVE_BANDS.default, 'full');
});

test('drive: every choice of the drive row carries a default', () => {
  const choices = Object.entries(items()).filter(([k, v]) => k.startsWith('DRIVE_') && Array.isArray(v.value) && v.kind === 'set');
  assert.ok(choices.length >= 5, 'the probe sees the drive choices');
  for (const [k, v] of choices) assert.notEqual(v.default, undefined, `${k} declares its come-up`);
});

test('the C and ts renders carry the two defaults', () => {
  const h = renderC(resolved())[0].text;
  assert.match(h, /^#define OMX_DRIVE_CURVES_DEFAULT OMX_DRIVE_CURVES_SOFT$/m);
  assert.match(h, /^#define OMX_DRIVE_BANDS_DEFAULT OMX_DRIVE_BANDS_FULL$/m);
  assert.match(renderTs(resolved())[0].text, /export const DRIVE_CURVES = \[[^\]]*\];/);
});
