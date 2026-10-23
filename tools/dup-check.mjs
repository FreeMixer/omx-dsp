#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// dup-check.mjs — a token-shingle clone check over include/omxdsp (FreeMixer/omx-dsp#138): comments
// stripped, numeric and string literals normalised, a run of DUP_MIN_TOKENS (30) or more identical
// tokens shared between two locations is a duplicate region. Two locations may be the same file.
//
// The check is shrink-only: test/golden/dup-regions-baseline.json holds, per file pair, the duplicate
// token count seen the day the gate was added (FreeMixer/omx-dsp#138's own 422-region survey). A run
// may find FEWER tokens duplicated between a pair than the baseline (progress) but never MORE (a new
// or grown duplicate), and never a pair the baseline does not know.
//
//   node tools/dup-check.mjs [--check] [dir]        the check against the committed baseline (make lint)
//   node tools/dup-check.mjs --update [dir]         rewrite the baseline from today's regions
//   node tools/dup-check.mjs --self-test            the engine and the shrink-only gate, on a planted tree
import { readFileSync, writeFileSync, readdirSync, statSync, mkdtempSync, rmSync, mkdirSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const BASELINE = join(ROOT, 'test/golden/dup-regions-baseline.json');
const DUP_MIN_TOKENS = 30;

/** Every `.h`/`.c` file under `dir`, sorted, relative paths kept in `root`-relative form by the caller. */
function listSourceFiles(dir) {
  const out = [];
  const walk = (d) => {
    for (const name of readdirSync(d).sort()) {
      const p = join(d, name);
      const st = statSync(p);
      if (st.isDirectory()) walk(p);
      else if (/\.(h|c)$/.test(name)) out.push(p);
    }
  };
  walk(dir);
  return out;
}

/** `text` with `//` and `/* *\/` comments blanked, newlines kept so line numbers do not move. */
function stripComments(text) {
  let out = '';
  for (let i = 0; i < text.length; i++) {
    if (text[i] === '/' && text[i + 1] === '/') {
      while (i < text.length && text[i] !== '\n') i++;
      out += '\n';
      continue;
    }
    if (text[i] === '/' && text[i + 1] === '*') {
      i += 2;
      while (i < text.length && !(text[i] === '*' && text[i + 1] === '/')) {
        out += text[i] === '\n' ? '\n' : ' ';
        i++;
      }
      i += 1;
      out += ' ';
      continue;
    }
    out += text[i];
  }
  return out;
}

const TOKEN_RE = /\n|[A-Za-z_][A-Za-z0-9_]*|0[xX][0-9a-fA-F]+[uUlL]*|\d+\.?\d*(?:[eE][+-]?\d+)?[fFuUlL]*|\.\d+(?:[eE][+-]?\d+)?[fF]?|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|==|!=|<=|>=|&&|\|\||->|::|<<|>>|\+=|-=|\*=|\/=|\+\+|--|./g;

/** `text` (comments already stripped) as `{tok, line}` tokens: identifiers and keywords as-is,
 *  numbers normalised to `N`, string/char literals to `S`, one multi-char or single-char token
 *  otherwise. Whitespace carries no token of its own. */
function tokenize(text) {
  const toks = [];
  let line = 1;
  for (const m of text.matchAll(TOKEN_RE)) {
    const s = m[0];
    if (s === '\n') { line++; continue; }
    if (/^\s/.test(s)) continue;
    if (/^[A-Za-z_]/.test(s)) { toks.push({ tok: s, line }); continue; }
    if (/^[0-9.]/.test(s)) { toks.push({ tok: 'N', line }); continue; }
    if (s[0] === '"' || s[0] === "'") { toks.push({ tok: 'S', line }); continue; }
    if (/^\s*$/.test(s)) continue;
    toks.push({ tok: s, line });
  }
  return toks;
}

/**
 * Every duplicate region of `DUP_MIN_TOKENS` tokens or more found across the files under `dir`.
 * @param {string} dir
 * @returns {{fileA: string, fileB: string, tokens: number}[]} one entry per (file, file) pair with
 *   `tokens` the most tokens found duplicated between them in one contiguous run.
 */
export function findDuplicateRegions(dir) {
  const files = listSourceFiles(dir);
  /** @type {{tok: string, line: number, file: number}[]} */
  const all = [];
  const bounds = []; // [start, end) token index per file, in `files` order
  for (let fi = 0; fi < files.length; fi++) {
    const start = all.length;
    const toks = tokenize(stripComments(readFileSync(files[fi], 'utf8')));
    for (const t of toks) all.push({ tok: t.tok, line: t.line, file: fi });
    bounds.push([start, all.length]);
  }
  const fileOf = (i) => all[i].file;
  const crossesBoundary = (i) => fileOf(i) !== fileOf(i + DUP_MIN_TOKENS - 1);

  const shingles = new Map(); // key -> [start indices]
  for (let i = 0; i + DUP_MIN_TOKENS <= all.length; i++) {
    if (crossesBoundary(i)) continue;
    let key = '';
    for (let k = 0; k < DUP_MIN_TOKENS; k++) key += all[i + k].tok + '\u0001';
    let bucket = shingles.get(key);
    if (!bucket) shingles.set(key, (bucket = []));
    bucket.push(i);
  }

  // Group seed pairs by (fileA, fileB, offset): same offset + consecutive seeds chain into one run.
  const groups = new Map();
  for (const bucket of shingles.values()) {
    if (bucket.length < 2) continue;
    for (let x = 0; x < bucket.length; x++) {
      for (let y = x + 1; y < bucket.length; y++) {
        const a = bucket[x], b = bucket[y]; // a < b
        if (fileOf(a) === fileOf(b) && b - a < DUP_MIN_TOKENS) continue; // trivial self-overlap
        const key = `${fileOf(a)}|${fileOf(b)}|${a - b}`;
        let g = groups.get(key);
        if (!g) groups.set(key, (g = []));
        g.push(a);
      }
    }
  }

  const best = new Map(); // "fileA|fileB" -> most tokens duplicated in one run
  for (const [key, positions] of groups) {
    const [, , offStr] = key.split('|');
    const offset = Number(offStr);
    positions.sort((p, q) => p - q);
    let runStart = positions[0], prev = positions[0];
    const flush = (end) => {
      const length = end - runStart + DUP_MIN_TOKENS;
      const fa = fileOf(runStart), fb = fileOf(runStart - offset);
      const pairKey = `${files[fa]}|${files[fb]}`;
      best.set(pairKey, Math.max(best.get(pairKey) ?? 0, length));
    };
    for (let i = 1; i < positions.length; i++) {
      if (positions[i] !== prev + 1) { flush(prev); runStart = positions[i]; }
      prev = positions[i];
    }
    flush(prev);
  }

  const regions = [];
  for (const [pairKey, tokens] of best) {
    const [fileA, fileB] = pairKey.split('|');
    regions.push({ fileA: relative(dir, fileA), fileB: relative(dir, fileB), tokens });
  }
  regions.sort((a, b) => (a.fileA + a.fileB).localeCompare(b.fileA + b.fileB));
  return regions;
}

/** Baseline shape: `{ "fileA|fileB": tokens }`, sorted keys. */
function toBaseline(regions) {
  const out = {};
  for (const r of regions) out[`${r.fileA}|${r.fileB}`] = r.tokens;
  return out;
}

/**
 * The shrink-only gate: every pair `regions` reports must be in `baseline` with at least as many
 * tokens as `regions` found; a pair absent from `baseline` is treated as a baseline of 0.
 * @returns {string[]} one line per pair that grew or is new; empty when the gate holds.
 */
export function checkShrinkOnly(regions, baseline) {
  const problems = [];
  for (const r of regions) {
    const key = `${r.fileA}|${r.fileB}`;
    const allowed = baseline[key] ?? 0;
    if (r.tokens > allowed) {
      problems.push(`${key}: ${r.tokens} tokens duplicated, baseline allows ${allowed}`);
    }
  }
  return problems;
}

function selfTest() {
  const dir = mkdtempSync(join(tmpdir(), 'dup-check-'));
  let fail = 0;
  try {
    mkdirSync(join(dir, 'sub'), { recursive: true });
    const shared = [
      'static inline double thirty_tokens(double zeta, double w) {',
      '  const double e = exp(-zeta * w);',
      '  const double th = sqrt(1.0 - zeta * zeta) * w;',
      '  const double ec = e * cos(th);',
      '  const double es = e * sin(th);',
      '  return ec + es + th + 42.0;',
      '}',
    ].join('\n');
    writeFileSync(join(dir, 'a.h'), `// a comment that should vanish\n${shared}\n`);
    writeFileSync(join(dir, 'sub', 'b.h'), `/* another comment */\n${shared.replace('42.0', '99')}\n`);
    writeFileSync(join(dir, 'c.h'), 'static inline int unrelated(int x) { return x + 1; }\n');

    let regions = findDuplicateRegions(dir);
    const pairKey = (a, b) => regions.find(
      (r) => (r.fileA === a && r.fileB === b) || (r.fileA === b && r.fileB === a),
    );
    const found = pairKey('a.h', 'sub/b.h');
    if (!found || found.tokens < DUP_MIN_TOKENS) {
      console.error(`dup-check --self-test: FAIL — the planted clone (comments stripped, a literal changed) was not found, got: ${JSON.stringify(regions)}`);
      fail = 1;
    }
    if (pairKey('a.h', 'c.h') || pairKey('sub/b.h', 'c.h')) {
      console.error('dup-check --self-test: FAIL — unrelated.h was named as a clone of something');
      fail = 1;
    }

    // The shrink-only gate: equal to baseline passes, below baseline passes, above fails, unknown pair fails.
    const exact = toBaseline(regions);
    if (checkShrinkOnly(regions, exact).length !== 0) {
      console.error('dup-check --self-test: FAIL — a run equal to its own baseline was refused');
      fail = 1;
    }
    const generous = { ...exact };
    for (const k of Object.keys(generous)) generous[k] += 5;
    if (checkShrinkOnly(regions, generous).length !== 0) {
      console.error('dup-check --self-test: FAIL — a run strictly under baseline was refused (shrinkage must pass)');
      fail = 1;
    }
    const stingy = { ...exact };
    for (const k of Object.keys(stingy)) stingy[k] = Math.max(0, stingy[k] - 1);
    if (checkShrinkOnly(regions, stingy).length === 0) {
      console.error('dup-check --self-test: FAIL — a region one token larger than its baseline was not refused');
      fail = 1;
    }
    if (checkShrinkOnly(regions, {}).length === 0) {
      console.error('dup-check --self-test: FAIL — a pair absent from the baseline entirely was not refused');
      fail = 1;
    }

    // Restore (shrink) the duplicate below DUP_MIN_TOKENS and the pair must stop being reported.
    writeFileSync(join(dir, 'sub', 'b.h'), 'static inline int also_unrelated(int y) { return y - 1; }\n');
    regions = findDuplicateRegions(dir);
    if (pairKey('a.h', 'sub/b.h')) {
      console.error('dup-check --self-test: FAIL — a restored (de-duplicated) file was still named');
      fail = 1;
    }
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
  if (!fail) console.log('dup-check --self-test: a planted clone across files and a sub-directory is found past comments and a literal; the shrink-only gate passes equal or smaller, refuses larger or unknown, and a restored copy is not named');
  return fail;
}

const args = process.argv.slice(2);
if (args[0] === '--self-test') {
  process.exit(selfTest());
}
const update = args[0] === '--update';
const dirArg = args.find((a) => !a.startsWith('--')) ?? join(ROOT, 'include/omxdsp');
const regions = findDuplicateRegions(dirArg);

if (update) {
  writeFileSync(BASELINE, `${JSON.stringify(toBaseline(regions), null, 2)}\n`);
  console.log(`dup-check: wrote ${BASELINE} (${regions.length} region(s) over ${dirArg})`);
  process.exit(0);
}

const baseline = JSON.parse(readFileSync(BASELINE, 'utf8'));
const problems = checkShrinkOnly(regions, baseline);
if (problems.length) {
  console.error('dup-check: a duplicate region grew past test/golden/dup-regions-baseline.json (run --update once it is a deliberate shrink, never to cover a growth):');
  for (const p of problems) console.error(`  ${p}`);
  process.exit(1);
}
console.log(`dup-check: ${regions.length} region(s) over ${dirArg}, none past test/golden/dup-regions-baseline.json`);
