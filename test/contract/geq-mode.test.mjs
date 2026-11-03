// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * #71: the graphic EQ's design mode is a control, GEQ_MODES (`standard`, `true`, coming up `standard`), appended after
 * the band faders, and whether the `true` solve clamped is a readout, designClamped. The ids are in the order of the
 * integer omx-dsp's design step reads (omx-dsp 0f23bed, include/omxdsp/fx/omx_geq_instance.h:71, quoted below), so
 * a reordered or renamed id turns this red before a plugin hands the kernel the wrong mode.
 */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, scratchData } from './helpers.mjs';

// omx-dsp's enum, verbatim: the consumer the contract's ids must agree with, index for index.
const OMX_DSP_GEQ_MODE = `enum omx_geq_mode {
  OMX_GEQ_MODE_STANDARD = 0, /**< Each section's gain is its fader: the proportional build-up. */
  OMX_GEQ_MODE_TRUE = 1      /**< The section gains solve the band interaction. */
};`;
const dspModes = () => [...OMX_DSP_GEQ_MODE.matchAll(/OMX_GEQ_MODE_([A-Z]+) = (\d+)/g)].map((m) => [m[1].toLowerCase(), Number(m[2])]);

const json = (dir = UNION) => JSON.parse(renderJson(resolveData(loadData(dir)), '0.0.0')[0].text);
const header = (dir = UNION) => renderC(resolveData(loadData(dir)))[0].text;
const rendered = (dir) => [...header(dir).matchAll(/^ {2}OMX_GEQ_MODES_([A-Z]+) = (\d+),$/gm)].map((m) => [m[1].toLowerCase(), Number(m[2])]);

test('geq: mode is a choice appended after the band faders, standard | true, coming up standard', () => {
  assert.deepEqual(json().kernels.geq.controls.map((c) => `${c.name}:${c.kind}:${c.global}`),
    ['band:travel:GEQ_BAND_RANGE', 'mode:choice:GEQ_MODES']);
  assert.deepEqual(json().items.GEQ_MODES.value, ['standard', 'true']);
  assert.equal(json().items.GEQ_MODES.default, 'standard');
  assert.match(header(), /^#define OMX_GEQ_MODES_DEFAULT OMX_GEQ_MODES_STANDARD$/m);
});

test('geq: the C enum gives each mode the integer omx-dsp\'s enum omx_geq_mode gives it', () => {
  assert.equal(dspModes().length, 2, 'the probe reads omx-dsp\'s two modes');
  assert.deepEqual(rendered(), dspModes());
});

test('geq: designClamped is a 0..1 stepped readout at READOUT_RATE_HZ', () => {
  assert.deepEqual(json().kernels.geq.readouts.map(({ doc, ...r }) => r),
    [{ name: 'designClamped', unit: '', min: 0, max: 1, scale: 'stepped', rateHz: json().items.READOUT_RATE_HZ.value }]);
  assert.match(header(), /^#define OMX_GEQ_READOUT_DESIGN_CLAMPED_SCALE OMX_PARAM_SCALES_STEPPED$/m);
});

test('perturbation: swapped ids no longer match omx-dsp, and --check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/geq.json', (d, at) => { at('GEQ_MODES').ids = ['true', 'standard']; });
  assert.notDeepEqual(rendered(dir), dspModes());
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
});

test('perturbation: a moved come-up value moves the C default', () => {
  const dir = scratchData();
  editData(dir, 'kernels/geq.json', (d, at) => { at('GEQ_MODES').default = 'true'; });
  assert.match(header(dir), /^#define OMX_GEQ_MODES_DEFAULT OMX_GEQ_MODES_TRUE$/m);
  assert.equal(main(['render', '--target', 'json', '--out', `${RENDERS}/share/omx-contract`, '--check', '--data', dir]), 1);
});
