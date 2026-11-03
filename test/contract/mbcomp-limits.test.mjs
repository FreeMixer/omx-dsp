// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The multiband compressor's limits (#62): the band floor and ceiling, the corner line as a fraction of the rate,
 * and the come-up corners for each band count. omx-dsp's omx_mbcomp.h staged them behind #ifndef until this
 * declaration (test/fixtures/omx-dsp-0f23bed/omx_mbcomp_staged.h, verbatim); the render defines every one of those
 * names with the value the kernel runs today, so dropping the staged block changes no sample. The corner line is
 * computed in double there, so its value is held against the _DOUBLE define. Moving any number turns this red.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, scratchData } from './helpers.mjs';

const staged = readFileSync(join(ROOT, 'test/fixtures/omx-dsp-0f23bed/omx_mbcomp_staged.h'), 'utf8');
/** NAME -> the numbers its define carries, read as C reads them (`2u` is 2, `1000.0f` is 1000). */
const numbers = (text) => new Map([...text.matchAll(/^#define (OMX_\w+) (.+)$/gm)].map((m) => [m[1], (m[2].match(/-?\d+(?:\.\d+)?/g) ?? []).map(Number)]));
const kernel = numbers(staged);
const header = (dir = UNION) => renderC(resolveData(loadData(dir)))[0].text;

/** The staged names whose numbers the render of `dir` does not give; the corner line is read in double. */
function differing(dir = UNION) {
  const ours = numbers(header(dir));
  const as = (n) => (n === 'OMX_MBC_CORNER_MAX_FRACTION' ? `${n}_DOUBLE` : n);
  return [...kernel.keys()].filter((n) => JSON.stringify(ours.get(as(n))) !== JSON.stringify(kernel.get(n)));
}

test('the fixture is the kernel\'s staged block: seven names, every one guarded by #ifndef', () => {
  assert.deepEqual([...kernel.keys()], ['OMX_MBC_MIN_BANDS', 'OMX_MBC_MAX_BANDS', 'OMX_MBC_CORNER_MAX_FRACTION',
    'OMX_MBC_CROSSOVER_DEFAULTS_2_INIT', 'OMX_MBC_CROSSOVER_DEFAULTS_3_INIT', 'OMX_MBC_CROSSOVER_DEFAULTS_4_INIT', 'OMX_MBC_CROSSOVER_DEFAULTS_5_INIT']);
  for (const n of kernel.keys()) assert.match(staged, new RegExp(`^#ifndef ${n}$`, 'm'), n);
});

test('every staged name is defined by the render with the number omx-dsp runs', () => {
  assert.deepEqual(differing(), []);
  const h = header();
  assert.match(h, /^#define OMX_MBC_MIN_BANDS 2$/m);
  assert.match(h, /^#define OMX_MBC_MAX_BANDS 5$/m);
  assert.match(h, /^#define OMX_MBC_CORNER_MAX_FRACTION_DOUBLE 0\.45$/m);
  assert.match(h, /^#define OMX_MBC_CROSSOVER_DEFAULTS_5_COUNT 4u$/m);
  assert.match(h, /^#define OMX_MBC_CROSSOVER_DEFAULTS_5_INIT \{ 100, 400, 1600, 6400 \}$/m);
});

test('each band count from the floor to the ceiling has its come-up list, bands - 1 corners, ascending, under the corner line at 48 kHz', () => {
  const h = numbers(header());
  const [lo] = h.get('OMX_MBC_MIN_BANDS');
  const [hi] = h.get('OMX_MBC_MAX_BANDS');
  const [frac] = h.get('OMX_MBC_CORNER_MAX_FRACTION_DOUBLE');
  for (let b = lo; b <= hi; b++) {
    const hz = h.get(`OMX_MBC_CROSSOVER_DEFAULTS_${b}_INIT`);
    assert.ok(hz, `${b} bands has a come-up list`);
    assert.equal(hz.length, b - 1, `${b} bands`);
    assert.ok(hz.every((f, i) => i === 0 || f > hz[i - 1]), `${b} bands: ascending`);
    assert.ok(hz.every((f) => f < frac * 48000), `${b} bands: under the line at 48 kHz`);
  }
});

const perturb = [
  ['MBC_MIN_BANDS', (at) => { at('MBC_MIN_BANDS').value = 3; }, 'OMX_MBC_MIN_BANDS'],
  ['MBC_MAX_BANDS', (at) => { at('MBC_MAX_BANDS').value = 6; }, 'OMX_MBC_MAX_BANDS'],
  ['MBC_CORNER_MAX_FRACTION', (at) => { at('MBC_CORNER_MAX_FRACTION').value = 0.4; }, 'OMX_MBC_CORNER_MAX_FRACTION'],
  ['MBC_CROSSOVER_DEFAULTS_2', (at) => { at('MBC_CROSSOVER_DEFAULTS_2').values[0] = 1200; }, 'OMX_MBC_CROSSOVER_DEFAULTS_2_INIT'],
  ['MBC_CROSSOVER_DEFAULTS_3', (at) => { at('MBC_CROSSOVER_DEFAULTS_3').values[1] = 2500; }, 'OMX_MBC_CROSSOVER_DEFAULTS_3_INIT'],
  ['MBC_CROSSOVER_DEFAULTS_4', (at) => { at('MBC_CROSSOVER_DEFAULTS_4').values[2] = 6000; }, 'OMX_MBC_CROSSOVER_DEFAULTS_4_INIT'],
  ['MBC_CROSSOVER_DEFAULTS_5', (at) => { at('MBC_CROSSOVER_DEFAULTS_5').values[0] = 80; }, 'OMX_MBC_CROSSOVER_DEFAULTS_5_INIT'],
];
for (const [item, edit, name] of perturb) {
  test(`perturbation: move ${item}, the check names ${name} and render --check goes red`, () => {
    const dir = scratchData();
    editData(dir, 'kernels/mbcomp.json', (d, at) => edit(at));
    assert.deepEqual(differing(dir), [name]);
    assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
  });
}
