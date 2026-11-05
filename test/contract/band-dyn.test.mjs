// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The band-dynamics kernel's constants, the two it still declares: the knee and the detector's attack floor. Its six
// controls and BAND_DYN_MAX_BANDS were retired in dfbc289 (no reader; dyneq's refs take the values they forwarded), so
// the arms over them went with them.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { limitUnits, loadData, renderC, resolveData } from './oc.mjs';
import { UNION, editData, scratchData } from './helpers.mjs';

const resolved = (dir = UNION) => resolveData(loadData(dir));
const header = (r) => renderC(r).find((f) => f.path === 'omxcontract/omx_contract_limits.h').text;

test('the attack floor is the detector constant, 0.5 ms, and the knee is 6 dB as omx-dsp\'s test spells it', () => {
  const r = resolved();
  assert.equal(r.get('DETECTOR_OVERSAMPLE_AUTO_MS').value, 0.5);
  assert.equal(r.get('BAND_DYN_KNEE_DB').value, 6);
});

test('the c limits header defines the knee with its value, as a limit unit of its own', () => {
  const text = header(resolved());
  assert.ok(text.includes('#define OMX_BAND_DYN_KNEE_DB 6\n'));
  const keys = new Set(limitUnits(text).filter((u) => u.frame === undefined).map((u) => u.key));
  assert.ok(keys.has('OMX_BAND_DYN_KNEE_DB'));
});

test('a moved knee moves its define', () => {
  const dir = scratchData();
  editData(dir, 'kernels/band_dyn.json', (d, at) => { at('BAND_DYN_KNEE_DB').value = 7; });
  assert.ok(header(resolved(dir)).includes('#define OMX_BAND_DYN_KNEE_DB 7\n'));
});
