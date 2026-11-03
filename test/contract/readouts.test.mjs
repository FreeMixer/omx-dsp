// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The readouts these kernels declare: each stated once, its range by reference to the travel that bounds it where one
// does, its rate by READOUT_RATE_HZ, and the controls that re-arm a kernel. (omx-contract held the four gain
// reductions of 2.4.0 and the limiter's look-ahead over its 2.14.0 copy of this data.) The comp's key level and
// effective attack came with its key path (c208f5f, FX-2); the clipper (347ae79) states its
// display floor as a number; the glue's is its threshold's minimum, by reference (owner ruling 2026-11-03).
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { loadData, renderJson, resolveData } from './oc.mjs';
import { UNION, resolvedOf, texts } from './helpers.mjs';

const rate = { ref: 'READOUT_RATE_HZ' };
const gr = (min) => ({ name: 'gainReductionDb', unit: 'dB', min, max: 0, scale: 'linear', rateHz: rate });
const WANT = {
  clipper: [gr(-60)],
  comp: [gr({ ref: 'COMP_LIMITS.thresholdDb.min' }),
    { name: 'keyLevelDb', unit: 'dBFS', min: { ref: 'COMP_LIMITS.thresholdDb.min' }, max: { ref: 'COMP_LIMITS.thresholdDb.max' }, scale: 'linear', rateHz: rate },
    { name: 'effectiveAttackMs', unit: 'ms', min: { ref: 'COMP_LIMITS.attackMs.min' }, max: { ref: 'COMP_LIMITS.attackMs.max' }, scale: 'log', rateHz: rate }],
  deesser: [gr({ ref: 'DEESS_RANGE_RANGE.min' })],
  gate: [gr({ ref: 'GATE_LIMITS.rangeDb.min' })],
  glue: [gr({ ref: 'GLUE_THRESHOLD_DB_RANGE.min' })],
  limiter: [gr({ ref: 'LIMITER_LIMITS.ceilingDb.min' })],
};

const declared = () => Object.fromEntries(loadData(UNION).files.filter((f) => f.rel.startsWith('data/kernels/') && f.doc.readouts?.some((x) => x.name === 'gainReductionDb'))
  .map((f) => [f.rel.replace(/^.*\/|\.json$/g, ''), f.doc.readouts.map(({ doc, ...x }) => x)]));

test('the gain reductions declared, each with its range and its rate', () => {
  assert.deepEqual(declared(), WANT);
});

test('a range by reference reads the travel that bounds it, and the rate is READOUT_RATE_HZ', () => {
  const r = resolvedOf(texts());
  const at = (path) => path.split('.').slice(1).reduce((v, k) => v[k], r.get(path.split('.')[0]).value);
  for (const [k, list] of Object.entries(WANT)) {
    list.forEach((want, i) => {
      const got = r.kernels[k].readouts[i];
      for (const end of ['min', 'max']) assert.equal(got[end], typeof want[end] === 'object' ? at(want[end].ref) : want[end], `${k}.${want.name}.${end}`);
      assert.equal(got.rateHz, r.get('READOUT_RATE_HZ').value, `${k}.${want.name}`);
    });
    assert.ok(r.kernels[k].readouts[0].min < 0, `${k}: a reduction runs below 0 dB`);
  }
  assert.equal(r.get('READOUT_RATE_HZ').value, 50);
});

test('the limiter look-ahead re-arms, and it is the only control that does', () => {
  const { kernels } = JSON.parse(renderJson(resolveData(loadData(UNION)), '0.0.0')[0].text);
  const rearming = Object.entries(kernels).flatMap(([k, { controls }]) => controls.filter((c) => c.rearms).map((c) => `${k}.${c.name}`));
  assert.deepEqual(rearming, ['limiter.lookaheadMs']);
});
