// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The FX delay's read taps: the tap-count travel (FX_DELAY_TAPS_RANGE, rendered as the integer
 * OMX_FX_DELAY_TAPS_RANGE_MAX the kernel sizes its tap arrays by), the flat DELAY_TAP_LIMITS sheet keyed by the delay
 * row's own field, and the tap factor set as two integer lists in member order, which omx_delay_instance.h takes from
 * the render. (omx-contract held these over its 2.14.0 copy of this data, against a 0f23bed excerpt of the kernel's
 * own lists; the kernel now reads the render.)
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { loadData, renderC, renderJson, resolveData } from './oc.mjs';
import { HERE, UNION } from './helpers.mjs';

const items = () => JSON.parse(renderJson(resolveData(loadData(UNION)), '0.0.0')[0].text).items;
const header = () => renderC(resolveData(loadData(UNION))).find((f) => f.path === 'omxcontract/omx_contract_limits.h').text;
const src = (p) => readFileSync(join(HERE, p), 'utf8');

test('the tap count travels 1..4 step 1 and comes up at 1, rendered as the integer OMX_FX_DELAY_TAPS_RANGE_MAX; 3.0.0 dropped its float alias', () => {
  assert.deepEqual(items().FX_DELAY_TAPS_RANGE.value, { min: 1, max: 4, step: 1, unit: '', default: 1, defaultFrom: 'desk' });
  assert.match(header(), /^#define OMX_FX_DELAY_TAPS_RANGE_MAX 4$/m);
  assert.doesNotMatch(header(), /OMX_DELAY_TAPS_MAX/);
});

test('each tap carries its own gain and pan limit, keyed by the delay row field', () => {
  const t = items().DELAY_TAP_LIMITS.value;
  assert.deepEqual(Object.keys(t), ['tap0Gain', 'tap0Pan', 'tap1Gain', 'tap1Pan', 'tap2Gain', 'tap2Pan', 'tap3Gain', 'tap3Pan']);
  assert.deepEqual(Object.fromEntries(Object.entries(t).map(([k, v]) => [k, v.default])), { tap0Gain: 1, tap0Pan: 0, tap1Gain: 0.7, tap1Pan: -0.5, tap2Gain: 0.5, tap2Pan: 0.5, tap3Gain: 0.35, tap3Pan: 0 });
  for (const [k, v] of Object.entries(t)) assert.deepEqual([v.min, v.max, v.step], k.endsWith('Gain') ? [0, 1, 0.01] : [-1, 1, 0.01], k);
});

test('the tap factor set: eleven members 1/4 .. 3 in order, every one a positive ratio, ascending', () => {
  const it = items();
  const num = it.DELAY_TAP_FACTOR_NUMS.value;
  const den = it.DELAY_TAP_FACTOR_DENS.value;
  assert.equal(den.length, num.length);
  assert.deepEqual(num.map((n, i) => (den[i] === 1 ? `${n}` : `${n}/${den[i]}`)), ['1/4', '1/3', '3/8', '1/2', '2/3', '3/4', '1', '4/3', '3/2', '2', '3']);
  assert.ok(num.every((n, i) => i === 0 || n * den[i - 1] > num[i - 1] * den[i]), 'ascending');
});

test('the kernel takes its tap ceiling and its factor lists from the render, not from a copy', () => {
  assert.match(src('include/omxdsp/fx/omx_delay.h'), /^#define OMX_FXDELAY_MAX_TAPS OMX_FX_DELAY_TAPS_RANGE_MAX$/m);
  const inst = src('include/omxdsp/fx/omx_delay_instance.h');
  assert.match(inst, /\[OMX_DELAY_TAP_FACTOR_NUMS_COUNT\] = OMX_DELAY_TAP_FACTOR_NUMS_INIT;/);
  assert.match(inst, /\[OMX_DELAY_TAP_FACTOR_DENS_COUNT\] = OMX_DELAY_TAP_FACTOR_DENS_INIT;/);
});
