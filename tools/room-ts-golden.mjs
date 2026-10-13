#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// room-ts-golden.mjs — runs openmixer's TypeScript room analysis and alignment fit
// (packages/core/src/room-transfer.ts, room-impulse.ts, room-rt60.ts, room-modes.ts,
// room-sweep-stimulus.ts, room-mode-eq.ts, smart-align.ts) from a checkout, under node's type
// stripping, on fixed inputs, and prints the parity table test/analysis/fixtures/room_ts.h that
// holds the C kernels (omx_room.h, omx_sweep.h, omx_align_fit.h) to what core computes today
// (openmixer docs/design/specs/2026-10-13-dsp-results-from-c.md §6.2, FreeMixer/openmixer#1602).
// Node 22.6 or later.
//
//   node --experimental-strip-types tools/room-ts-golden.mjs <openmixer> <contract-ts>
//
// <contract-ts> holds omx-contract's TypeScript render (`omx-contract.js`; the release tarball's
// package/dist/ts). Every input is built from exact IEEE operations only (an integer PRNG, sums,
// products and quotients of emitted constants), so test/analysis/room_ts.test.c rebuilds the same
// bits; the transform core takes as a port is omx_fft_radix2_d's butterflies, written below in
// JavaScript, as the server's `fftComplex` runs it.
import { execFileSync } from 'node:child_process';
import { register } from 'node:module';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [openmixerArg, contractTsArg] = process.argv.slice(2);
if (!openmixerArg || !contractTsArg) {
  console.error('usage: room-ts-golden.mjs <openmixer> <contract-ts>');
  process.exit(2);
}
const openmixer = resolve(openmixerArg);
const contractTs = resolve(contractTsArg);
register('./ts-golden-hooks.mjs', import.meta.url, { data: { openmixer, contractTs } });
const core = async (m) => import(pathToFileURL(`${openmixer}/packages/core/src/${m}.ts`).href);
const transfer = await core('room-transfer');
const impulse = await core('room-impulse');
const rt60 = await core('room-rt60');
const modes = await core('room-modes');
const sweep = await core('room-sweep-stimulus');
const modeEq = await core('room-mode-eq');
const align = await core('smart-align');
const route = await core('output-route');
const commit = execFileSync('git', ['-C', openmixer, 'rev-parse', 'HEAD']).toString().trim();

// ---- the transform: omx_fft_radix2_d (include/omxdsp/omx_fft.h), op for op -----------------------
function fft(re, im, inverse) {
  const n = re.length;
  if (im.length !== n || n < 2 || (n & (n - 1)) !== 0) return false;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  const sign = inverse ? -1 : 1;
  for (let len = 2; len <= n; len <<= 1) {
    const half = len >> 1, stride = n / len;
    for (let k = 0; k < half; k++) {
      const ang = (-2 * Math.PI * (k * stride)) / n;
      const cr = Math.cos(ang), ci = sign * Math.sin(ang);
      for (let i = 0; i < n; i += len) {
        const a = i + k, b = a + half;
        const xr = re[b] * cr - im[b] * ci;
        const xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] = re[a] + xr;
        im[a] = im[a] + xi;
      }
    }
  }
  if (inverse) for (let i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
  return true;
}

