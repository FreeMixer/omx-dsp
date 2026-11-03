// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The items omx-contract 1.3.0 added that these definitions now own are all still declared, and the union resolves.
// (omx-contract held this list over its 2.14.0 copy of this data.)
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { loadData, resolveData } from './oc.mjs';
import { UNION } from './helpers.mjs';

test('the shipped data resolves, and the 1.3.0 items are all there', () => {
  const r = resolveData(loadData(UNION));
  for (const n of ['FBS_TRACK_SLOTS', 'DEFAULT_THRESHOLDS', 'HRP_MAX_VOICES', 'HRP_AMOUNT_RANGE', 'RTA_FFT_SIZE_MAX', 'BUS_INPUT_CAP', 'OPERATOR_EQ_BANDS_RESERVE',
    'EQ_BAND_TYPES', 'FILTER_SLOPES', 'EQ_BAND_COUNTS', 'DEESS_MODES', 'DRIVE_CURVES', 'DRIVE_BANDS', 'REVERB_ALGORITHMS', 'ROTARY_SPEEDS', 'TREMOLO_MODES',
    'GATE_KEY_SOURCES', 'PARAM_SCALES', 'ORACLE_FLOOR_RATES', 'ORACLE_FULL_RATES', 'CHORUS_BASE_MS', 'FLANGER_BASE_MS', 'PHASER_F_TOP_HZ', 'REVERB_GATE_ATTACK_MS']) {
    assert.ok(r.has(n), n);
  }
  assert.deepEqual(r.get('EQ_BAND_COUNTS').value.channel, { default: 4, max: 24 });
});
