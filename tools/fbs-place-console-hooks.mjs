// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// fbs-place-console-hooks.mjs — the module hooks tools/fbs-place-console.mjs runs the console's placement under:
// the checkout's TypeScript by node's type stripping, `@freemixer/declarations/omx-contract` and the console's
// own `../omx-contract/omx-contract.js` as the given TypeScript render of a contract directory, `@freemixer/core`
// as the core modules the placement reads (no core index: it reaches the engine), the DSP door as the given
// cents module (the console's door calls omx_hrp_cents_between, which the C vector test holds), and a
// generated protocol table the placement never reads as an empty one.
import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';

let map = {};
let core = '';
let cents = '';
const CORE = 'omx-fbs-console:core';
const DSP = 'omx-fbs-console:dsp';
const EMPTY = 'omx-fbs-console:generated';

export async function initialize(data) {
  const pkgs = `${data.openmixer}/packages`;
  const src = `${pkgs}/core/src`;
  const contract = pathToFileURL(`${data.contractTs}/omx-contract.js`).href;
  map = { '@freemixer/declarations/omx-contract': contract, '@openmixer/omx-contract': contract, '@freemixer/declarations': pathToFileURL(`${pkgs}/declarations/src/index.ts`).href };
  const from = (f) => JSON.stringify(pathToFileURL(`${src}/${f}`).href);
  core = [
    `export * from ${from('fbs.ts')};`,
    `export { channelKey } from ${from('model.ts')};`,
    `export { clampEqGain, clampEqFreq, clampEqQFor } from ${from('eq.ts')};`,
    `export { thresholdsFor } from ${from('feedback.ts')};`,
  ].join('\n');
  cents = `import { fbsCents } from ${JSON.stringify(pathToFileURL(data.cents).href)};\nexport const dsp = { dspHrpCentsBetween: (a, b) => fbsCents(a, b) };`;
}

export async function resolve(spec, ctx, next) {
  if (map[spec]) return { url: map[spec], shortCircuit: true };
  if (spec === '@freemixer/core') return { url: CORE, shortCircuit: true };
  if (spec === './native-dsp.js') return { url: DSP, shortCircuit: true };
  if (spec.startsWith('.') && ctx.parentURL && ctx.parentURL.startsWith('file:')) {
    if (/\.generated\.js$/.test(spec) && !existsSync(fileURLToPath(new URL(spec, ctx.parentURL)))) return { url: `${EMPTY}?${spec}`, shortCircuit: true };
    for (const cand of [spec.replace(/\.js$/, '.ts'), `${spec}.ts`]) {
      const ts = new URL(cand, ctx.parentURL);
      if (existsSync(fileURLToPath(ts))) return { url: ts.href, shortCircuit: true };
    }
  }
  return next(spec, ctx);
}

export async function load(url, ctx, next) {
  if (url === CORE) return { format: 'module', source: core, shortCircuit: true };
  if (url === DSP) return { format: 'module', source: cents, shortCircuit: true };
  if (url.startsWith(EMPTY)) return { format: 'module', source: 'export const MOD_HOST_COMMANDS = new Proxy({}, { get: () => ({ verb: "" }) });', shortCircuit: true };
  return next(url, ctx);
}