// ---- exact inputs ---------------------------------------------------------------------------
/** mulberry32: 32-bit integer state, each draw an integer over 2^32 (exact in double). */
function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a) >>> 0;
    t = ((t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t) >>> 0;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
/** Four draws less two: zero-mean, exact (each draw has 32 significant bits at most). */
const noiseOf = (u) => () => u() + u() + u() + u() - 2;

/** A rotator turning one bin's phase by a delay of `d` samples at length `n`. */
const rot = (d, n) => [Math.cos((-2 * Math.PI * d) / n), Math.sin((-2 * Math.PI * d) / n)];

/** H(k) = g·r1^k + a·r2^k + Σ A_j / (1 + i(k − k0_j)/w_j), by recurrence and quotients only. */
function transferOf(fs) {
  const bins = fs.fft >> 1;
  const hr = new Float64Array(bins), hi = new Float64Array(bins);
  let z1r = 1, z1i = 0, z2r = 1, z2i = 0;
  for (let k = 0; k < bins; k++) {
    let r = fs.g * z1r + fs.a * z2r;
    let i = fs.g * z1i + fs.a * z2i;
    for (const m of fs.modes) {
      const x = (k - m.k0) / m.w;
      const den = 1 + x * x;
      r += m.amp / den;
      i += (-m.amp * x) / den;
    }
    hr[k] = r;
    hi[k] = i;
    const n1r = z1r * fs.r1[0] - z1i * fs.r1[1], n1i = z1r * fs.r1[1] + z1i * fs.r1[0];
    const n2r = z2r * fs.r2[0] - z2i * fs.r2[1], n2i = z2r * fs.r2[1] + z2i * fs.r2[0];
    z1r = n1r; z1i = n1i; z2r = n2r; z2i = n2i;
  }
  return { hr, hi };
}

/** The frames of a frame set: a tilted noise reference, the target through H plus noise. */
function framesOf(fs) {
  const bins = fs.fft >> 1;
  const { hr, hi } = transferOf(fs);
  const noise = noiseOf(rng(fs.seed));
  const frames = [];
  for (let f = 0; f < fs.frames; f++) {
    const t = { re: new Float64Array(bins), im: new Float64Array(bins) };
    const r = { re: new Float64Array(bins), im: new Float64Array(bins) };
    for (let k = 0; k < bins; k++) {
      const sh = 1 / (1 + fs.tilt * k);
      const rr = noise() * sh, ri = noise() * sh;
      const n1 = noise(), n2 = noise();
      r.re[k] = rr;
      r.im[k] = ri;
      t.re[k] = hr[k] * rr - hi[k] * ri + fs.nz * n1;
      t.im[k] = hr[k] * ri + hi[k] * rr + fs.nz * n2;
    }
    const last = f === fs.frames - 1 && fs.mismatch;
    frames.push({ target: t, reference: r, sampleRate: last ? fs.sr * 2 : fs.sr, fftSize: fs.fft, referenceBackSamples: fs.back });
  }
  return frames;
}

const FS = [];
const fs = (name, o) => {
  const d = { name, seed: 1, sr: 48000, fft: 1024, frames: 6, tilt: 0.0625, nz: 0.05, g: 1, r1: [1, 0], a: 0, r2: [1, 0],
    modes: [], back: 0, mismatch: 0, ...o };
  FS.push(d);
  return FS.length - 1;
};
const F_DELAY = fs('delay +7.3', { seed: 11, r1: rot(7.3, 1024) });
const F_INVERT = fs('inverted, delay -12.6', { seed: 12, g: -1, nz: 0.1, r1: rot(-12.6, 1024) });
const F_FAR = fs('delay 700 of 1024', { seed: 13, nz: 0.02, r1: rot(700.4, 1024) });
const F_BACK = fs('delay 40, reference read 30 back', { seed: 14, r1: rot(10.25, 1024), back: 30 });
const F_NOISE = fs('uncorrelated', { seed: 15, g: 0, nz: 1, frames: 8 });
const F_THREE = fs('three frames', { seed: 16, frames: 3, r1: rot(3, 1024) });
const F_MISMATCH = fs('a frame at another rate', { seed: 17, frames: 5, mismatch: 1, r1: rot(3, 1024) });
const F_REFL = fs('a reflection 150 samples after the direct sound', { seed: 18, fft: 2048, r1: rot(20, 2048), a: 0.45,
  r2: rot(170, 2048), nz: 0.02 });
const F_FLAT = fs('a flat room under a tilted reference', { seed: 19, tilt: 0.25, nz: 0, frames: 4 });
const MODE_SET = [{ k0: 9.3, w: 0.8, amp: 4 }, { k0: 15.7, w: 1.1, amp: 3 }, { k0: 26.2, w: 1.5, amp: 2.5 }];
const F_POS = [37, 53, 71].map((d, p) => fs(`mode position ${p + 1}`, { seed: 21 + p, fft: 8192, frames: 4, tilt: 0,
  nz: 0.01, a: 0.6, r2: rot(d, 8192), modes: MODE_SET }));

const D = {}; // every flat double array the table carries, by name
const push = (name, values) => {
  if (!D[name]) D[name] = [];
  const off = D[name].length;
  for (const v of values) D[name].push(v);
  return off;
};

const BAND = [align.DEFAULT_BAND.lowHz, align.DEFAULT_BAND.highHz];
const MODE_BAND = [modes.ROOM_MODE_BAND.lowHz, modes.ROOM_MODE_BAND.highHz];
const REFUSAL = { 'too-few-frames': 1, 'shape-mismatch': 2, 'empty-band': 3, 'no-shared-energy': 4,
  'residual-exceeds-half-sample': 5, 'too-few-positions': 6, 'too-short': 7, 'no-signal': 8, 'insufficient-decay': 9 };
const code = (r) => (r.kind === 'refused' ? REFUSAL[r.code] : 0);
const framesCache = FS.map(framesOf);

// ---- the alignment fit and its coarse delay ----------------------------------------------------
const FITS = [];
for (const [f, arrival] of [[F_DELAY, 'either'], [F_INVERT, 'either'], [F_FAR, 'either'], [F_FAR, 'physical'],
  [F_BACK, 'physical'], [F_NOISE, 'either'], [F_THREE, 'either'], [F_MISMATCH, 'either'], [F_REFL, 'physical']]) {
  const r = align.fitAlignment(fft, framesCache[f], align.DEFAULT_BAND, arrival);
  FITS.push({ f, arrival, code: code(r), frames: r.frames, delay: r.delayMs ?? 0, invert: r.polarity === 'invert' ? 1 : 0,
    coh: r.coherence ?? 0, resid: r.residualDeg ?? 0 });
}
const COARSE = [];
for (const [f, klo, khi, arrival] of [[F_DELAY, 2, 85, 'either'], [F_INVERT, 2, 85, 'either'], [F_FAR, 2, 85, 'either'],
  [F_FAR, 2, 85, 'physical'], [F_REFL, 3, 170, 'physical'], [F_DELAY, -4, 900, 'either'], [F_DELAY, 30, 20, 'either']]) {
  const { hr, hi } = transferOf(FS[f]);
  const r = align.coarseDelayOf(fft, { re: hr, im: hi }, { fftSize: FS[f].fft, kLo: klo, kHi: khi, arrival });
  COARSE.push({ f, klo, khi, arrival, found: r ? 1 : 0, samples: r?.samples ?? 0, lag: r?.lag ?? 0, peak: r?.peak ?? 0 });
}
const CORRECTIONS = [];
for (const [d, inv, ad, ai, lo, hi] of [[2.5, 0, 5, 0, 0, 50], [7.25, 1, 5, 0, 0, 50], [-3, 1, 1, 1, 0, 2], [0.125, 0, 0, 1, 0, 50]]) {
  const c = align.alignCorrection({ kind: 'fit', delayMs: d, polarity: inv ? 'invert' : 'normal', coherence: 1, residualDeg: 0,
    frames: 4 }, { delayMs: ad, inverted: !!ai }, { min: lo, max: hi });
  CORRECTIONS.push({ d, inv, ad, ai, lo, hi, out: c.delayMs, inverted: c.inverted ? 1 : 0, clamped: c.clamped ? 1 : 0 });
}

// ---- the transfer curve and the impulse --------------------------------------------------------
const curveRow = (c) => {
  const off = push('curve', []);
  for (const b of c.bins) { D.curve.push(b.hz, b.magnitudeDb, b.phaseDeg, b.coherence); }
  return { off: off / 4, n: c.bins.length };
};
const TRANSFERS = [];
for (const [f, band] of [[F_DELAY, BAND], [F_INVERT, BAND], [F_REFL, BAND], [F_FLAT, BAND], [F_NOISE, BAND],
  [F_THREE, BAND], [F_MISMATCH, BAND], [F_BACK, BAND], [F_DELAY, [3000, 3010]], [F_POS[0], MODE_BAND]]) {
  const acc = transfer.roomTransferAccumulate(framesCache[f], { lowHz: band[0], highHz: band[1] });
  const row = { f, band, code: code(acc), frames: acc.frames, klo: acc.kLo ?? 0, khi: acc.kHi ?? 0, off: 0, n: 0 };
  if (!('kind' in acc)) Object.assign(row, curveRow(transfer.roomTransferCurveFromAccum(acc)));
  TRANSFERS.push(row);
}
const reflRows = (r) => {
  const off = push('refl', []);
  for (const x of r.reflections) D.refl.push(x.arrivalMs, x.distanceM, x.levelDb);
  return { roff: off / 3, rn: r.reflections.length };
};
const IMPULSES = [];
for (const [f, band] of [[F_REFL, BAND], [F_DELAY, BAND], [F_FLAT, BAND], [F_THREE, BAND]]) {
  const r = impulse.roomImpulseOf(fft, framesCache[f], { lowHz: band[0], highHz: band[1] });
  const row = { f, band, code: code(r), direct: r.directArrivalSamples ?? 0, roff: 0, rn: 0 };
  if (r.kind === 'impulse') Object.assign(row, reflRows(r));
  IMPULSES.push(row);
}
/** Impulse responses given sample by sample: sparse arrivals at exact levels. */
const IRS = [
  { name: 'three arrivals', sr: 48000, n: 700, taps: [[30, 1], [120, 0.6], [290, -0.35]], band: BAND },
  { name: 'two arrivals at 96 kHz', sr: 96000, n: 2000, taps: [[50, 1], [430, 0.3]], band: BAND },
  { name: 'silence', sr: 48000, n: 300, taps: [], band: BAND },
  { name: 'a band above Nyquist', sr: 48000, n: 64, taps: [[3, 1]], band: [30000, 40000] },
];
const IR_ROWS = [];
for (const s of IRS) {
  const ir = new Float64Array(s.n);
  for (const [i, v] of s.taps) ir[i] = v;
  const acc = transfer.accumFromImpulseResponse(fft, ir, s.sr, { lowHz: s.band[0], highHz: s.band[1] });
  const row = { s, code: code(acc), frames: acc.frames, fftSize: acc.fftSize ?? 0, klo: acc.kLo ?? 0, khi: acc.kHi ?? 0,
    off: 0, n: 0, icode: 0, direct: 0, roff: 0, rn: 0 };
  if (!('kind' in acc)) {
    Object.assign(row, curveRow(transfer.roomTransferCurveFromAccum(acc)));
    const r = impulse.roomImpulseFromAccum(fft, acc);
    row.icode = code(r);
    if (r.kind === 'impulse') { row.direct = r.directArrivalSamples; Object.assign(row, reflRows(r)); }
  }
  IR_ROWS.push(row);
}

// ---- room modes, proposals, residuals ----------------------------------------------------------
const peakRows = [];
const perPosition = [];
for (const f of [...F_POS, F_THREE]) {
  const r = modes.roomModePeaksOf(framesCache[f], modes.ROOM_MODE_BAND);
  const off = push('peaks', []);
  if (r.kind === 'peaks') for (const p of r.peaks) D.peaks.push(p.hz, p.q, p.aboveMedianDb);
  peakRows.push({ f, code: code(r), binHz: r.binHz ?? 0, off: off / 3, n: r.kind === 'peaks' ? r.peaks.length : 0 });
  perPosition.push(framesCache[f]);
}
const MODE_ROWS = [];
for (const set of [[0, 1, 2], [0, 1], [0], [0, 1, 3], [2, 0, 1]]) {
  const r = modes.roomModesOf(set.map((i) => perPosition[i]), modes.ROOM_MODE_BAND);
  const off = push('modes', []);
  if (r.kind === 'modes') for (const m of r.modes) D.modes.push(m.hz, m.positions, m.q, m.aboveMedianDb);
  MODE_ROWS.push({ set, code: code(r), off: off / 4, n: r.kind === 'modes' ? r.modes.length : 0, list: r.modes ?? [] });
}
/** Mode lists fed to the proposal straight: the confirmed set above, then one that hits every bound. */
const PROPOSAL_INPUTS = [
  MODE_ROWS[0].list,
  [{ hz: 50, positions: 3, q: 0.1, aboveMedianDb: 30 }, { hz: 80, positions: 1, q: 4, aboveMedianDb: 12 },
    { hz: 120, positions: 2, q: 20, aboveMedianDb: 9 }, { hz: 150, positions: 2, q: 5, aboveMedianDb: 0 },
    { hz: 200, positions: 4, q: 3, aboveMedianDb: 9 }, { hz: 240, positions: 2, q: 0.3, aboveMedianDb: 6.5 }],
];
const PROPOSALS = [];
for (const list of PROPOSAL_INPUTS) {
  const ioff = push('pin', []);
  for (const m of list) D.pin.push(m.hz, m.positions, m.q, m.aboveMedianDb);
  const out = modeEq.roomModeEqProposalsOf(list);
  const off = push('prop', []);
  for (const p of out) D.prop.push(p.hz, p.gainDb, p.q, p.positions, p.bounded ? 1 : 0, p.requestedDb);
  PROPOSALS.push({ ioff: ioff / 4, in: list.length, off: off / 6, n: out.length });
}
const posCurve = transfer.roomTransferOf(framesCache[F_POS[1]], modes.ROOM_MODE_BAND);
const RESIDUALS = [];
const resCurve = curveRow(posCurve);
for (const hz of [...MODE_ROWS[0].list.map((m) => m.hz), 20, 1000, 140.3]) {
  const r = modeEq.roomModeResidualDb(posCurve.bins, hz);
  RESIDUALS.push({ hz, defined: r === undefined ? 0 : 1, value: r ?? 0 });
}
const emptyResidual = modeEq.roomModeResidualDb([], 100) === undefined ? 0 : 1;
const MEDIANS = [[3], [5, 1], [2, 9, -4, 7], [1.5, -2.25, 8, 8, 0, 3.75, -1]].map((v) => ({ v, m: modes.median(v) }));
const ML = (hz) => hz.map((h) => ({ hz: h, positions: 2, q: 4, aboveMedianDb: 7 }));
const STABLE = [[[50, 80], [51, 79.5], 1.5], [[50, 80], [50], 1.5], [[50, 80], [50, 83], 1.5], [[50, 51], [50.5, 50.6], 1],
  [[50, 51], [50.2, 52.5], 1], [[], [], 1]].map(([b, a, tol]) => ({ b, a, tol, r: modes.roomModeSetIsStable(ML(b), ML(a), tol) ? 1 : 0 }));

// ---- RT60 --------------------------------------------------------------------------------------
const DECAYS = [
  { name: 'T60 350 ms', seed: 31, sr: 48000, n: 48000, t60: 0.35, nf: 0 },
  { name: 'T60 800 ms over a floor', seed: 32, sr: 48000, n: 72000, t60: 0.8, nf: 1e-4 },
  { name: 'T60 500 ms at 96 kHz', seed: 33, sr: 96000, n: 96000, t60: 0.5, nf: 0 },
  { name: 'stationary noise', seed: 34, sr: 48000, n: 48000, t60: 0, nf: 0 },
  { name: 'too short', seed: 35, sr: 48000, n: 100, t60: 0.2, nf: 0 },
  { name: 'empty', seed: 36, sr: 48000, n: 0, t60: 0.2, nf: 0 },
];
const RT60_ROWS = [];
for (const d of DECAYS) {
  d.r = d.t60 > 0 ? Math.pow(10, -3 / (d.t60 * d.sr)) : 1;
  const noise = noiseOf(rng(d.seed));
  const x = new Float64Array(d.n);
  let env = 1;
  for (let i = 0; i < d.n; i++) {
    const a = noise(), b = noise();
    x[i] = a * env + d.nf * b;
    env *= d.r;
  }
  const r = rt60.roomRt60Of(x, d.sr);
  const off = push('rt60', []);
  for (const b of r.bands) D.rt60.push(b.hz, code(b), b.t60Ms ?? 0);
  RT60_ROWS.push({ d, off: off / 3, n: r.bands.length });
}

// ---- the Farina sweep --------------------------------------------------------------------------
const SWEEPS = [
  { sr: 48000, f0: 40, f1: 16000, dur: 0.25, fadeIn: undefined, fadeOut: undefined, stride: 97,
    taps: [[0, 1], [37, 0.5], [203, -0.25]], length: 400, pre: 25 },
  { sr: 96000, f0: 20, f1: 20000, dur: 0.5, fadeIn: 0.01, fadeOut: 0.03, stride: 331, taps: [[5, 0.8], [900, 0.2]],
    length: 1200, pre: 0 },
];
const SWEEP_ROWS = [];
for (const s of SWEEPS) {
  const sw = sweep.generateSweep({ sampleRate: s.sr, f0: s.f0, f1: s.f1, durationS: s.dur, fadeInS: s.fadeIn, fadeOutS: s.fadeOut });
  const fadeIn = s.fadeIn ?? sweep.DEFAULT_FADE_FRACTION * sw.durationS;
  const fadeOut = s.fadeOut ?? sweep.DEFAULT_FADE_FRACTION * sw.durationS;
  const soff = push('sweep', []);
  for (let i = 0; i < sw.lengthSamples; i += s.stride) D.sweep.push(sw.stimulus[i], sw.inverse[i]);
  const maxTap = Math.max(...s.taps.map((t) => t[0]));
  const resp = new Float64Array(sw.lengthSamples + maxTap);
  for (const [d, g] of s.taps) for (let i = 0; i < sw.lengthSamples; i++) resp[i + d] += sw.stimulus[i] * g;
  const ir = sweep.recoverImpulseResponse(fft, resp, sw, { length: s.length, preRollSamples: s.pre });
  const ioff = push('swir', ir);
  const harm = [1, 2, 3, 4.5, 7].map((k) => sweep.harmonicDelaySamples(sw, k));
  const hoff = push('harm', harm);
  SWEEP_ROWS.push({ s, fadeIn, fadeOut, n: sw.lengthSamples, L: sw.L, dur: sw.durationS, soff: soff / 2,
    sn: Math.ceil(sw.lengthSamples / s.stride), respLen: resp.length, ioff, hoff, lag: sweep.linearImpulseLag(sw) });
}
const CONVOLVES = [[[1, 2, 3, 0.5], [1, -1, 0.25]], [[2], [3]], [[0.5, -0.125, 4, 1, 1, 2, -3], [0.25, 0.5]]].map(([a, b]) => ({
  a, b, out: Array.from(sweep.convolve(fft, a, b)) }));

// ---- the table ---------------------------------------------------------------------------------
const num = (x) => {
  if (Number.isNaN(x)) return 'NAN';
  if (x === Infinity) return 'INFINITY';
  if (x === -Infinity) return '-INFINITY';
  const s = String(x); // the shortest string that reads back as x: the C compiler rounds it to x
  return /[.e]/.test(s) ? s : `${s}.0`;
};
const out = [];
const line = (s) => out.push(s);
const arr = (name, values, per = 6) => {
  const v = values.length ? values : [0];
  line(`static const double ${name}[${v.length}] = {`);
  for (let i = 0; i < v.length; i += per) line(`  ${v.slice(i, i + per).map(num).join(', ')},`);
  line('};');
};
const rows = (type, name, list, fmt) => {
  line(`static const struct ${type} ${name}[${list.length}] = {`);
  for (const r of list) line(`  {${fmt(r)}},`);
  line('};');
};
line('// SPDX-License-Identifier: GPL-3.0-or-later');
line('// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>');
line(`/* GENERATED by tools/room-ts-golden.mjs from openmixer ${commit}: core's room analysis and`);
line(' * alignment fit (packages/core/src/room-*.ts, smart-align.ts) on the fixed inputs that tool builds,');
line(' * which test/analysis/room_ts.test.c rebuilds bit for bit. Refusal codes are enum');
line(' * omx_analysis_refusal\'s. Do not edit; regenerate. */');
line('#ifndef OMXDSP_TEST_ROOM_TS_H');
line('#define OMXDSP_TEST_ROOM_TS_H');
line('');
line('#include <math.h>');
line('#include <stdint.h>');
line('');
line('/* core\'s declarations the kernels take from their caller, as core declares them. */');
line(`static const uint32_t TS_MIN_FRAMES = ${align.MIN_FRAMES}u;`);
line(`static const double TS_DEFAULT_BAND[2] = {${BAND.map(num).join(', ')}};`);
line(`static const double TS_ALIGN_TRUST_COHERENCE = ${num(align.ALIGN_TRUST_COHERENCE)};`);
line(`static const double TS_ANALYSIS_FLOOR_DB = ${num(route.ANALYSIS_FLOOR_DB)};`);
line(`static const double TS_EARLY_REFLECTION_WINDOW_MS = ${num(impulse.EARLY_REFLECTION_WINDOW_MS)};`);
line(`static const double TS_EARLY_REFLECTION_FLOOR_DB = ${num(impulse.EARLY_REFLECTION_FLOOR_DB)};`);
line(`static const double TS_T20[2] = {${num(rt60.T20_UPPER_DB)}, ${num(rt60.T20_LOWER_DB)}};`);
line(`static const double TS_OCTAVE_BAND_CENTRES_HZ[${rt60.OCTAVE_BAND_CENTRES_HZ.length}] = {${rt60.OCTAVE_BAND_CENTRES_HZ.map(num).join(', ')}};`);
line(`static const double TS_ROOM_MODE_BAND[2] = {${MODE_BAND.map(num).join(', ')}};`);
line(`static const double TS_ROOM_MODE_MIN_ABOVE_MEDIAN_DB = ${num(modes.ROOM_MODE_MIN_ABOVE_MEDIAN_DB)};`);
line(`static const uint32_t TS_ROOM_MODE_MIN_POSITIONS = ${modes.ROOM_MODE_MIN_POSITIONS}u;`);
line(`static const double TS_ROOM_MODE_MAX_CUT_DB = ${num(modeEq.ROOM_MODE_MAX_CUT_DB)};`);
line(`static const double TS_DEFAULT_FADE_FRACTION = ${num(sweep.DEFAULT_FADE_FRACTION)};`);
line('');
line('/* A frame set: H(k) = g·r1^k + a·r2^k + Σ amp/(1 + i(k − k0)/w); frames of tilted noise. */');
line('struct ts_mode_term { double k0, w, amp; };');
line('struct ts_frame_set { const char *name; uint32_t seed; double sr; uint32_t fft, frames; double tilt, nz, g, r1[2], a, r2[2];');
line('                      uint32_t nmodes; struct ts_mode_term modes[3]; double back; uint32_t mismatch; };');
rows('ts_frame_set', 'TS_FRAME_SETS', FS, (f) => `"${f.name}", ${f.seed}u, ${num(f.sr)}, ${f.fft}u, ${f.frames}u, ${num(f.tilt)}, ` +
  `${num(f.nz)}, ${num(f.g)}, {${f.r1.map(num).join(', ')}}, ${num(f.a)}, {${f.r2.map(num).join(', ')}}, ${f.modes.length}u, ` +
  `{${f.modes.length ? f.modes.map((m) => `{${num(m.k0)}, ${num(m.w)}, ${num(m.amp)}}`).join(', ') : '{0.0, 0.0, 0.0}'}}, ` +
  `${num(f.back)}, ${f.mismatch}u`);
line('');
line('struct ts_fit { uint32_t fs, physical, code, frames; double delay_ms; uint32_t invert; double coherence, residual_deg; };');
rows('ts_fit', 'TS_FITS', FITS, (r) => `${r.f}u, ${r.arrival === 'physical' ? 1 : 0}u, ${r.code}u, ${r.frames}u, ${num(r.delay)}, ` +
  `${r.invert}u, ${num(r.coh)}, ${num(r.resid)}`);
line('struct ts_coarse { uint32_t fs; int32_t k_lo, k_hi; uint32_t physical, found; double samples, lag, peak; };');
rows('ts_coarse', 'TS_COARSE', COARSE, (r) => `${r.f}u, ${r.klo}, ${r.khi}, ${r.arrival === 'physical' ? 1 : 0}u, ${r.found}u, ` +
  `${num(r.samples)}, ${num(r.lag)}, ${num(r.peak)}`);
line('struct ts_correction { double fit_ms; uint32_t fit_invert; double applied_ms; uint32_t applied_inverted;');
line('                       double lo, hi, delay_ms; uint32_t inverted, clamped; };');
rows('ts_correction', 'TS_CORRECTIONS', CORRECTIONS, (r) => `${num(r.d)}, ${r.inv}u, ${num(r.ad)}, ${r.ai}u, ${num(r.lo)}, ` +
  `${num(r.hi)}, ${num(r.out)}, ${r.inverted}u, ${r.clamped}u`);
line('');
line('/* Curves: four doubles per bin, {hz, magnitude dB, unwrapped phase degrees, coherence}. */');
arr('TS_CURVE', D.curve ?? [], 4);
line('struct ts_transfer { uint32_t fs; double band[2]; uint32_t code, frames, k_lo, k_hi, off, n; };');
rows('ts_transfer', 'TS_TRANSFERS', TRANSFERS, (r) => `${r.f}u, {${r.band.map(num).join(', ')}}, ${r.code}u, ${r.frames}u, ` +
  `${r.klo}u, ${r.khi}u, ${r.off}u, ${r.n}u`);
line('/* Reflections: three doubles each, {arrival ms, distance m, level dB}. */');
arr('TS_REFL', D.refl ?? [], 3);
line('struct ts_impulse { uint32_t fs; double band[2]; uint32_t code, direct, off, n; };');
rows('ts_impulse', 'TS_IMPULSES', IMPULSES, (r) => `${r.fs ?? r.f}u, {${r.band.map(num).join(', ')}}, ${r.code}u, ${r.direct}u, ` +
  `${r.roff}u, ${r.rn}u`);
line('struct ts_ir { const char *name; double sr; uint32_t n, ntaps; uint32_t at[3]; double level[3]; double band[2];');
line('               uint32_t code, frames, fft_size, k_lo, k_hi, off, n_bins, impulse_code, direct, roff, rn; };');
rows('ts_ir', 'TS_IRS', IR_ROWS, (r) => `"${r.s.name}", ${num(r.s.sr)}, ${r.s.n}u, ${r.s.taps.length}u, ` +
  `{${[0, 1, 2].map((i) => `${r.s.taps[i]?.[0] ?? 0}u`).join(', ')}}, {${[0, 1, 2].map((i) => num(r.s.taps[i]?.[1] ?? 0)).join(', ')}}, ` +
  `{${r.s.band.map(num).join(', ')}}, ${r.code}u, ${r.frames}u, ${r.fftSize}u, ${r.klo}u, ${r.khi}u, ${r.off}u, ${r.n}u, ` +
  `${r.icode}u, ${r.direct}u, ${r.roff}u, ${r.rn}u`);
line('');
line('/* Peaks: three doubles each, {hz, q, dB above the median}; modes: four, {hz, positions, q, dB above}. */');
arr('TS_PEAKS', D.peaks ?? [], 3);
line('struct ts_peaks { uint32_t fs, code; double bin_hz; uint32_t off, n; };');
rows('ts_peaks', 'TS_PEAK_ROWS', peakRows, (r) => `${r.f}u, ${r.code}u, ${num(r.binHz)}, ${r.off}u, ${r.n}u`);
arr('TS_MODES', D.modes ?? [], 4);
line('struct ts_modes { uint32_t np; uint32_t pos[3]; uint32_t code, off, n; };');
rows('ts_modes', 'TS_MODE_ROWS', MODE_ROWS, (r) => `${r.set.length}u, {${[0, 1, 2].map((i) => `${r.set[i] ?? 0}u`).join(', ')}}, ` +
  `${r.code}u, ${r.off}u, ${r.n}u`);
line('/* Proposal inputs: four doubles per mode; proposals: six, {hz, gain dB, q, positions, bounded, requested dB}. */');
arr('TS_PROPOSAL_IN', D.pin ?? [], 4);
arr('TS_PROPOSAL_OUT', D.prop ?? [], 6);
line('struct ts_proposals { uint32_t in_off, in_n, off, n; };');
rows('ts_proposals', 'TS_PROPOSALS', PROPOSALS, (r) => `${r.ioff}u, ${r.in}u, ${r.off}u, ${r.n}u`);
line(`/* Residuals over the curve of frame set ${F_POS[1]} in the mode band, TS_CURVE from ${resCurve.off}, ${resCurve.n} bins. */`);
line(`static const uint32_t TS_RESIDUAL_FS = ${F_POS[1]}u, TS_RESIDUAL_CURVE_OFF = ${resCurve.off}u, TS_RESIDUAL_CURVE_N = ${resCurve.n}u;`);
line(`static const int TS_RESIDUAL_EMPTY_DEFINED = ${emptyResidual};`);
line('struct ts_residual { double hz; int defined; double value; };');
rows('ts_residual', 'TS_RESIDUALS', RESIDUALS, (r) => `${num(r.hz)}, ${r.defined}, ${num(r.value)}`);
line('struct ts_median { uint32_t n; double v[8]; double median; };');
rows('ts_median', 'TS_MEDIANS', MEDIANS, (r) => `${r.v.length}u, {${r.v.map(num).join(', ')}}, ${num(r.m)}`);
line('struct ts_stable { uint32_t nb, na; double before[2], after[2], tol; int stable; };');
rows('ts_stable', 'TS_STABLE', STABLE, (r) => `${r.b.length}u, ${r.a.length}u, {${[0, 1].map((i) => num(r.b[i] ?? 0)).join(', ')}}, ` +
  `{${[0, 1].map((i) => num(r.a[i] ?? 0)).join(', ')}}, ${num(r.tol)}, ${r.r}`);
line('');
line('/* RT60: a decay x[i] = noise·env + nf·noise, env *= r; bands: three doubles each, {hz, code, t60 ms}. */');
arr('TS_RT60', D.rt60 ?? [], 3);
line('struct ts_decay { const char *name; uint32_t seed; double sr; uint32_t n; double r, nf; uint32_t off, nbands; };');
rows('ts_decay', 'TS_DECAYS', RT60_ROWS, (x) => `"${x.d.name}", ${x.d.seed}u, ${num(x.d.sr)}, ${x.d.n}u, ${num(x.d.r)}, ` +
  `${num(x.d.nf)}, ${x.off}u, ${x.n}u`);
line('');
line('/* Sweeps: {stimulus, inverse} pairs every `stride` samples; the recovered IR in full; harmonic delays. */');
arr('TS_SWEEP', D.sweep, 2);
arr('TS_SWEEP_IR', D.swir);
arr('TS_SWEEP_HARMONIC', D.harm, 5);
line('static const double TS_HARMONIC_ORDERS[5] = {1.0, 2.0, 3.0, 4.5, 7.0};');
line('struct ts_sweep { double sr, f0, f1, dur, fade_in_s, fade_out_s; uint32_t n; double L, realised; uint32_t stride, off, ns;');
line('                  uint32_t ntaps, at[3]; double level[3]; uint32_t resp_len, length, pre, ir_off, harm_off, lag; };');
rows('ts_sweep', 'TS_SWEEPS', SWEEP_ROWS, (r) => `${num(r.s.sr)}, ${num(r.s.f0)}, ${num(r.s.f1)}, ${num(r.s.dur)}, ${num(r.fadeIn)}, ` +
  `${num(r.fadeOut)}, ${r.n}u, ${num(r.L)}, ${num(r.dur)}, ${r.s.stride}u, ${r.soff}u, ${r.sn}u, ${r.s.taps.length}u, ` +
  `{${[0, 1, 2].map((i) => `${r.s.taps[i]?.[0] ?? 0}u`).join(', ')}}, {${[0, 1, 2].map((i) => num(r.s.taps[i]?.[1] ?? 0)).join(', ')}}, ` +
  `${r.respLen}u, ${r.s.length}u, ${r.s.pre}u, ${r.ioff}u, ${r.hoff}u, ${r.lag}u`);
line('struct ts_convolve { uint32_t na, nb; double a[8], b[4]; uint32_t n; double out[12]; };');
rows('ts_convolve', 'TS_CONVOLVES', CONVOLVES, (r) => `${r.a.length}u, ${r.b.length}u, {${r.a.map(num).join(', ')}}, ` +
  `{${r.b.map(num).join(', ')}}, ${r.out.length}u, {${r.out.map(num).join(', ')}}`);
line('');
line('#endif /* OMXDSP_TEST_ROOM_TS_H */');
process.stdout.write(`${out.join('\n')}\n`);
