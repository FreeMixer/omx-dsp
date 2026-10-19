#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// ts-golden.mjs — runs openmixer's TypeScript EQ maths (core's eq.ts) from a checkout,
// under node's type stripping, so the C that replaces it is held to what it computes today
// (DSP-results-from-C design, §8 step 2). Node 22.6 or later.
//
//   node tools/ts-golden.mjs curve <openmixer> <contract-ts>
//       eqCurveDb over the corpus below at the nine oracle rates, printed as the C fixture
//       test/kernels/fixtures/eq_curve_ts.h (FreeMixer/omx-dsp#69)
//   node tools/ts-golden.mjs design <openmixer> <contract-ts>
//       every EQ design and curve function openmixer's TypeScript computes (core's eq.ts, the web
//       surface's eqCurveGeom.ts, the catalog's octavesToQ) over the fixed inputs below at the nine
//       oracle rates, printed as the C fixture test/kernels/fixtures/eq_design_ts.h.
//       Needs node's --experimental-transform-types, which it re-runs itself under.
//   node tools/ts-golden.mjs bandwidth-q <openmixer> <contract-ts> <table.json>
//       bandwidthOctavesQ against the C law's table test/golden/eq_bandwidth_q.json: prints the
//       largest gap in ulps, exits 1 past MAX_ULPS (FreeMixer/omx-dsp#41)
//
// <contract-ts> is omx-contract's TypeScript render (`omx-contract render --target ts --out <dir>`).
import { execFileSync, spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { register } from 'node:module';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [cmd, openmixerArg, contractTsArg, tableArg] = process.argv.slice(2);
if (!cmd || !openmixerArg || !contractTsArg) {
  console.error('usage: ts-golden.mjs curve|design|bandwidth-q <openmixer> <contract-ts> [table.json]');
  process.exit(2);
}
if (cmd === 'design' && !process.execArgv.includes('--experimental-transform-types')) {
  const r = spawnSync(process.execPath, ['--experimental-transform-types', '--no-warnings', ...process.execArgv,
    process.argv[1], ...process.argv.slice(2)], { stdio: 'inherit' });
  process.exit(r.status ?? 1);
}
const openmixer = resolve(openmixerArg);
const contractTs = resolve(contractTsArg);
register('./ts-golden-hooks.mjs', import.meta.url, { data: { openmixer, contractTs } });
const eq = await import(pathToFileURL(`${openmixer}/packages/core/src/eq.ts`).href);
const commit = execFileSync('git', ['-C', openmixer, 'rev-parse', 'HEAD']).toString().trim();

/** The nine rates every oracle runs at, read from the contract render the tool is given
 * (omx-contract's ORACLE_FULL_RATES): never a list typed here. */
const { ORACLE_FULL_RATES } = await import(pathToFileURL(`${contractTs}/omx-contract.js`).href);
if (!Array.isArray(ORACLE_FULL_RATES) || ORACLE_FULL_RATES.length === 0) {
  console.error(`ts-golden: ${contractTs}/omx-contract.js exports no ORACLE_FULL_RATES (omx-contract 2.16.0 or later)`);
  process.exit(2);
}

/** The C kind each drawn-band type designs as (enum omx_eq_kind). */
const KIND = {
  bell: 'OMX_EQ_PEAKING', lowshelf: 'OMX_EQ_LOWSHELF', highshelf: 'OMX_EQ_HIGHSHELF',
  highpass: 'OMX_EQ_HIGHPASS', lowpass: 'OMX_EQ_LOWPASS', notch: 'OMX_EQ_NOTCH',
  allpass1: 'OMX_EQ_ALLPASS1', allpass2: 'OMX_EQ_ALLPASS2',
};

/** Every band type, the travel's edges, a Q-116 notch, features near Nyquist. */
const CORPUS = [
  { name: 'flat', bands: [] },
  { name: 'starter', bands: [
    { type: 'lowshelf', freq: 120, gainDb: 4, q: 0.7 }, { type: 'bell', freq: 800, gainDb: -3, q: 1 },
    { type: 'bell', freq: 3000, gainDb: 6, q: 2 }, { type: 'highshelf', freq: 10000, gainDb: -5, q: 0.7 }] },
  { name: 'pass-24', bands: [
    { type: 'highpass', freq: 80, gainDb: 0, q: 0.5411961 }, { type: 'highpass', freq: 80, gainDb: 0, q: 1.306563 },
    { type: 'lowpass', freq: 18000, gainDb: 0, q: 0.5411961 }, { type: 'lowpass', freq: 18000, gainDb: 0, q: 1.306563 }] },
  { name: 'fbs-notch', bands: [
    { type: 'notch', freq: 130, gainDb: 0, q: 116 }, { type: 'bell', freq: 1000, gainDb: 12, q: 0.3 }] },
  { name: 'near-nyquist', bands: [
    { type: 'bell', freq: 19000, gainDb: 9, q: 4 }, { type: 'highshelf', freq: 15000, gainDb: 12, q: 1 },
    { type: 'notch', freq: 17000, gainDb: 0, q: 10 }] },
  { name: 'allpass', bands: [
    { type: 'allpass1', freq: 500, gainDb: 0, q: 1 }, { type: 'allpass2', freq: 2000, gainDb: 0, q: 0.7 },
    { type: 'bell', freq: 40, gainDb: -15, q: 8 }] },
  { name: 'edges', bands: [
    { type: 'bell', freq: 20, gainDb: 15, q: 0.1 }, { type: 'lowshelf', freq: 20000, gainDb: -15, q: 0.3 },
    { type: 'lowpass', freq: 200, gainDb: 0, q: 4 }, { type: 'highpass', freq: 20, gainDb: 0, q: 10 }] },
];
/** Grid points per curve: the evaluator is under test, not the grid's density; the anchors ride on top. */
const POINTS = 24;

const num = (x) => {
  if (!Number.isFinite(x)) throw new Error(`non-finite value ${x}`);
  const s = String(x); // the shortest string that reads back as x: the C compiler rounds it to x
  return /[.e]/.test(s) ? s : `${s}.0`;
};

function curve() {
  const out = [];
  const curves = [];
  const fs = [];
  const dbs = [];
  for (const sr of ORACLE_FULL_RATES) {
    for (const c of CORPUS) {
      const pts = eq.eqCurveDb(c.bands, { sampleRate: sr, points: POINTS });
      curves.push({ sr, c, off: fs.length, n: pts.length });
      for (const p of pts) { fs.push(p.f); dbs.push(p.db); }
    }
  }
  out.push('// SPDX-License-Identifier: GPL-3.0-or-later');
  out.push('// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>');
  out.push(`/* GENERATED by tools/ts-golden.mjs curve from the console's TypeScript: core's eqCurveDb`);
  out.push(` * over ${CORPUS.length} band sets at the nine oracle rates, ${POINTS} grid points`);
  out.push(' * from 20 Hz to 20 kHz plus each band\'s anchors. Do not edit; regenerate. */');
  out.push('#ifndef OMXDSP_TEST_EQ_CURVE_TS_H');
  out.push('#define OMXDSP_TEST_EQ_CURVE_TS_H');
  out.push('');
  out.push('struct ts_band { enum omx_eq_kind kind; double freq, gain_db, q; };');
  out.push('struct ts_curve { const char *name; double sr; uint32_t nb; struct ts_band b[4]; uint32_t off, n; };');
  out.push('');
  out.push(`static const double TS_CURVE_PLOT_FLOOR_DB = -120.0;`);
  out.push(`static const double TS_CURVE_F[${fs.length}] = {`);
  for (let i = 0; i < fs.length; i += 6) out.push(`  ${fs.slice(i, i + 6).map(num).join(', ')},`);
  out.push('};');
  out.push(`static const double TS_CURVE_DB[${dbs.length}] = {`);
  for (let i = 0; i < dbs.length; i += 6) out.push(`  ${dbs.slice(i, i + 6).map(num).join(', ')},`);
  out.push('};');
  out.push(`static const struct ts_curve TS_CURVES[${curves.length}] = {`);
  for (const { sr, c, off, n } of curves) {
    const b = c.bands.map((x) => `{${KIND[x.type]}, ${num(x.freq)}, ${num(x.gainDb)}, ${num(x.q)}}`).join(', ');
    out.push(`  {"${c.name}", ${num(sr)}, ${c.bands.length}u, {${b}}, ${off}u, ${n}u},`);
  }
  out.push('};');
  out.push('');
  out.push('#endif /* OMXDSP_TEST_EQ_CURVE_TS_H */');
  process.stdout.write(`${out.join('\n')}\n`);
}

/** The largest gap the TypeScript copy may keep from the C law, in ulps of Q. */
const MAX_ULPS = 4;

function ulps(a, b) {
  if (a === b) return 0;
  const buf = new DataView(new ArrayBuffer(16));
  buf.setFloat64(0, a);
  buf.setFloat64(8, b);
  return Number(buf.getBigInt64(0) - buf.getBigInt64(8)) * (a > 0 ? 1 : -1);
}

function bandwidthQ() {
  const table = JSON.parse(readFileSync(tableArg, 'utf8'));
  let worst = 0;
  let at = null;
  for (const [sr, f, bw, q] of table.rows) {
    const d = Math.abs(ulps(eq.bandwidthOctavesQ(f, bw, sr), q));
    if (d > worst) { worst = d; at = { sr, f, bw }; }
  }
  console.log(`bandwidthOctavesQ (openmixer ${commit}) against ${table.rows.length} rows of ${table.law}: ` +
    `largest gap ${worst} ulp${at ? ` at ${JSON.stringify(at)}` : ''}, allowed ${MAX_ULPS}`);
  process.exit(worst > MAX_ULPS ? 1 : 0);
}

/** The C kind of each of core's BiquadKind and EqBandType names. */
const CORE_KIND = {
  peaking: 'OMX_EQ_PEAKING', lowShelf: 'OMX_EQ_LOWSHELF', highShelf: 'OMX_EQ_HIGHSHELF', highpass: 'OMX_EQ_HIGHPASS',
  lowpass: 'OMX_EQ_LOWPASS', notch: 'OMX_EQ_NOTCH', bandpass: 'OMX_EQ_BANDPASS', allpass1: 'OMX_EQ_ALLPASS1',
  allpass2: 'OMX_EQ_ALLPASS2', bell: 'OMX_EQ_PEAKING',
};

/** rbjCoeffs' inputs: every kind across the travel, past Nyquist and at a non-positive Q. */
function designRows() {
  const rows = [];
  const gainKinds = ['peaking', 'lowShelf', 'highShelf'];
  const kinds = ['peaking', 'lowShelf', 'highShelf', 'highpass', 'lowpass', 'notch', 'bandpass', 'allpass1', 'allpass2'];
  for (const kind of kinds) {
    for (const f of [20, 1000, 15000, 30000]) {
      for (const q of [0.3, 4]) {
        for (const g of gainKinds.includes(kind) ? [-12, 6] : [0]) rows.push({ kind, f, q, g });
      }
    }
  }
  rows.push({ kind: 'notch', f: 130, q: 116, g: 0 }, { kind: 'peaking', f: 1000, q: 0, g: 9 },
    { kind: 'lowpass', f: 200, q: 10, g: 0 }, { kind: 'highShelf', f: 19000, q: 1, g: 15 },
    { kind: 'lowShelf', f: 20, q: 0.1, g: -15 });
  return rows;
}

/** EqState corpora: the default, every band type on and off, identity bands, both slopes, the EQ off. */
function eqStates() {
  const band = (type, freqHz, gainDb, q, on = true) => ({ type, freqHz, gainDb, q, on });
  return [
    { name: 'default', eq: eq.defaultEqState() },
    { name: 'console', eq: { on: true, bands: [
      band('lowShelf', 90, 3, 0.7), band('bell', 250, -4, 1.4), band('bell', 1200, 0, 1),
      band('bell', 3500, 5, 2.5, false), band('highShelf', 9000, -2, 0.7), band('notch', 130, 0, 116)],
      hpf: { on: true, freqHz: 80, slope: 24 }, lpf: { on: true, freqHz: 16000, slope: 12 } } },
    { name: 'bypassed', eq: { on: false, bands: [band('bell', 400, 6, 1), band('lowShelf', 100, -3, 0.7)],
      hpf: { on: true, freqHz: 120, slope: 12 }, lpf: { on: false, freqHz: 18000, slope: 24 } } },
    { name: 'phase', eq: { on: true, bands: [
      band('allpass1', 300, 0, 1), band('allpass2', 2500, 0, 0.7), band('bell', 19000, 9, 4), band('lowShelf', 20, -15, 0.3)],
      hpf: { on: false, freqHz: 80, slope: 12 }, lpf: { on: true, freqHz: 20000, slope: 24 } } },
    { name: 'filters', eq: { on: true, bands: [], hpf: { on: true, freqHz: 20, slope: 12 }, lpf: { on: true, freqHz: 20000, slope: 12 } } },
  ];
}

async function design() {
  const geom = await import(pathToFileURL(`${openmixer}/packages/web-ui/app/utils/eqCurveGeom.ts`).href);
  const catalog = await import(pathToFileURL(`${openmixer}/packages/catalog/src/eq.ts`).href);
  const out = [];
  const arr = (name, type, xs, per = 6, dims = '') => {
    out.push(`static const ${type} ${name}[${Math.max(1, xs.length)}]${dims} = {`);
    if (xs.length === 0) out.push('  0,');
    for (let i = 0; i < xs.length; i += per) out.push(`  ${xs.slice(i, i + per).join(', ')},`);
    out.push('};');
  };
  const c5 = (c) => `{${Array.from(c, num).join(', ')}}`;
  const cband = (b) => `{${CORE_KIND[b.type]}, ${num(b.freqHz)}, ${num(b.gainDb)}, ${num(b.q)}, ${b.on ? 1 : 0}}`;
  const cpass = (p) => `{${p.on ? 1 : 0}, ${num(p.freqHz)}, ${p.slope === 24 ? 1 : 0}}`;

  out.push('// SPDX-License-Identifier: GPL-3.0-or-later');
  out.push('// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>');
  out.push(`/* GENERATED by tools/ts-golden.mjs design from the console's TypeScript: what the TypeScript computes`);
  out.push(' * for the EQ on fixed inputs at the nine oracle rates — core\'s rbjCoeffs, complementSection,');
  out.push(' * eqBandIsIdentity, eqCoeffs, passFilterCoeffs and biquadMagnitudeDb, the');
  out.push(' * web surface\'s eqStateCurveDb, compositeCurveDb and magnitudeSumDb');
  out.push(' * and the catalog\'s octavesToQ. Do not edit; regenerate. */');
  out.push('#ifndef OMXDSP_TEST_EQ_DESIGN_TS_H');
  out.push('#define OMXDSP_TEST_EQ_DESIGN_TS_H');
  out.push('');

  // rbjCoeffs; complementSection of each bandpass and bell it designed (the de-esser's remainder).
  const design = [];
  const complement = [];
  for (const sr of ORACLE_FULL_RATES) {
    for (const r of designRows()) {
      const c = eq.rbjCoeffs(r.kind, r.f, r.q, r.g, sr);
      design.push(`{${CORE_KIND[r.kind]}, ${num(r.f)}, ${num(r.q)}, ${num(r.g)}, ${num(sr)}, ${c5(c)}}`);
      if (r.kind !== 'bandpass' && r.kind !== 'peaking') continue;
      const s = eq.rbjSection(r.kind, r.f, r.q, r.g, sr);
      if (s.a0 !== 1) throw new Error('rbjSection no longer folds a0 to 1');
      const k = eq.complementSection(s);
      complement.push(`{${c5([s.b0, s.b1, s.b2, s.a1, s.a2])}, ${c5([k.b0, k.b1, k.b2, k.a1, k.a2])}}`);
    }
  }
  out.push('/* rbjCoeffs(kind, f, q, gain, sr). */');
  out.push('struct ts_design { enum omx_eq_kind kind; double freq, q, gain_db, sr; double c[5]; };');
  arr('TS_DESIGN', 'struct ts_design', design, 1);
  out.push('/* complementSection(s), s a bandpass or bell rbjSection designed above, a0 folded. */');
  out.push('struct ts_complement { double c[5], out[5]; };');
  arr('TS_COMPLEMENT', 'struct ts_complement', complement, 1);
  out.push('');

  // eqBandIsIdentity over every band type and a few gains.
  const ident = [];
  for (const type of ['bell', 'lowShelf', 'highShelf', 'notch', 'allpass1', 'allpass2']) {
    for (const g of [0, 1e-9, -6, 15]) {
      ident.push(`{${CORE_KIND[type]}, ${num(g)}, ${eq.eqBandIsIdentity({ type, freqHz: 1000, gainDb: g, q: 1, on: true }) ? 1 : 0}}`);
    }
  }
  out.push('/* eqBandIsIdentity({type, gainDb}). */');
  out.push('struct ts_identity { enum omx_eq_kind kind; double gain_db; int identity; };');
  arr('TS_IDENTITY', 'struct ts_identity', ident, 1);
  out.push('');

  // octavesToQ, the catalog's width law, across and below its floor.
  const oq = [0, 0.01, 0.05, 0.1, 1 / 3, 0.5, 1, 1.5, 2, 3, 4, 8].map((w) => `{${num(w)}, ${num(catalog.octavesToQ(w))}}`);
  out.push('/* octavesToQ(bw). */');
  out.push('struct ts_octaves_q { double bw_oct, q; };');
  arr('TS_OCTAVES_Q', 'struct ts_octaves_q', oq, 1);
  out.push('');

  // eqCoeffs, passFilterCoeffs, eqStateCurveDb and the seats, per EqState and rate.
  const states = eqStates();
  out.push('/* The EqStates. */');
  out.push('struct ts_state { const char *name; int on; uint32_t nb; struct omx_eq_band b[6]; struct omx_eq_pass hpf, lpf; };');
  out.push(`static const struct ts_state TS_STATES[${states.length}] = {`);
  for (const { name, eq: st } of states) {
    if (st.bands.length > 6) throw new Error('a state of more than six bands');
    out.push(`  {"${name}", ${st.on ? 1 : 0}, ${st.bands.length}u, {${st.bands.map(cband).join(', ')}}, ` +
      `${cpass(eq.passFilter(st, 'hpf'))}, ${cpass(eq.passFilter(st, 'lpf'))}},`);
  }
  out.push('};');
  const runs = [];
  const bankC = [];
  const bankOn = [];
  const curveF = states.map(() => null);
  const curveDb = [];
  const seatF = [];
  const seatDb = [];
  const win = geom.axisWindow();
  for (const sr of ORACLE_FULL_RATES) {
    for (let s = 0; s < states.length; s++) {
      const st = states[s].eq;
      const bank = eq.eqCoeffs(st, sr);
      const pass = eq.passFilterCoeffs(st, sr);
      const bOff = bankC.length;
      for (const b of bank.bands) { bankC.push(c5(b.coeffs)); bankOn.push(b.enabled ? 1 : 0); }
      const pOff = bankC.length;
      for (const b of pass.bands) {
        if (!b.enabled) throw new Error('a parked pass section');
        bankC.push(c5(b.coeffs));
        bankOn.push(1);
      }
      const pts = geom.eqStateCurveDb(st, sr, win);
      const fs = pts.map((p) => num(p.f));
      if (curveF[s] === null) curveF[s] = fs;
      else if (curveF[s].join() !== fs.join()) throw new Error('eqStateCurveDb samples a state at frequencies that move with the rate');
      const cOff = curveDb.length;
      for (const p of pts) curveDb.push(num(p.db));
      const drawn = geom.eqStateBands(st);
      const sOff = seatF.length;
      for (const f of [...drawn.map((b) => b.freq), 1000, 15990, sr * 0.49975]) {
        seatF.push(num(f));
        seatDb.push(num(geom.magnitudeSumDb(drawn, f, sr)));
      }
      runs.push(`{${s}u, ${num(sr)}, ${bOff}u, ${bank.count}u, ${pOff}u, ${pass.count}u, ${cOff}u, ${pts.length}u, ${sOff}u, ${seatF.length - sOff}u}`);
    }
  }
  out.push('/* Per state and rate: eqCoeffs (TS_BANK from bank_off, enabled in TS_BANK_ON), passFilterCoeffs');
  out.push(' * (TS_BANK from pass_off), eqStateCurveDb over axisWindow() (TS_STATE_DB from curve_off, at the');
  out.push(' * state\'s frequencies TS_STATE_F from TS_STATE_F_OFF, the same at every rate) and');
  out.push(' * magnitudeSumDb(eqStateBands(eq), f) at each drawn band, 1 kHz, 15.99 kHz and 0.9995 Nyquist. */');
  out.push('struct ts_run { uint32_t state; double sr; uint32_t bank_off, bank_n, pass_off, pass_n, curve_off, curve_n, seat_off, seat_n; };');
  arr('TS_RUNS', 'struct ts_run', runs, 1);
  arr('TS_BANK', 'double', bankC, 1, '[5]');
  arr('TS_BANK_ON', 'uint8_t', bankOn, 24);
  out.push(`static const double TS_WINDOW_F_MIN = ${num(win.fMin)}, TS_WINDOW_F_MAX = ${num(win.fMax)};`);
  out.push(`static const uint32_t TS_WINDOW_POINTS = ${geom.EQ_CURVE_POINTS}u;`);
  const fOff = [];
  const flat = [];
  for (const f of curveF) { fOff.push(`${flat.length}u`); flat.push(...f); }
  arr('TS_STATE_F_OFF', 'uint32_t', fOff, 12);
  arr('TS_STATE_F', 'double', flat);
  arr('TS_STATE_DB', 'double', curveDb);
  arr('TS_SEAT_F', 'double', seatF);
  arr('TS_SEAT_DB', 'double', seatDb);
  out.push('');

  // biquadMagnitudeDb at, around and past Nyquist; compositeCurveDb over the EQ axis.
  const drawnBands = [
    { type: 'bell', freq: 1000, gainDb: 6, q: 2 }, { type: 'notch', freq: 130, gainDb: 0, q: 116 },
    { type: 'highshelf', freq: 12000, gainDb: 12, q: 0.7 }, { type: 'lowpass', freq: 18000, gainDb: 0, q: 1.306563 },
    { type: 'highpass', freq: 40, gainDb: 0, q: 0.5411961 }, { type: 'allpass2', freq: 900, gainDb: 0, q: 3 },
  ];
  const bandmag = [];
  for (const sr of ORACLE_FULL_RATES) {
    for (const b of drawnBands) {
      for (const f of [-5, 0, 10, 130, 1000, 15990, sr * 0.49975, sr * 0.5, 30000]) {
        bandmag.push(`{{${KIND[b.type]}, ${num(b.freq)}, ${num(b.gainDb)}, ${num(b.q)}, 1}, ${num(sr)}, ${num(f)}, ${num(eq.biquadMagnitudeDb(b, f, sr))}}`);
      }
    }
  }
  out.push('/* biquadMagnitudeDb(band, f, sr). */');
  out.push('struct ts_bandmag { struct omx_eq_band b; double sr, f, db; };');
  arr('TS_BANDMAG', 'struct ts_bandmag', bandmag, 1);
  const filters = { hpfOn: true, hpfHz: 60, hpfSlope: 24, lpfOn: true, lpfHz: 14000 };
  const comp = [];
  let compF = null;
  const compDb = [];
  for (const sr of ORACLE_FULL_RATES) {
    const pts = geom.compositeCurveDb(drawnBands.slice(0, 3), filters, sr);
    const fs = pts.map((p) => num(p.f));
    if (compF === null) compF = fs;
    else if (compF.join() !== fs.join()) throw new Error('compositeCurveDb samples at frequencies that move with the rate');
    comp.push(`{${num(sr)}, ${compDb.length}u}`);
    for (const p of pts) compDb.push(num(p.db));
  }
  out.push(`/* compositeCurveDb(the first three bands above, ${JSON.stringify(filters).replace(/"/g, '')}, sr) over EQ_AXIS. */`);
  out.push(`static const double TS_AXIS_F_MIN = ${num(geom.EQ_AXIS.fMin)}, TS_AXIS_F_MAX = ${num(geom.EQ_AXIS.fMax)};`);
  out.push(`static const struct omx_eq_pass TS_COMPOSITE_HPF = ${cpass({ on: filters.hpfOn, freqHz: filters.hpfHz, slope: filters.hpfSlope })};`);
  out.push(`static const struct omx_eq_pass TS_COMPOSITE_LPF = ${cpass({ on: filters.lpfOn, freqHz: filters.lpfHz, slope: 12 })};`);
  out.push('struct ts_composite { double sr; uint32_t off; };');
  arr('TS_COMPOSITE', 'struct ts_composite', comp, 1);
  arr('TS_COMPOSITE_F', 'double', compF);
  arr('TS_COMPOSITE_DB', 'double', compDb);
  out.push('');
  out.push('#endif /* OMXDSP_TEST_EQ_DESIGN_TS_H */');
  process.stdout.write(`${out.join('\n')}\n`);
}

if (cmd === 'curve') curve();
else if (cmd === 'design') await design();
else if (cmd === 'bandwidth-q' && tableArg) bandwidthQ();
else { console.error(`ts-golden.mjs: unknown command ${cmd}`); process.exit(2); }
