// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * omx-contract's reader, renderer and CLI, from the checkout OMX_CONTRACT names (make contract-proof sets it): the
 * declaration tests run this library's live data/ through the same code omx-contract installs it with.
 */
import { existsSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export const OC = process.env.OMX_CONTRACT ? resolve(process.env.OMX_CONTRACT) : '';
if (!OC || !existsSync(join(OC, 'lib/data.mjs'))) throw new Error('OMX_CONTRACT names no omx-contract checkout (make contract-proof OMX_CONTRACT=<dir>)');
const oc = (p) => import(pathToFileURL(join(OC, p)).href);

export const { loadData, resolveData, packageVersion, controlGlobal, isKernelFile, isUse } = await oc('lib/data.mjs');
export const { validateData } = await oc('lib/validate.mjs');
export const { renderC } = await oc('render/c.mjs');
export const { renderJson } = await oc('render/json.mjs');
export const { renderTs } = await oc('render/ts.mjs');
export const { main } = await oc('bin/omx-contract.mjs');
export const { limitUnits } = await oc('tools/proof/limit-units.mjs');
export const { classify } = await oc('lib/semver.mjs');
