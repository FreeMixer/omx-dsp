#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// fbs-place-console.mjs — runs the console's own placement, openmixer's FbsCorrector
// (packages/server/src/fbs-corrector.ts, with core's fbs.ts and eq.ts clamps), from a checkout under node's type
// stripping, over every vector of test/analysis/fixtures/fbs_place.vectors, and holds it to the values the file
// states: the C placement (test/analysis/fbs_place.test.c) and its TypeScript twin (ts/test/fbs-place.test.mjs)
// are held to the same file, so a vector the console misses is a place where the port and the console differ.
// A desk check, like room-ts-golden: the console is not a build dependency.
//
//   node --experimental-strip-types tools/fbs-place-console.mjs <openmixer> <contract-ts> [<vectors>]
//
// <contract-ts> holds the TypeScript render of a contract directory (omx-contract's, this tree's data/ and
// omx-clap-core's), `omx-contract.js`, which the console's declarations read as their generated
// `omx-contract/omx-contract.js`. The console has slots no more than an EQ list in plant order, so a `slot` line
// is held as "a band with these words exists", `free` is not judged, and an action is its word and frequency.
// The EQ store is the console's rules for the three writes the placement makes (add, set gain, remove), with the
// console's own clamps; the timers are the placement's interval steps fired in due order on the vector's clock.
// Exit 0 every vector held; 1 a vector the console misses (each printed); 2 usage.
import { register } from 'node:module';
import { join, resolve } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { readFileSync } from 'node:fs';

const [openmixerArg, contractTsArg, vectorsArg] = process.argv.slice(2);
if (!openmixerArg || !contractTsArg) {
  console.error('usage: fbs-place-console.mjs <openmixer> <contract-ts>');
  process.exit(2);
}
const HERE = fileURLToPath(new URL('..', import.meta.url));
const openmixer = resolve(openmixerArg);
const cents = join(HERE, 'ts', 'dist', 'fbs-place.js');
register('./fbs-place-console-hooks.mjs', import.meta.url, { data: { openmixer, contractTs: resolve(contractTsArg), cents } });
const core = await import(pathToFileURL(`${openmixer}/packages/core/src/fbs.ts`).href);
const eq = await import(pathToFileURL(`${openmixer}/packages/core/src/eq.ts`).href);
const { FbsCorrector } = await import(pathToFileURL(`${openmixer}/packages/server/src/fbs-corrector.ts`).href);
const { EQ_MAX_BANDS } = await import(pathToFileURL(`${openmixer}/packages/declarations/src/index.ts`).href);

const text = readFileSync(vectorsArg ?? join(HERE, 'test', 'analysis', 'fixtures', 'fbs_place.vectors'), 'utf8');
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
  else cur.lines.push(w);
}
const num = (w) => (w === 'nan' ? NaN : Number(w));
const near = (a, b, t) => (Number.isNaN(b) ? Number.isNaN(a) : Math.abs(a - b) <= t);

/** The console store's rules for the writes the placement makes (channel-eq-store.ts newBand, applyBand, removeBand). */
class Store {
  bands = [];
  next = 1;
  eqFor() { return { bands: this.bands }; }
  addBand(_ch, seed) {
    if (this.bands.length >= EQ_MAX_BANDS) return { bands: this.bands };
    const type = seed.type ?? 'bell';
    const band = {
      id: this.next++, type, freqHz: eq.clampEqFreq(seed.freqHz ?? 1000),
      gainDb: seed.tag !== undefined && core.isFbsTag(seed.tag) ? core.clampFbsCut(seed.gainDb ?? 0) : eq.clampEqGain(seed.gainDb ?? 0),
      q: eq.clampEqQFor(type, seed.q ?? 1), on: true, tag: seed.tag,
    };
    this.bands.push(band);
    return { bands: this.bands };
  }
  applyEqSet(_ch, set) {
    const b = this.bands.find((x) => x.id === set.band);
    if (b && set.param === 'gainDb') b.gainDb = core.isFbsTag(b.tag) ? core.clampFbsCut(set.value) : eq.clampEqGain(set.value);
  }
  removeBand(_ch, id) { this.bands = this.bands.filter((b) => b.id !== id); }
}

