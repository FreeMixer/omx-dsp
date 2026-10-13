// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// ts-golden-hooks.mjs — the module resolution tools/ts-golden.mjs runs openmixer's core under:
// `./x.js` imports read `./x.ts`, and the two workspace packages core imports resolve to the
// openmixer checkout's declarations source and an omx-contract TypeScript render.
import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';

let map = {};

export async function initialize(data) {
  map = {
    '@openmixer/omx-contract': `${data.contractTs}/omx-contract.js`,
    '@freemixer/declarations': `${data.openmixer}/packages/declarations/src/index.ts`,
  };
}

export async function resolve(spec, ctx, next) {
  if (map[spec]) return { url: pathToFileURL(map[spec]).href, shortCircuit: true };
  if (spec.startsWith('.') && spec.endsWith('.js') && ctx.parentURL) {
    const ts = new URL(spec.replace(/\.js$/, '.ts'), ctx.parentURL);
    if (existsSync(fileURLToPath(ts))) return { url: ts.href, shortCircuit: true };
  }
  return next(spec, ctx);
}
