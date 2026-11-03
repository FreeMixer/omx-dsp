// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The EQ curve grid (omx-dsp's, read from the owners' fixture; docs/curves.md): the log frequency grid an EQ response curve is sampled on.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { loadData } from './oc.mjs';
import { classify } from './oc.mjs';
import { validateData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderTs } from './oc.mjs';
import { relOf, resolvedOf, texts } from './helpers.mjs';

const curves = relOf('EQ_CURVE_GRID');
const problems = (t) => validateData(loadData(null, t)).map((p) => `${p.item}:${p.rule}`);
const gridOf = (t = texts()) => resolvedOf(t).get('EQ_CURVE_GRID').value;

/** The grid's points at `rate`: startHz x 10^(k / pointsPerDecade), k = 0, 1, ... while at or below the bound. */
function points(grid, rate) {
  const { startHz, pointsPerDecade, nyquistFraction } = grid;
  const bound = nyquistFraction * (rate / 2);
  const out = [];
  for (let k = 0; ; k++) {
    const f = startHz * Math.pow(10, k / pointsPerDecade);
    if (f > bound) return out;
    out.push(f);
  }
}

// The curve the console draws today: 160 points from 20 Hz to 20 kHz inclusive, log-spaced, each point
// fMin x (fMax / fMin)^(i / (n - 1)).
const today = Array.from({ length: 160 }, (_, i) => 20 * Math.pow(20000 / 20, i / 159));
const RATES = [32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000];

test('EQ_CURVE_GRID is a sheet of a start, a step per decade and a Nyquist fraction, each with its prose', () => {
  const g = gridOf();
  assert.deepEqual(Object.keys(g).sort(), ['nyquistFraction', 'pointsPerDecade', 'startHz']);
  const item = JSON.parse(texts()[curves]).EQ_CURVE_GRID;
  assert.equal(item.kind, 'sheet');
  assert.deepEqual(Object.keys(item.docs).sort(), Object.keys(g).sort());
  assert.deepEqual(problems(texts()), []);
});

test('the grid reproduces the 160 points the console draws from 20 Hz to 20 kHz, within a few ulp', () => {
  const at = points(gridOf(), 96000);
  const below = at.filter((f) => f <= 20000 * (1 + 1e-12));
  assert.equal(below.length, 160);
  for (let i = 0; i < 160; i++) assert.ok(Math.abs(below[i] - today[i]) <= 1e-14 * today[i], `point ${i}: ${below[i]} against ${today[i]}`);
  assert.ok(Math.abs(below[159] - 20000) <= 1e-14 * 20000, 'the 160th point is 20 kHz');
  assert.ok(at[160] > 20000 * 1.04, 'the grid goes on above 20 kHz at the same step');
});

test('the points up to 20 kHz are the same at every rate whose bound lies above it, and none passes the bound', () => {
  const g = gridOf();
  const ref = points(g, 192000).slice(0, 160);
  for (const rate of RATES) {
    const at = points(g, rate);
    const bound = g.nyquistFraction * (rate / 2);
    assert.ok(at.at(-1) <= bound, `${rate}: the last point is at or below 0.999 x Nyquist`);
    assert.ok(g.startHz * Math.pow(10, at.length / g.pointsPerDecade) > bound, `${rate}: the next point would pass the bound`);
    if (bound > 20000) assert.deepEqual(at.slice(0, 160), ref, `${rate}: the points up to 20 kHz`);
  }
  assert.equal(points(g, 48000).length, 164);
  assert.equal(points(g, 96000).length, 180);
  assert.equal(points(g, 192000).length, 196);
});

test('the C render carries the grid as OMX_EQ_CURVE_GRID_*, the TypeScript render as EQ_CURVE_GRID', () => {
  const h = renderC(resolvedOf(texts()))[0].text;
  assert.match(h, /^#define OMX_EQ_CURVE_GRID_START_HZ 20$/m);
  assert.match(h, /^#define OMX_EQ_CURVE_GRID_POINTS_PER_DECADE 53$/m);
  assert.match(h, /^#define OMX_EQ_CURVE_GRID_NYQUIST_FRACTION 0\.999f$/m);
  assert.match(h, /^#define OMX_EQ_CURVE_GRID_NYQUIST_FRACTION_DOUBLE 0\.999$/m);
  const js = renderTs(resolvedOf(texts())).find((f) => f.path.endsWith('.js')).text;
  assert.match(js, /^export const EQ_CURVE_GRID = \{\n {2}"startHz": 20,\n {2}"pointsPerDecade": 53,\n {2}"nyquistFraction": 0\.999\n\};$/m);
});

test('perturbation: a moved step moves the render and no longer reproduces the drawn curve', () => {
  const moved = texts((d) => { d[curves].EQ_CURVE_GRID.value.pointsPerDecade = 54; });
  assert.match(renderC(resolvedOf(moved))[0].text, /^#define OMX_EQ_CURVE_GRID_POINTS_PER_DECADE 54$/m);
  assert.notEqual(points(gridOf(moved), 96000).filter((f) => f <= 20000 * (1 + 1e-12)).length, 160);
  assert.ok(problems(texts((d) => { d[curves].EQ_CURVE_GRID.docs.stopHz = 'x'; })).includes('EQ_CURVE_GRID:sheet-docs'));
});

test('semver: the grid added is MINOR, a moved value MAJOR, its prose PATCH', () => {
  const base = resolvedOf(texts());
  const level = (fn) => classify(base, resolvedOf(texts(fn))).level;
  assert.equal(classify(resolvedOf(texts((d) => { delete d[curves]; })), base).level, 'minor');
  assert.equal(level((d) => { d[curves].EQ_CURVE_GRID.value.startHz = 10; }), 'major');
  assert.equal(level((d) => { d[curves].EQ_CURVE_GRID.docs.startHz = 'reworded'; }), 'patch');
});
