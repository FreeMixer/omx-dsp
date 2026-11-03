// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// A kernel file refers to a value it declares, never repeats its number, and a header reads a declared value from the
// render, never restates it (owner ruling 2026-11-03): a readout's range
// bound written as a number equal to a bound of one of the kernel's own travels in the same unit, or a travel's min
// or max written as a number equal to one of the kernel's own scalars in the same unit, is the declared value
// written twice. The comp's gain reduction is the model: its floor is { "ref": "COMP_LIMITS.thresholdDb.min" }.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { controlGlobal, loadData } from './oc.mjs';
import { HERE, UNION } from './helpers.mjs';

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

/** The numbers a declaration holds (a scalar's value, a sheet's or a travel's numeric leaves), references aside. */
const numbers = (v) => (typeof v === 'number' ? [v] : v && typeof v === 'object' && !('ref' in v) && !('derive' in v) ? Object.values(v).flatMap(numbers) : []);

/**
 * The C defines that restate a declared value: a declaration whose `source.impl` names `<header>#<MACRO>` says that
 * macro is its implementation, and a `#define <MACRO>` there written as the declaration's own number is the value
 * written twice; the macro reads the render instead. `read(path)` gives a header's text.
 */
export function repeatedDefines(files, read) {
  const out = [];
  const visit = (rel, o, name) => {
    if (!o || typeof o !== 'object') return;
    const impl = o.source?.impl;
    if (typeof impl === 'string' && impl.includes('#')) {
      const [header, macro] = impl.split('#');
      const m = new RegExp(`^#define ${macro}\\s+\\(?\\s*(-?[0-9.eE+-]+?)[fFuU]?\\s*\\)?\\s*$`, 'm').exec(read(header));
      if (m && [...numbers(o.value), ...numbers(o.travel)].includes(Number(m[1]))) out.push(`${header}: #define ${macro} ${m[1]} is ${name} (${rel})`);
    }
    for (const [k, v] of Object.entries(o)) if (k !== 'source') visit(rel, v, /^[A-Z][A-Z0-9_]*$/.test(k) ? k : name);
  };
  for (const f of files) visit(f.rel, f.doc, '');
  return out;
}

const readHere = (p) => { try { return readFileSync(join(HERE, p), 'utf8'); } catch { return ''; } };
const ownFiles = () => loadData(UNION).files.filter((f) => f.rel.startsWith('data/kernels/') || f.rel.startsWith('data/primitives/'));

test('no header restates as a literal the value of a declaration it implements', () => {
  assert.deepEqual(repeatedDefines(ownFiles(), readHere), []);
});

test('the check names a define written as its declaration\'s number, and passes one that reads the render', () => {
  const doc = { X_GATE_LU: { kind: 'scalar', unit: 'dB', value: -10, source: { impl: 'x.h#OMX_X_GATE_LU' } } };
  const files = [{ rel: 'data/primitives/x.json', doc }];
  assert.deepEqual(repeatedDefines(files, () => '#define OMX_X_GATE_LU (-10.0)\n'), ['x.h: #define OMX_X_GATE_LU -10.0 is X_GATE_LU (data/primitives/x.json)']);
  assert.deepEqual(repeatedDefines(files, () => '#define OMX_X_GATE_LU ((double)OMX_X_GATE_LU_RENDERED)\n'), []);
  assert.deepEqual(repeatedDefines(files, () => '#define OMX_X_GATE_LU (-20.0)\n'), [], 'another number is no repeat');
});