const ch = { kind: 'input', index: 1 };
let failed = 0;
let held = 0;
for (const v of vectors) {
  const store = new Store();
  let clock = 0;
  let timerId = 0;
  const timers = new Map();
  const log = [];
  const fbs = new FbsCorrector({
    eq: store, announceEq: () => {}, onState: () => {}, now: () => clock,
    onAction: (_c, action, freqHz) => log.push(`${action} ${freqHz}`),
    setInterval: (fn, ms) => { const id = ++timerId; timers.set(id, { fn, ms, due: clock + ms }); return id; },
    clearInterval: (id) => timers.delete(id),
  });
  const advance = (t) => {
    for (;;) {
      let best = null;
      for (const [id, tm] of timers) if (tm.due <= t && (!best || tm.due < best[1].due || (tm.due === best[1].due && id < best[0]))) best = [id, tm];
      if (!best) break;
      clock = best[1].due;
      best[1].due += best[1].ms;
      best[1].fn();
    }
    if (t > clock) clock = t;
  };
  const rings = (w, from) => {
    const c = [];
    const s = [];
    for (let i = from; i < w.length; ) {
      if (w[i] === 'c') { c.push({ freqHz: num(w[i + 1]), depthDb: num(w[i + 2]), q: 116, pool: 'live' }); i += 3; }
      else { s.push({ freqHz: num(w[i + 1]), depthDb: 0, q: 116, pool: 'live' }); i += 2; }
    }
    return { c, s };
  };
  const tick = (t, r) => { advance(t); fbs.onTick(ch, r.s, r.c); };
  fbs.set(ch, { mode: 'live' });
  let seen = 0;
  const fail = (w, got) => { failed++; console.log(`${v.name}: ${w.join(' ')}: the console reads ${got}`); };
  for (const w of v.lines) {
    if (['config', 'slot', 'count', 'log', 'cap'].includes(w[0])) held++;
    switch (w[0]) {
      case 'pool': fbs.set(ch, { mode: w[1] === 'fixed' ? 'ringout' : 'live' }); break;
      case 'set': fbs.set(ch, { holdSeconds: w[1] === 'never' ? 'never' : num(w[1]), maxAutoBands: num(w[2]), notchQ: num(w[3]), mergeCents: num(w[4]), cooldownSeconds: num(w[5]) }); break;
      case 'config': {
        const s = fbs.stateFor(ch);
        const got = [s.holdSeconds, s.maxAutoBands, s.notchQ, s.mergeCents, s.cooldownSeconds].join(' ');
        const want = [w[1] === 'never' ? 'never' : num(w[1]), num(w[2]), num(w[3]), num(w[4]), num(w[5])].join(' ');
        if (got !== want) fail(w, got);
        break;
      }
      case 'tick': tick(num(w[1]), rings(w, 2)); break;
      case 'ticks': { const r = rings(w, 4); for (let t = num(w[1]); t <= num(w[2]); t += num(w[3])) tick(t, r); break; }
      case 'advance': advance(num(w[1])); break;
      case 'slot': {
        const hit = store.bands.find((b) => b.freqHz === num(w[2]) && near(b.gainDb, num(w[3]), tol.gain) && b.q === num(w[4]) && b.on === (w[5] === '1') && b.tag === w[6] && b.type === w[7]);
        if (!hit) fail(w, JSON.stringify(store.bands.map((b) => [b.freqHz, b.gainDb, b.q, b.tag, b.type])));
        break;
      }
      case 'free': break;
      case 'count': if (fbs.autoBandCount(ch) !== num(w[1])) fail(w, fbs.autoBandCount(ch)); break;
      case 'log': {
        const got = log.slice(seen).join(' ');
        const want = [];
        for (let i = 1; i < w.length; i += 3) want.push(`${w[i]} ${num(w[i + 1])}`);
        if (got !== want.join(' ')) fail(w, `[${got}]`);
        seen = log.length;
        break;
      }
      case 'cap': {
        const tag = w[1] === 'user' ? undefined : w[1];
        const got = tag !== undefined && core.isFbsTag(tag) ? core.clampFbsCut(num(w[2])) : eq.clampEqGain(num(w[2]));
        if (!near(got, num(w[3]), 0)) fail(w, got);
        break;
      }
      case 'cents': break;
      default: fail(w, 'no such line');
    }
  }
  fbs.dispose();
}
console.log(`fbs-place-console: ${vectors.length} vectors, ${held} lines judged, ${failed} the console misses`);
process.exit(failed ? 1 : 0);
