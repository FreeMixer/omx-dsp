#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The proofs over this library's definitions that omx-contract ran until they moved here, against this tree's live
 * data/ and recipes/answers/ with omx-contract's own reader and renderer (the checkout named on the command line):
 *
 *   round-trip    data/, rendered with omx-contract's data as omx_contract_limits.h, holds every unit this library's
 *                 v0.1.3 header held (git show v0.1.3:include/omxdsp/omx_contract_limits.h) byte for byte and in
 *                 its order, and every unit v0.1.3 read in include/, src/, test/ or its Makefile. A unit v0.1.3 lacks
 *                 belongs to an item a later release added (its answer says `since`); a unit whose value moved after
 *                 v0.1.3 is named in MOVED_AFTER_PIN with its reason, and the list is exact.
 *   completeness  every kernel of data/kernels/ has every artifact omx-contract's recipes/kernel.recipe.json lists,
 *                 checked in a scratch tree of data/, these answers and their renders.
 *
 *   node tools/contract-proof.mjs <omx-contract checkout> [--self-test]
 *
 * --self-test plants an answer change (a later item's `since` set to the first release) and a moved value, and
 * fails unless the round-trip goes red on each. Exit 0 both proofs hold, 1 one fails, 2 usage.
 */
import { execFileSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PIN_TAG = 'v0.1.3';
const PIN_HEADER = 'include/omxdsp/omx_contract_limits.h';

/**
 * The units whose value moved after v0.1.3, each with the reason. The definitions declare the value in use today;
 * the v0.1.3 header keeps the value of its day.
 */
export const MOVED_AFTER_PIN = new Map([
  ['OMX_INPUT_ALIGN_DELAY_RANGE_STEP', 'the input alignment step went from 0.01 ms to 0.0001 ms when the alignment line became a fractional read'],
  ['OMX_INPUT_ALIGN_DELAY_RANGE_STEP_DOUBLE', 'the same step, as a double'],
]);

/** The units a first-release item gained after v0.1.3 (a sheet's new field), each with the reason. */
export const ADDED_AFTER_PIN = new Map([
  ['OMX_PITCH_PREFILTER_NYQUIST_FRACTION', '1366d77: the pitch pre-filter corner is also capped at this fraction of Nyquist, so it stays off Nyquist at 32 kHz'],
  ['OMX_PITCH_PREFILTER_NYQUIST_FRACTION_DOUBLE', 'the same fraction, as a double'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_FAST_MS', 'b5bfafe: the release mode became fixed, opto or auto, and auto took its own profile'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_SLOW_MIN_MS', 'the same profile'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_SLOW_MAX_MS', 'the same profile'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_SHARE', 'the same profile'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_SHARE_DOUBLE', 'the same profile, as a double'],
]);

/**
 * The release line `since` is read in: omx-contract's, frozen at the move. An item first shipped before the definitions
 * moved here says the omx-contract release that carried it (1.0.0 to MOVED_AT); one first shipped after says AFTER_MOVE,
 * and its origin sentence names the omx-dsp release and commit. recipes/README.md states the rule.
 */
export const MOVED_AT = '2.14.0';
export const AFTER_MOVE = '3.0.0';

/** The units v0.1.3 read that the definitions no longer carry, each with the reason. */
export const DROPPED_AFTER_PIN = new Map();

const OC = process.argv[2];
if (!OC || !existsSync(join(OC, 'lib/data.mjs'))) {
  console.error('usage: node tools/contract-proof.mjs <omx-contract checkout> [--self-test]');
  process.exit(2);
}
const oc = (p) => import(pathToFileURL(join(resolve(OC), p)).href);
const { loadData, resolveData, entriesOf, packageVersion, TOOL_FILES } = await oc('lib/data.mjs');
const { renderC } = await oc('render/c.mjs');
const { renderJson } = await oc('render/json.mjs');
const { renderTs } = await oc('render/ts.mjs');
const { limitUnits } = await oc('tools/proof/limit-units.mjs');
const { completeness, itemSince, loadRecipe } = await oc('tools/kernel-recipe.mjs');

/** Every `.json` under `dir`, as `{ 'data/<rel>': text }`, the tool files at its top left out. */
function textsUnder(dir, pre = '') {
  const out = {};
  for (const f of existsSync(dir) ? readdirSync(dir, { withFileTypes: true }) : []) {
    const rel = pre ? `${pre}/${f.name}` : f.name;
    if (f.isDirectory()) Object.assign(out, textsUnder(join(dir, f.name), rel));
    else if (f.name.endsWith('.json') && !(pre === '' && TOOL_FILES.includes(f.name))) out[`data/${rel}`] = readFileSync(join(dir, f.name), 'utf8');
  }
  return out;
}

const git = (...a) => execFileSync('git', ['-C', HERE, ...a], { encoding: 'utf8', maxBuffer: 1 << 28 });

/** v0.1.3's header and the units of it that v0.1.3's own include/, src/, test/ and Makefile read. */
function pin() {
  const header = git('show', `${PIN_TAG}:${PIN_HEADER}`);
  const keys = [...header.matchAll(/^#define (OMX_\w+)/gm)].map((m) => m[1]);
  const files = git('ls-tree', '-r', '--name-only', PIN_TAG).split('\n')
    .filter((f) => (/^(include|src|test)\//.test(f) || f === 'Makefile') && f !== PIN_HEADER);
  const code = files.map((f) => git('show', `${PIN_TAG}:${f}`).replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '')).join('\n');
  return { header, read: keys.filter((k) => new RegExp(`\\b${k}\\b`).test(code)) };
}

const units = (t) => limitUnits(t).filter((u) => u.frame === undefined);
const text = (u) => u.lines.join('\n');

/** The items a later release added: an answered item whose `since` is not the first release, or a data item's own. */
function addedLater(answersDir, texts, first) {
  const out = new Set();
  for (const f of readdirSync(answersDir)) {
    const a = JSON.parse(readFileSync(join(answersDir, f), 'utf8'));
    for (const it of a.items) if (itemSince(a, it) !== first || (it.renderedSince ?? first) !== first) out.add(it.name);
  }
  for (const [rel, t] of Object.entries(texts)) {
    if (/^data\/kernels\//.test(rel)) continue;
    for (const [name, it] of entriesOf(rel, JSON.parse(t))) if (it.since !== undefined && it.since !== first) out.add(name);
  }
  return out;
}

const vcmp = (a, b) => { const x = a.split('.').map(Number), y = b.split('.').map(Number); for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1; return 0; };
const inLine = (v) => /^\d+\.\d+\.\d+$/.test(v) && (v === AFTER_MOVE || (vcmp(v, '1.0.0') >= 0 && vcmp(v, MOVED_AT) <= 0));

/** Every `since` of this library's (an answer's, an answered item's, a data item's) outside the release line, as `<where>: <since>`. */
function sinceProblems(answersDir, texts) {
  const bad = [];
  for (const f of readdirSync(answersDir)) {
    const a = JSON.parse(readFileSync(join(answersDir, f), 'utf8'));
    if (!inLine(a.since ?? '')) bad.push(`${f}: ${a.since}`);
    for (const it of a.items) if (it.since !== undefined && !inLine(it.since)) bad.push(`${f} ${it.name}: ${it.since}`);
  }
  for (const [rel, t] of Object.entries(texts)) for (const [name, it] of entriesOf(rel, JSON.parse(t))) if (it.since !== undefined && !inLine(it.since)) bad.push(`${rel} ${name}: ${it.since}`);
  return bad;
}

/** The round-trip over `dataDir` and `answersDir`. Returns the report; `ok` is the verdict. */
export function roundTrip({ dataDir = join(HERE, 'data'), answersDir = join(HERE, 'recipes/answers'), moved = MOVED_AFTER_PIN, added = ADDED_AFTER_PIN, dropped = DROPPED_AFTER_PIN } = {}) {
  const first = loadRecipe(resolve(OC)).tree.firstRelease;
  const texts = { ...textsUnder(join(resolve(OC), 'data')), ...textsUnder(dataDir) };
  const resolved = resolveData(loadData(null, texts));
  const limits = (r) => renderC(r).find((f) => f.path === 'omxcontract/omx_contract_limits.h').text;
  const { header, read } = pin();
  const P = units(header);
  const S = units(limits(resolved));
  const pinned = new Map(P.map((u) => [u.key, u]));
  const ours = new Set(S.map((u) => u.key));
  const later = addedLater(answersDir, texts, first);
  const firstRelease = new Set(units(limits(new Map([...resolved].filter(([n]) => !later.has(n))))).map((u) => u.key));
  const unequal = S.filter((u) => pinned.has(u.key) && text(pinned.get(u.key)) !== text(u)).map((u) => u.key);
  const movedHere = unequal.filter((k) => moved.has(k) && !firstRelease.has(k));
  const differing = unequal.filter((k) => !movedHere.includes(k));
  const movedStale = [...moved.keys()].filter((k) => !movedHere.includes(k));
  const addedHere = S.filter((u) => !pinned.has(u.key) && firstRelease.has(u.key) && added.has(u.key)).map((u) => u.key);
  const notInPin = S.filter((u) => !pinned.has(u.key) && firstRelease.has(u.key) && !added.has(u.key)).map((u) => u.key);
  const addedStale = [...added.keys()].filter((k) => !addedHere.includes(k));
  const sinceOff = sinceProblems(answersDir, textsUnder(dataDir));
  const inOrder = JSON.stringify(P.filter((u) => ours.has(u.key)).map((u) => u.key)) === JSON.stringify(S.filter((u) => pinned.has(u.key)).map((u) => u.key));
  const readMissing = read.filter((k) => !ours.has(k) && !dropped.has(k));
  const droppedStale = [...dropped.keys()].filter((k) => ours.has(k) || !read.includes(k));
  const ok = !differing.length && !movedStale.length && !notInPin.length && !addedStale.length && inOrder && !readMissing.length && !droppedStale.length && !sinceOff.length;
  return { ok, units: S.length, pinUnits: P.length, read: read.length, differing, moved: movedHere, movedStale, added: addedHere, addedStale, notInPin, inOrder, readMissing, droppedStale, sinceOff };
}

/** A scratch tree of omx-contract's schema and recipe, this tree's data and answers, and their renders. */
function completenessTree() {
  const dir = mkdtempSync(join(tmpdir(), 'omx-dsp-contract-proof-'));
  for (const p of ['schema', 'recipes', 'package.json']) cpSync(join(resolve(OC), p), join(dir, p), { recursive: true });
  rmSync(join(dir, 'recipes/answers'), { recursive: true, force: true });
  cpSync(join(resolve(OC), 'data'), join(dir, 'data'), { recursive: true });
  cpSync(join(HERE, 'data'), join(dir, 'data'), { recursive: true });
  cpSync(join(HERE, 'recipes/answers'), join(dir, 'recipes/answers'), { recursive: true });
  const resolved = resolveData(loadData(join(dir, 'data')));
  const files = [
    ...renderC(resolved).map((f) => ['include', f]),
    ...renderJson(resolved, packageVersion(resolve(OC))).map((f) => ['share/omx-contract', f]),
    ...renderTs(resolved).map((f) => ['test/golden/ts', f]),
  ];
  for (const [d, f] of files) {
    mkdirSync(dirname(join(dir, d, f.path)), { recursive: true });
    writeFileSync(join(dir, d, f.path), f.text);
  }
  return dir;
}

function report(r) {
  console.log(`round-trip: ${r.units} units, ${r.pinUnits} in ${PIN_TAG}'s header, order ${r.inOrder ? 'kept' : 'BROKEN'}, ${r.read - r.readMissing.length}/${r.read} read units carried`);
  if (r.moved.length) console.log(`  moved after ${PIN_TAG}, declared as in use today: ${r.moved.join(', ')}`);
  if (r.added.length) console.log(`  added to a first-release item after ${PIN_TAG}: ${r.added.join(', ')}`);
  for (const [k, v] of [['differing from the pin', r.differing], ['not in the pin and of no later item', r.notInPin], ['named as moved but not', r.movedStale],
    ['named as added but not', r.addedStale], ['read by the pin and no longer carried', r.readMissing], ['named as dropped but not', r.droppedStale],
    [`a since outside the release line (1.0.0..${MOVED_AT}, or ${AFTER_MOVE} after the move)`, r.sinceOff]]) if (v.length) console.log(`  ${k}: ${v.join(', ')}`);
  console.log(`round-trip: ${r.ok ? 'HOLDS' : 'FAILS'}`);
}

if (process.argv.includes('--self-test')) {
  const t = mkdtempSync(join(tmpdir(), 'omx-dsp-contract-proof-st-'));
  let bad = 0;
  const red = (what, r, why) => {
    if (r.ok || !why(r)) { console.log(`contract-proof: FAIL: ${what} left the round-trip green or red for another reason`); bad = 1; }
    else console.log(`contract-proof: self-test: ${what} is red`);
  };
  try {
    // a later item's answer moved to the first release: its units are now first-release units v0.1.3 lacks
    cpSync(join(HERE, 'recipes/answers'), join(t, 'answers'), { recursive: true });
    const f = join(t, 'answers', 'trim.json');
    const a = JSON.parse(readFileSync(f, 'utf8'));
    a.since = loadRecipe(resolve(OC)).tree.firstRelease;
    for (const it of a.items) delete it.since;
    writeFileSync(f, JSON.stringify(a, null, 2));
    red('an answer whose since is planted back to the first release', roundTrip({ answersDir: join(t, 'answers') }), (r) => r.notInPin.some((k) => k.startsWith('OMX_TRIM_')));
    red('a moved unit left out of MOVED_AFTER_PIN', roundTrip({ moved: new Map() }), (r) => r.differing.includes('OMX_INPUT_ALIGN_DELAY_RANGE_STEP'));
    const g = join(t, 'answers', 'width.json');
    writeFileSync(f, readFileSync(join(HERE, 'recipes/answers', 'trim.json')));
    const w = JSON.parse(readFileSync(g, 'utf8'));
    w.since = '0.27.0';
    writeFileSync(g, JSON.stringify(w, null, 2));
    red('an answer whose since is an omx-dsp release', roundTrip({ answersDir: join(t, 'answers') }), (r) => r.sinceOff.some((x) => x.startsWith('width.json')));
    red('a unit named as added that was not', roundTrip({ added: new Map([...ADDED_AFTER_PIN, ['OMX_COMP_RATIO_MIN', 'planted']]) }), (r) => r.addedStale.includes('OMX_COMP_RATIO_MIN'));
    red('a dropped unit left unnamed', roundTrip({ dropped: new Map() }), (r) => r.readMissing.includes('OMX_DELAY_TIME_MS_MAX'));
    red('a unit named as moved that did not move', roundTrip({ moved: new Map([...MOVED_AFTER_PIN, ['OMX_COMP_RATIO_MIN', 'planted']]) }), (r) => r.movedStale.includes('OMX_COMP_RATIO_MIN'));
  } finally {
    rmSync(t, { recursive: true, force: true });
  }
  process.exit(bad);
}

const r = roundTrip();
report(r);
const tree = completenessTree();
let v;
try {
  v = completeness(tree);
} finally {
  rmSync(tree, { recursive: true, force: true });
}
for (const l of v.lines) console.log(l);
for (const g of v.gaps) console.log(`  GAP ${g}`);
console.log(v.ok ? 'completeness: every kernel has every artifact of the recipe' : `completeness: ${v.gaps.length} gap(s)`);
process.exit(r.ok && v.ok ? 0 : 1);
