// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The declaration tests' data: omx-contract's own data/ and this library's LIVE data/ as one data directory (the
 * union a consumer installs), its renders, and the helpers that perturb a scratch copy of it. Moved from omx-contract's
 * test/helpers.mjs with the tests, reading this tree instead of a 2.14.0 copy of it.
 */
import { cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { OC, controlGlobal, isKernelFile, isUse, loadData, packageVersion, renderC, renderJson, renderTs, resolveData } from './oc.mjs';

/** omx-contract's checkout: its own data/, schema/, committed renders and release fixtures. */
export const ROOT = OC;
/** omx-contract's own data. */
export const DATA = join(OC, 'data');
/** This library's checkout. */
export const HERE = resolve(dirname(fileURLToPath(import.meta.url)), '../..');

const scratch = [];
process.on('exit', () => { for (const d of scratch) rmSync(d, { recursive: true, force: true }); });

/** A scratch directory in TMPDIR, removed when the test process exits: a test never leaves one behind (#169). */
export function scratchDir(prefix) {
  const dir = mkdtempSync(join(tmpdir(), prefix));
  scratch.push(dir);
  return dir;
}

/** A scratch data directory holding omx-contract's data/ and this library's data/ (schema/ beside it). */
export function scratchUnion() {
  const dir = scratchDir('omx-contract-union-');
  cpSync(DATA, join(dir, 'data'), { recursive: true });
  cpSync(join(HERE, 'data'), join(dir, 'data'), { recursive: true });
  cpSync(join(ROOT, 'schema'), join(dir, 'schema'), { recursive: true });
  return join(dir, 'data');
}

/** The union of omx-contract's data/ and this library's, read like a data directory. */
export const UNION = scratchUnion();

const unionItems = loadData(UNION).items;
/** The file of the union that declares item `name`, as a rel (`data/primitives/butterworth-q.json`). */
export function relOf(name) {
  const it = unionItems.get(name);
  if (!it) throw new Error(`the union declares no ${name}`);
  return it.rel;
}

/** A scratch copy of the union (and schema/ beside it), to perturb; returns its data directory. */
export const scratchData = scratchUnion;

/**
 * The renders of the union, laid out as this tree's (include/, share/omx-contract/, test/golden/ts/): what a
 * perturbed union's `render --check` is held against, as the committed renders hold data/.
 */
export const RENDERS = (() => {
  const dir = scratchDir('omx-contract-renders-');
  const resolved = resolveData(loadData(UNION));
  const files = [
    ...renderC(resolved).map((f) => ['include', f]),
    ...renderJson(resolved, packageVersion(OC)).map((f) => ['share/omx-contract', f]),
    ...renderTs(resolved).map((f) => ['test/golden/ts', f]),
  ];
  for (const [base, f] of files) {
    mkdirSync(dirname(join(dir, base, f.path)), { recursive: true });
    writeFileSync(join(dir, base, f.path), f.text);
  }
  return dir;
})();

/**
 * Where a data file writes the item `name`, to perturb in place. In a kernel file: the control whose item it is (its
 * `travel`, `ids`, `default`, `c` are the item's), the table's or the aggregate's own entry, or the constant; with
 * `TABLE.field`, that field control's travel. In rates.json and primitives.json, the item itself.
 * @throws when the file declares no such item, so a perturbation can never move nothing.
 */
export function declOf(rel, doc, name) {
  const found = (() => {
    if (!isKernelFile(rel)) return doc[name];
    const [head, field] = name.split('.');
    if (field !== undefined) return doc.controls?.find((c) => c.table === head && c.name === field)?.travel;
    return doc.controls?.find((c) => !c.table && !isUse(c) && controlGlobal(rel, c) === head) ?? doc.tables?.[head] ?? doc.aggregates?.[head] ?? doc.constants?.[head];
  })();
  if (found === undefined) throw new Error(`${rel} declares no ${name}`);
  return found;
}

/** Remove the declaration of item `name` from a kernel file (a control, a table with its controls, an aggregate, a constant). */
export function dropDecl(rel, doc, name) {
  declOf(rel, doc, name);
  if (!isKernelFile(rel)) { delete doc[name]; return; }
  doc.controls = (doc.controls ?? []).filter((c) => !(c.table === name || (!c.table && controlGlobal(rel, c) === name)));
  for (const s of ['tables', 'aggregates', 'constants']) if (doc[s]) delete doc[s][name];
}

/**
 * Edit one data file of a scratch copy in place: `fn(doc, at)` mutates the parsed document, `at(name)` being
 * declOf on it.
 */
export function editData(dataDir, rel, fn) {
  const p = join(dataDir, rel);
  const doc = JSON.parse(readFileSync(p, 'utf8'));
  fn(doc, (name) => declOf(rel.startsWith('data/') ? rel : `data/${rel}`, doc, name));
  writeFileSync(p, `${JSON.stringify(doc, null, 2)}\n`);
}

/** The data as `{ rel: text }` with `fn(docsByRel, at)` applied, `at(rel, name)` being declOf, for loadData(null, texts). */
export function texts(fn) {
  const data = loadData(UNION);
  const docs = Object.fromEntries(data.files.map((f) => [f.rel, JSON.parse(f.text)]));
  fn?.(docs, (rel, name) => declOf(rel, docs[rel], name));
  return Object.fromEntries(Object.entries(docs).map(([k, v]) => [k, JSON.stringify(v)]));
}

export const resolvedOf = (t) => resolveData(loadData(null, t));
