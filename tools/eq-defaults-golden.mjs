#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// eq-defaults-golden.mjs — THE default EQ rule's JavaScript spelling, moved from omx-contract
// (lib/eq-defaults.mjs and its oracle test/eq-defaults.test.mjs, removed in omx-contract 3.0.0) beside
// its C spelling, include/omxdsp/omx_eq_defaults.h. It reads the rule's items from data/kernels/eq.json
// (EQ_DEFAULT_CENTRES_FOUR_BAND_HZ, ISO_THIRD_OCTAVE_CENTRES_HZ, EQ_CENTRE_SERIES_R20_HZ, EQ_BAND_DEFAULTS,
// and EQ_FREQ_RANGE, the eq kernel's `freq` travel) and prints the default bands of 1..62 bands, one line
// per count: `<n> <type>:<centre Hz> ...`, or `<n> none` where the count has no distinct centres.
// test/eq_default_bands.c holds omx_eq_default_centres() and omx_eq_default_type() equal to it, bit for
// bit at float, and states the operator's ruled answers (2026-10-08) on their own.
//
//   node tools/eq-defaults-golden.mjs [--check]     (make eq-defaults-table)
//
// Without --check it writes test/golden/eq_default_bands.txt; with it, it writes nothing and exits 1
// when the committed file differs.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const GOLDEN = join(ROOT, 'test/golden/eq_default_bands.txt');
const COUNTS = 62; // one past the R20 series' 61 values: the first count with no answer

/**
 * The default band centres (Hz) of an EQ with `count` bands.
 * @param {number} count how many bands the strip's EQ starts with, a positive integer.
 * @param {{ four: number[], series: number[][], lo: number, hi: number }} p the declared four-band
 *   set, the preferred-frequency series from coarsest to finest, and the band frequency travel.
 * @returns {number[]}
 */
export function eqDefaultCentres(count, p) {
  if (!Number.isInteger(count) || count < 1) throw new RangeError(`eqDefaultCentres: ${count} is not a band count`);
  if (count === 4) return [...p.four];
  const targets = [];
  for (let i = 0; i < count; i++) targets.push(Math.log(p.lo) + (Math.log(p.hi / p.lo) * (i + 0.5)) / count);
  for (const series of p.series) {
    const out = targets.map((t) => {
      let best = series[0];
      let bestD = Infinity;
      for (const f of series) {
        const d = Math.abs(Math.log(f) - t);
        if (d < bestD) { bestD = d; best = f; }
      }
      return best;
    });
    if (new Set(out).size === out.length) return out;
  }
  throw new RangeError(`eqDefaultCentres: ${count} bands have no distinct preferred centres across ${p.lo}..${p.hi} Hz`);
}

/**
 * The band types of an EQ with `count` bands, by EQ_BAND_TYPES id: a low shelf, bells, a high shelf.
 * @param {number} count
 * @returns {string[]}
 */
export function eqDefaultTypes(count) {
  if (!Number.isInteger(count) || count < 1) throw new RangeError(`eqDefaultTypes: ${count} is not a band count`);
  return Array.from({ length: count }, (_, i) => (count === 1 ? 'bell' : i === 0 ? 'lowShelf' : i === count - 1 ? 'highShelf' : 'bell'));
}

/** The parameters of the rule, read off data/kernels/eq.json. */
function eqDefaultParams() {
  const eq = JSON.parse(readFileSync(join(ROOT, 'data/kernels/eq.json'), 'utf8'));
  const v = (name) => {
    const item = eq.constants[name];
    if (!item) throw new Error(`the default centre rule needs ${name}`);
    return item.values ?? item.value;
  };
  const freq = eq.controls.find((c) => c.name === 'freq');
  if (!freq?.travel) throw new Error('the default centre rule needs EQ_FREQ_RANGE (the eq kernel\'s freq travel)');
  return {
    four: v('EQ_DEFAULT_CENTRES_FOUR_BAND_HZ'), series: [v('ISO_THIRD_OCTAVE_CENTRES_HZ'), v('EQ_CENTRE_SERIES_R20_HZ')],
    lo: freq.travel.min, hi: freq.travel.max,
  };
}

const p = eqDefaultParams();
const lines = [];
for (let n = 1; n <= COUNTS; n++) {
  let c;
  try { c = eqDefaultCentres(n, p); } catch (e) { if (!(e instanceof RangeError)) throw e; }
  if (!c) { lines.push(`${n} none`); continue; }
  const types = eqDefaultTypes(n);
  lines.push(`${n} ${c.map((f, i) => `${types[i]}:${f}`).join(' ')}`);
}
const text = `${lines.join('\n')}\n`;
if (process.argv[2] === '--check') {
  if (readFileSync(GOLDEN, 'utf8') !== text) {
    console.error('eq-defaults-golden: test/golden/eq_default_bands.txt differs from the rule over data/ (run without --check)');
    process.exit(1);
  }
  console.log(`eq-defaults-golden: test/golden/eq_default_bands.txt is the rule over data/ (${COUNTS} counts)`);
} else {
  writeFileSync(GOLDEN, text);
  console.log(`eq-defaults-golden: wrote test/golden/eq_default_bands.txt (${COUNTS} counts)`);
}
