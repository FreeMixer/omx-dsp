// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// A kernel file refers to a value it declares, never repeats its number (owner ruling 2026-11-03): a readout's range
// bound written as a number equal to a bound of one of the kernel's own travels in the same unit, or a travel's min
// or max written as a number equal to one of the kernel's own scalars in the same unit, is the declared value
// written twice. The comp's gain reduction is the model: its floor is { "ref": "COMP_LIMITS.thresholdDb.min" }.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { controlGlobal, loadData } from './oc.mjs';
import { UNION } from './helpers.mjs';

/** The numbers a kernel file writes where one of its own declared values stands: `<file> <where> = <value> = <name>`. */
export function repeatedBounds(files) {
  const out = [];
  for (const f of files) {
    const travels = (f.doc.controls ?? []).filter((c) => c.travel).map((c) => ({ c, name: c.table ? `${c.table}.${c.name}` : controlGlobal(f.rel, c) }));
    const bounds = travels.flatMap(({ c, name }) => ['min', 'max'].filter((k) => typeof c.travel[k] === 'number').map((k) => ({ ref: `${name}.${k}`, v: c.travel[k], unit: c.travel.unit })));
    const scalars = Object.entries(f.doc.constants ?? {}).filter(([, k]) => k.kind === 'scalar' && typeof k.value === 'number').map(([n, k]) => ({ ref: n, v: k.value, unit: k.unit }));
    const file = f.rel.replace(/^.*\//, '');
    for (const r of f.doc.readouts ?? []) for (const k of ['min', 'max']) {
      const hit = typeof r[k] === 'number' && r[k] !== 0 && bounds.find((b) => b.v === r[k] && b.unit === r.unit);
      if (hit) out.push(`${file} readout ${r.name}.${k} = ${r[k]} = ${hit.ref}`);
    }
    for (const { c, name } of travels) for (const k of ['min', 'max']) {
      const hit = typeof c.travel[k] === 'number' && c.travel[k] !== 0 && scalars.find((s) => s.v === c.travel[k] && s.unit === c.travel.unit);
      if (hit) out.push(`${file} travel ${name}.${k} = ${c.travel[k]} = ${hit.ref}`);
    }
  }
  return out;
}

const kernels = () => loadData(UNION).files.filter((f) => f.rel.startsWith('data/kernels/'));

test('no kernel file writes a bound as the number of a value it declares', () => {
  assert.deepEqual(repeatedBounds(kernels()), []);
});

test('the check names a readout floor and a travel bound written as a number, and passes a reference', () => {
  const doc = {
    controls: [{ name: 'thresholdDb', kind: 'travel', travel: { min: -40, max: 0, step: 0.5, unit: 'dB' } }],
    readouts: [{ name: 'gainReductionDb', unit: 'dB', min: -40, max: 0, scale: 'linear', rateHz: 50 }],
    constants: { X_CEILING_DB: { kind: 'scalar', unit: 'dB', value: 6 }, X_COUNT: { kind: 'scalar', unit: '', value: -40 } },
  };
  doc.controls.push({ name: 'boostDb', kind: 'travel', travel: { min: 0, max: 6, step: 0.5, unit: 'dB' } });
  assert.deepEqual(repeatedBounds([{ rel: 'data/kernels/x.json', doc }]), [
    'x.json readout gainReductionDb.min = -40 = X_THRESHOLD_DB_RANGE.min',
    'x.json travel X_BOOST_DB_RANGE.max = 6 = X_CEILING_DB',
  ]);
  doc.readouts[0].min = { ref: 'X_THRESHOLD_DB_RANGE.min' };
  doc.controls[1].travel.max = { ref: 'X_CEILING_DB' };
  assert.deepEqual(repeatedBounds([{ rel: 'data/kernels/x.json', doc }]), [], 'a reference is no repeat, and a number in another unit is none');
});
