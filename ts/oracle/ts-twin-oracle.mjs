// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The kernel half of every agreement test: ts-twin-oracle.c answers a kernel by name for a grid of rows, one
 * process per call (moved here with the twins from the console). It links this checkout's
 * build/libomxdsp.a when `make lib` has built it (CI does), else the installed omx-dsp through pkg-config, and
 * says which on stderr so a run never hides what it measured.
 */
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, renameSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const SRC = join(HERE, 'ts-twin-oracle.c');
const REPO = join(HERE, '..', '..');

/** The compile and link flags: the in-tree library first, the installed one second. */
export const linkage = () => {
  const archive = join(REPO, 'build', 'libomxdsp.a');
  if (existsSync(archive)) return { from: 'tree', flags: `-ffp-contract=off -I'${join(REPO, 'include')}' -o @ '${SRC}' '${archive}' -lm` };
  const pc = execFileSync('pkg-config', ['--cflags', '--libs', 'omxdsp'], { encoding: 'utf8' }).trim();
  return { from: `installed omx-dsp ${execFileSync('pkg-config', ['--modversion', 'omxdsp'], { encoding: 'utf8' }).trim()}`, flags: `-o @ '${SRC}' ${pc}` };
};

let built = '';
export const oraclePath = () => {
  if (built) return built;
  const { from, flags } = linkage();
  const key = createHash('sha256').update(readFileSync(SRC)).update(flags).digest('hex').slice(0, 16);
  const dir = join(tmpdir(), `omx-dsp-ts-oracle-${process.getuid?.() ?? 0}`);
  mkdirSync(dir, { recursive: true });
  const bin = join(dir, `oracle-${key}`);
  if (!existsSync(bin)) {
    const tmp = `${bin}.${process.pid}`;
    execFileSync('sh', ['-c', `cc -Wall -Wextra -Werror -O2 ${flags.replace('-o @', `-o '${tmp}'`)}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    renameSync(tmp, bin);
  }
  process.stderr.write(`ts-twin-oracle: linked against the ${from}\n`);
  built = bin;
  return bin;
};

export const oracleKernels = () =>
  execFileSync(oraclePath(), ['--list'], { encoding: 'utf8' }).split('\n').filter(Boolean);

const wire = (x) => {
  if (Object.is(x, -0)) return '-0';
  if (typeof x === 'boolean') return x ? '1' : '0';
  if (Number.isNaN(x)) return 'nan';
  if (x === Infinity) return 'inf';
  if (x === -Infinity) return '-inf';
  return String(x);
};
const unwire = (t) => {
  const s = t.toLowerCase();
  if (s === 'inf') return Infinity;
  if (s === '-inf') return -Infinity;
  if (s === 'nan' || s === '-nan') return NaN;
  return Number(t);
};

/**
 * Ask the C kernel `kernel` one question per row of `rows`; the answer rows come back in order.
 * A refusal from the oracle (unknown kernel, wrong arity) throws: a silent empty answer would
 * read as agreement with an empty twin.
 */
export const oracle = (kernel, rows) => {
  if (rows.length === 0) throw new Error(`oracle(${kernel}): no rows — an empty grid proves nothing`);
  const input = rows.map((r) => `${kernel} ${r.map(wire).join(' ')}\n`).join('');
  const r = spawnSync(oraclePath(), [], { input, encoding: 'utf8', maxBuffer: 1 << 28 });
  if (r.status !== 0) throw new Error(`oracle(${kernel}) exited ${r.status}: ${r.stderr}`);
  const lines = r.stdout.split('\n').filter(Boolean);
  if (lines.length !== rows.length) throw new Error(`oracle(${kernel}): ${rows.length} questions, ${lines.length} answers`);
  return lines.map((line, i) => {
    const [tag, ...rest] = line.split(' ');
    if (tag !== 'ok') throw new Error(`oracle(${kernel}) row ${i} [${rows[i].join(' ')}]: ${line}`);
    return rest.map(unwire);
  });
};

/**
 * The gap between a twin's number and the kernel's, as the test states its tolerance: a number
 * agrees when it is within `abs` OR within `rel` of the kernel's (the larger of the two bounds), and
 * non-finite answers agree only when they are the same non-finite value. Returns '' on agreement,
 * else a sentence for the failure message.
 */
export const gap = (twin, kernel, { rel = 0, abs = 0 }) => {
  if (Number.isNaN(twin) || Number.isNaN(kernel)) return Number.isNaN(twin) && Number.isNaN(kernel) ? '' : `twin ${twin}, kernel ${kernel}`;
  if (!Number.isFinite(twin) || !Number.isFinite(kernel)) return twin === kernel ? '' : `twin ${twin}, kernel ${kernel}`;
  const d = Math.abs(twin - kernel);
  return d <= Math.max(abs, rel * Math.abs(kernel)) ? '' : `twin ${twin}, kernel ${kernel}, off by ${d} (allowed abs ${abs}, rel ${rel})`;
};

/**
 * Compare two equal-length lists of numbers; returns the failures (an empty list is agreement) and
 * the largest gap seen, so a test can print how close the tolerance sits to the truth.
 */
export const compare = (twin, kernel, tol, label = '') => {
  if (twin.length !== kernel.length) return { failures: [`${label}: twin has ${twin.length} numbers, kernel ${kernel.length}`], worst: Infinity };
  const failures = [];
  let worst = 0;
  for (let i = 0; i < twin.length; i++) {
    const g = gap(twin[i], kernel[i], tol);
    if (g) failures.push(`${label}[${i}]: ${g}`);
    if (Number.isFinite(twin[i]) && Number.isFinite(kernel[i])) worst = Math.max(worst, Math.abs(twin[i] - kernel[i]));
  }
  return { failures, worst };
};

/** The kernel's answers to a grid, asked once so the twin and its sabotage arms read the same ones. */
export const ask = (kernel, rows) => ({ kernel, rows, answers: oracle(kernel, rows) });

/**
 * Run `twin` (row -> list of numbers) over every row of an {@link ask} and compare with the kernel's
 * answer inside `tol`. A twin that throws on a row is a failure of that row. Returns
 * `{ rows, failures, worst }`: the row count, one sentence per disagreement and the largest finite gap.
 */
export const against = (asked, twin, tol) => {
  const failures = [];
  let worst = 0;
  asked.rows.forEach((row, i) => {
    let t;
    try {
      t = twin(row);
    } catch (e) {
      failures.push(`${asked.kernel}(${row.join(', ')}): the twin threw ${e}`);
      return;
    }
    const c = compare(t, asked.answers[i], tol, `${asked.kernel}(${row.join(', ')})`);
    failures.push(...c.failures);
    worst = Math.max(worst, c.worst);
  });
  return { rows: asked.rows.length, failures, worst };
};
