// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The HRP instrument families (#85): the family set INSTRUMENT_FAMILIES (primitives.json) and the search each family implies,
 * HRP_FAMILY_SEARCH. Both are read by omx-dsp (enum OmxHrpFamily and omx_hrp_family_search in
 * include/omxdsp/analysis/omx_hrp_correct.h) and by the console engine's core (INSTRUMENT_FAMILIES in
 * instrument-templates.ts, HRP_FAMILY_SEARCH in hrp.ts). The values below are the ones both sides hold today;
 * a drift in the data turns these red, and moving one moves every render and turns `render --check` red.
 */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { renderTs } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, relOf, scratchData } from './helpers.mjs';

const json = (dir = UNION) => JSON.parse(renderJson(resolveData(loadData(dir)), '0.0.0')[0].text);
const header = (dir = UNION) => renderC(resolveData(loadData(dir)))[0].text;

// omx-dsp's OmxHrpFamily order, which is also the console's INSTRUMENT_FAMILIES order.
const FAMILIES = ['drums', 'percussion', 'bass', 'guitar', 'keys', 'vocals', 'brass', 'wind', 'strings', 'playback'];
// omx-dsp's omx_hrp_family_search and the console's HRP_FAMILY_SEARCH, family by family.
const SEARCH = {
  drums: { maxVoices: 1, f0MinHz: 35, f0MaxHz: 400 },
  percussion: { maxVoices: 1, f0MinHz: 60, f0MaxHz: 1200 },
  bass: { maxVoices: 2, f0MinHz: 25, f0MaxHz: 400 },
  guitar: { maxVoices: 6, f0MinHz: 60, f0MaxHz: 1400 },
  keys: { maxVoices: 8, f0MinHz: 27.5, f0MaxHz: 4200 },
  vocals: { maxVoices: 2, f0MinHz: 80, f0MaxHz: 1200 },
  brass: { maxVoices: 2, f0MinHz: 40, f0MaxHz: 1400 },
  wind: { maxVoices: 2, f0MinHz: 55, f0MaxHz: 2600 },
  strings: { maxVoices: 8, f0MinHz: 40, f0MaxHz: 2800 },
  playback: { maxVoices: 8, f0MinHz: 25, f0MaxHz: 2100 },
};

test('the families are the set INSTRUMENT_FAMILIES, in the order omx-dsp numbers them, with no come-up id', () => {
  const j = json();
  assert.deepEqual(j.items.INSTRUMENT_FAMILIES.value, FAMILIES);
  assert.equal(j.items.INSTRUMENT_FAMILIES.default, undefined, 'no family is -1, which no id names');
  assert.ok(!j.kernels.hrp.controls.some((c) => c.global === 'INSTRUMENT_FAMILIES'), 'a shared vocabulary, not an hrp control');
  FAMILIES.forEach((f, i) => assert.match(header(), new RegExp(`^  OMX_INSTRUMENT_FAMILIES_${f.toUpperCase()} = ${i},$`, 'm')));
  assert.match(header(), /^#define OMX_INSTRUMENT_FAMILIES_COUNT 10u$/m);
});

test('the rendered names never collide with the OMX_HRP_FAMILY_* enum omx-dsp spells today', () => {
  assert.doesNotMatch(header(), /OMX_HRP_FAMILY_(NONE|COUNT|DRUMS|PERCUSSION|BASS|GUITAR|KEYS|VOCALS|BRASS|WIND|STRINGS|PLAYBACK)\b/);
});

test('each family carries its voices and fundamental range, one entry per family id', () => {
  const s = json().items.HRP_FAMILY_SEARCH.value;
  assert.deepEqual(Object.keys(s).sort(), [...FAMILIES].sort());
  assert.deepEqual(s, SEARCH);
  assert.match(header(), /^#define OMX_HRP_FAMILY_SEARCH_KEYS_F0_MIN_HZ 27\.5f$/m);
  assert.match(header(), /^#define OMX_HRP_FAMILY_SEARCH_KEYS_F0_MIN_HZ_DOUBLE 27\.5$/m);
  assert.match(header(), /^#define OMX_HRP_FAMILY_SEARCH_GUITAR_MAX_VOICES 6$/m);
});

test('perturbation: move a family\'s range, every render follows and --check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/hrp.json', (d, at) => { at('HRP_FAMILY_SEARCH').value.wind.f0MaxHz = 2700; });
  assert.match(header(dir), /^#define OMX_HRP_FAMILY_SEARCH_WIND_F0_MAX_HZ 2700$/m);
  assert.match(renderTs(resolveData(loadData(dir)))[0].text, /"f0MaxHz": 2700/);
  assert.equal(json(dir).items.HRP_FAMILY_SEARCH.value.wind.f0MaxHz, 2700);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
  assert.equal(main(['render', '--target', 'json', '--out', `${RENDERS}/share/omx-contract`, '--check', '--data', dir]), 1);
});

test('perturbation: reorder two families, the integers move and --check goes red', () => {
  const dir = scratchData();
  editData(dir, relOf('INSTRUMENT_FAMILIES').slice('data/'.length), (d) => { const ids = d.INSTRUMENT_FAMILIES.ids; [ids[0], ids[1]] = [ids[1], ids[0]]; });
  assert.match(header(dir), /^  OMX_INSTRUMENT_FAMILIES_PERCUSSION = 0,$/m);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
});
