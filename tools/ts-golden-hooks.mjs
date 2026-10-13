// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// ts-golden-hooks.mjs — the module resolution tools/ts-golden.mjs runs openmixer's TypeScript under:
// `./x.js` and `./x` imports read `./x.ts`; the workspace packages resolve to the openmixer
// checkout's sources and an omx-contract TypeScript render. `@freemixer/core` resolves to the core
// modules the EQ code imports from it, re-exported by name (its index pulls in the engine, which
// node's type stripping cannot read), and `vue` to the one call the web surface's trace buffer
// makes at load. Neither carries any arithmetic of its own.
import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';

let map = {};
let core = '';

const CORE_FACADE = 'omx-golden:core';
const VUE_STUB = 'omx-golden:vue';

export async function initialize(data) {
  const src = `${data.openmixer}/packages/core/src`;
  map = {
    '@openmixer/omx-contract': pathToFileURL(`${data.contractTs}/omx-contract.js`).href,
    '@freemixer/declarations': pathToFileURL(`${data.openmixer}/packages/declarations/src/index.ts`).href,
    '@freemixer/core/eq': pathToFileURL(`${src}/eq.ts`).href,
    '@freemixer/core': CORE_FACADE,
    vue: VUE_STUB,
  };
  const from = (f) => JSON.stringify(pathToFileURL(`${src}/${f}`).href);
  core = [
    `export { clamp } from ${from('param.ts')};`,
    `export { binToHz } from ${from('feedback.ts')};`,
    `export { hzToBin, aggregateSpectrumBands, reduceSpectrumBands, fftSizeForRate, TIER_CROSSOVER_HZ, RTA_TAP_GATE_DB } from ${from('spectrum-bands.ts')};`,
    `export { dbToLinearGain, linearToDbFloored } from ${from('output-route.ts')};`,
    `export { deEsserBandMagDb, deEsserReductionMagDb } from ${from('deesser-curve.ts')};`,
  ].join('\n');
}

export async function resolve(spec, ctx, next) {
  if (map[spec]) return { url: map[spec], shortCircuit: true };
  if (spec.startsWith('.') && ctx.parentURL && ctx.parentURL.startsWith('file:')) {
    for (const cand of [spec.replace(/\.js$/, '.ts'), `${spec}.ts`]) {
      const ts = new URL(cand, ctx.parentURL);
      if (existsSync(fileURLToPath(ts))) return { url: ts.href, shortCircuit: true };
    }
  }
  return next(spec, ctx);
}

export async function load(url, ctx, next) {
  if (url === CORE_FACADE) return { format: 'module', source: core, shortCircuit: true };
  if (url === VUE_STUB) {
    return { format: 'module', source: 'export function shallowRef(value) { return { value }; }', shortCircuit: true };
  }
  return next(url, ctx);
}
