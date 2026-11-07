// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * THE PLACEMENT TWIN READS THE C PLACEMENT'S VECTORS: FbsPlace (dist/, the published code) runs every vector of
 * test/analysis/fixtures/fbs_place.vectors, the file test/analysis/fbs_place.test.c runs omx_fbs_place.h over, and
 * must give every value it states inside the file's tolerances. Each rule has an arm that breaks it in the twin
 * (a subclass overriding the one method) and must turn a vector red.
 */
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { EQ_GAIN_RANGE, FBS_HOLD_NEVER, FBS_MAX_CUT_DB, FbsPlace, ORIGIN, ORIGIN_WORDS, SHAPE_WORDS, fbsCents, fbsGainCap } from '../dist/index.js';

const VECTORS = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'test', 'analysis', 'fixtures', 'fbs_place.vectors');

const num = (w) => (w === 'nan' ? NaN : Number(w));
const near = (a, b, tol) => (Number.isNaN(b) ? Number.isNaN(a) : Math.abs(a - b) <= tol);

/** The file as vectors of lines, and its tolerances. */
export function readVectors(text = readFileSync(VECTORS, 'utf8')) {
  const tol = {};
  const vectors = [];
  let cur = null;
  for (const raw of text.split('\n')) {
    const line = raw.replace(/#.*/, '').trim();
    if (!line) continue;
    const w = line.split(/\s+/);
    if (w[0] === 'tolerance') tol[w[1]] = Number(w[2]);
    else if (w[0] === 'vector') vectors.push((cur = { name: w[1], lines: [] }));
    else if (w[0] === 'end') cur = null;
    else if (cur) cur.lines.push(w);
    else throw new Error(`a line outside a vector: ${line}`);
  }
  return { tol, vectors };
}

const rings = (w, from) => {
  const c = [];
  const s = [];
  for (let i = from; i < w.length; ) {
    if (w[i] === 'c') { c.push({ freqHz: num(w[i + 1]), depthDb: num(w[i + 2]) }); i += 3; }
    else if (w[i] === 's') { s.push(num(w[i + 1])); i += 2; }
    else throw new Error(`a ring word that is neither c nor s: ${w[i]}`);
  }
  return { c, s };
};

/** Run one vector on a placement the factory makes; the failures, as strings. */
export function runVector(v, tol, make = (pool) => new FbsPlace(pool)) {
  const fail = [];
  const p = make('live');
  let seen = 0;
  const hold = (w) => (w === 'never' ? FBS_HOLD_NEVER : num(w));
  for (const w of v.lines) {
    const at = `${v.name}: ${w.join(' ')}`;
    switch (w[0]) {
      case 'pool': p.pool = w[1]; break;
      case 'set': p.set(hold(w[1]), num(w[2]), num(w[3]), num(w[4]), num(w[5])); break;
      case 'config': {
        const got = [p.holdS === FBS_HOLD_NEVER ? 'never' : p.holdS, p.maxBands, p.notchQ, p.mergeCents, p.cooldownS];
        const want = [w[1] === 'never' ? 'never' : num(w[1]), num(w[2]), num(w[3]), num(w[4]), num(w[5])];
        if (JSON.stringify(got) !== JSON.stringify(want)) fail.push(`${at}: read ${got.join(' ')}`);
        break;
      }
      case 'tick': { const r = rings(w, 2); p.tick(num(w[1]), r.s, r.c); break; }
      case 'ticks': {
        const r = rings(w, 4);
        for (let t = num(w[1]); t <= num(w[2]); t += num(w[3])) p.tick(t, r.s, r.c);
        break;
      }
      case 'advance': p.advance(num(w[1])); break;
      case 'slot': {
        const r = p.report(num(w[1]));
        const ok = r.freqHz === num(w[2]) && near(r.depthDb, num(w[3]), tol.gain) && r.q === num(w[4]) && r.on === num(w[5]) &&
          ORIGIN_WORDS[r.origin] === w[6] && SHAPE_WORDS[r.shape] === w[7];
        if (!ok) fail.push(`${at}: read ${r.freqHz} ${r.depthDb} ${r.q} ${r.on} ${ORIGIN_WORDS[r.origin]} ${SHAPE_WORDS[r.shape]}`);
        break;
      }
      case 'free': {
        const r = p.report(num(w[1]));
        if (r.on !== 0 || r.origin !== ORIGIN.user || SHAPE_WORDS[r.shape] !== 'off' || r.freqHz !== 0) fail.push(`${at}: not free`);
        break;
      }
      case 'count': if (p.count() !== num(w[1])) fail.push(`${at}: read ${p.count()}`); break;
      case 'log': {
        const got = p.log.slice(seen).map((e) => `${e.action} ${e.freqHz} ${e.slot}`).join(' ');
        const want = [];
        for (let i = 1; i < w.length; i += 3) want.push(`${w[i]} ${num(w[i + 1])} ${num(w[i + 2])}`);
        if (got !== want.join(' ')) fail.push(`${at}: read [${got}]`);
        seen = p.log.length;
        break;
      }
      case 'cap': {
        const got = fbsGainCap(ORIGIN_WORDS.indexOf(w[1]), num(w[2]));
        if (!near(got, num(w[3]), 0)) fail.push(`${at}: read ${got}`);
        break;
      }
      case 'cents': {
        const got = fbsCents(num(w[1]), num(w[2]));
        if (!near(got, num(w[3]), tol.cents)) fail.push(`${at}: read ${got}`);
        break;
      }
      default: throw new Error(`${at}: no such line`);
    }
  }
  return fail;
}

/** Every vector's failures under a factory. */
const runAll = (make) => {
  const { tol, vectors } = readVectors();
  return vectors.flatMap((v) => runVector(v, tol, make));
};

describe('the placement twin reads the C placement\'s vectors', () => {
  const { tol, vectors } = readVectors();
  it('states its tolerances and covers every rule the brief names', () => {
    assert.equal(tol.gain, 1e-9);
    assert.equal(tol.cents, 1e-4);
    const names = vectors.map((v) => v.name);
    for (const n of ['empty-and-silence', 'gain-cap-every-origin', 'pools-fixed-notch-live-bell', 'merge-across-the-radius', 'staged-fade', 'slots-keep-their-finds'])
      assert.ok(names.includes(n), n);
  });
  for (const v of vectors) it(v.name, () => assert.deepEqual(runVector(v, tol), []));
});

/** One rule broken: the twin with one method overridden. */
const arm = (override) => (pool) => {
  const p = new FbsPlace(pool);
  override(p);
  return p;
};

describe('every rule has an arm that turns a vector red', () => {
  const arms = {
    'merge radius one cent wider': (p) => { p.sameRegion = function (a, b) { return Math.abs(a - b) <= Math.max(3, 0.015 * Math.max(a, b)) || fbsCents(a, b) <= this.mergeCents + 1; }; },
    'fade skips a stage': (p) => { p.nextStage = (stage) => (stage === 1 ? stage + 2 : stage + 1); },
    'pools swapped': (p) => { const own = p.poolOfFind.bind(p); p.poolOfFind = () => (own() === 'fixed' ? 'live' : 'fixed'); },
    'gain cap applied to the wrong origin': (p) => { p.capGain = (o, db) => fbsGainCap(o === ORIGIN.fbsLive || o === ORIGIN.fbsFixed ? ORIGIN.user : o, db); },
    'gain cap dropped': (p) => { p.capGain = (_o, db) => db; },
    'slots compacted after a release': (p) => {
      const free = p.release.bind(p);
      p.release = (k) => { free(k); const s = p.slots; for (let i = k; i + 1 < s.length; i++) s[i] = s[i + 1]; free(s.length - 1); };
    },
    'the youngest band answers a merge': (p) => { const own = p.bySeq.bind(p); p.bySeq = () => own().reverse(); },
  };
  for (const [name, override] of Object.entries(arms)) it(name, () => assert.ok(runAll(arm(override)).length > 0, `${name} stayed green`));
  it('the per-origin cap arm is the console\'s for every origin', () => {
    for (const o of ORIGIN_WORDS) for (const db of [-40, -24, -15, -0.5, 0, 3, 15, 20]) {
      const fbs = o === 'fbs-fixed' || o === 'fbs-live';
      const want = fbs ? Math.min(0, Math.max(FBS_MAX_CUT_DB, db)) : Math.min(EQ_GAIN_RANGE.max, Math.max(EQ_GAIN_RANGE.min, db));
      assert.equal(fbsGainCap(ORIGIN_WORDS.indexOf(o), db), want, `${o} ${db}`);
    }
  });
});
