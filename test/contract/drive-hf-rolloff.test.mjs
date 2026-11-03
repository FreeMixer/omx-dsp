// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// 2.9.0 (#29): the drive's hf roll-off ids are the console's, off, 12000, 16000 (decided 2026-10-13), each
// carrying its corner in Hz as its value and off none, so no 0 Hz doubles as "no roll-off". The console re-exports
// DRIVE_HF_ROLLOFFS; a kernel reads the corner from the id with omx_drive_hf_rolloffs_value(). The C render keeps
// every line of the released 2.10.0 header but the roll-off set's, the hosted stage's numbers (2.12.0) and the EQ curve grid's (2.13.0).
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { main } from './oc.mjs';
import { loadData, resolveData } from './oc.mjs';
import { renderC } from './oc.mjs';
import { renderJson } from './oc.mjs';
import { renderTs } from './oc.mjs';
import { DATA, RENDERS, ROOT, UNION, editData, scratchData } from './helpers.mjs';

const resolved = (dir = UNION) => resolveData(loadData(dir));
const header = (dir) => renderC(resolved(dir))[0].text;
const json = (dir) => JSON.parse(renderJson(resolved(dir), '0.0.0')[0].text);

test('drive: DRIVE_HF_ROLLOFFS is off, 12000, 16000, carrying none, 12000 and 16000 Hz, and comes up off', () => {
  const it = json().items.DRIVE_HF_ROLLOFFS;
  assert.deepEqual([it.value, it.values, it.unit, it.default], [['off', '12000', '16000'], [null, 12000, 16000], 'Hz', 'off']);
  const js = renderTs(resolved())[0].text;
  assert.match(js, /^export const DRIVE_HF_ROLLOFFS = \[\n {2}"off",\n {2}"12000",\n {2}"16000"\n\];$/m, 'the console\'s wire vocabulary, as it re-exports it');
  // an object's integer-like keys come first in JavaScript, so the map is read by id, not in order
  const m = /^export const DRIVE_HF_ROLLOFFS_VALUES = (\{[^;]*\});$/m.exec(js);
  assert.ok(m, 'the ts render exports DRIVE_HF_ROLLOFFS_VALUES');
  assert.deepEqual(JSON.parse(m[1]), { off: null, 12000: 12000, 16000: 16000 });
});

/** The default EQ rule's two functions, under their comments, as the released headers carried them until 3.0.0. */
const EQ_DEFAULT_RULE = /\/\* The default bands of an EQ [\s\S]*?omx_eq_default_type\([\s\S]*?\n\}\n\n/;
/** The c.alias section (a travel's second C spelling), as the released headers carried it until 3.0.0 dropped it. */
const ALIASES_RULE = /\/\* The second C spelling of a travel[\s\S]*?\n\n(?=\/\* Every scalar)/;

test('the C render is the 2.10.0 header with the roll-off set\'s block replaced, which gives the kernel each corner from its id', () => {
  const old = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.10.0/omx_contract_limits.h'), 'utf8');
  const was = [
    '  OMX_DRIVE_HF_ROLLOFFS_0 = 0,',
    '  OMX_DRIVE_HF_ROLLOFFS_12000 = 12000,',
    '  OMX_DRIVE_HF_ROLLOFFS_16000 = 16000,',
    '};',
    '#define OMX_DRIVE_HF_ROLLOFFS_COUNT 3u',
    '#define OMX_DRIVE_HF_ROLLOFFS_DEFAULT OMX_DRIVE_HF_ROLLOFFS_0',
  ].join('\n');
  const is = [
    '  OMX_DRIVE_HF_ROLLOFFS_OFF = 0,',
    '  OMX_DRIVE_HF_ROLLOFFS_12000 = 1,',
    '  OMX_DRIVE_HF_ROLLOFFS_16000 = 2,',
    '};',
    '#define OMX_DRIVE_HF_ROLLOFFS_COUNT 3u',
    '#define OMX_DRIVE_HF_ROLLOFFS_DEFAULT OMX_DRIVE_HF_ROLLOFFS_OFF',
    '#define OMX_DRIVE_HF_ROLLOFFS_UNIT "Hz"',
    '#define OMX_DRIVE_HF_ROLLOFFS_12000_VALUE 12000.0f',
    '#define OMX_DRIVE_HF_ROLLOFFS_16000_VALUE 16000.0f',
    'static inline int omx_drive_hf_rolloffs_value(int id, float *out) {',
    '  switch (id) {',
    '  case OMX_DRIVE_HF_ROLLOFFS_12000: *out = OMX_DRIVE_HF_ROLLOFFS_12000_VALUE; return 1;',
    '  case OMX_DRIVE_HF_ROLLOFFS_16000: *out = OMX_DRIVE_HF_ROLLOFFS_16000_VALUE; return 1;',
    '  default: return 0;',
    '  }',
    '}',
  ].join('\n');
  assert.equal(old.split(was).length, 2, 'the 2.10.0 header holds the old block once');
  // 2.12.0 adds the hosted stage's numbers (data/hosting.json) and 2.13.0 the EQ curve grid (data/curves.json),
  // which the 2.10.0 header does not hold, and the curve and band come-ups (#46), and the delay's read-tap travel and
  // per-tap limits, and the graphic EQ's mode set and designClamped readout (#71), the multiband compressor's limits (#62), the delay's tap factor lists (#63), and the kernel travels and constants of 2.14.0 (#44), the biquad kinds and the low-pass hand-over table (#86), and the HRP family set and search (#85),
  // and 2.16.0 renames RME_RATES to ORACLE_FULL_RATES (#102; its values are held to the 2.10.0 ones by
  // test/formerly.test.mjs, and 2.19.0 drops the old name, #125), and 2.16.1 rewords the prose above the rate tail (#108; the prose
  // is left out on both sides, the tail's defines are held), and 3.0.0 drops the default EQ rule's functions, which
  // left for omx-dsp (#1841), and drops the c.alias section (a travel's second C spelling): no consumer reads an
  // alias spelling any more, so the word and its render are gone outright
  const tailProse = (h) => h.replace(/\n\/\*(?:(?!\*\/)[^])*\*\/\n(?=#ifndef OMX_DECLARED_RATE_COUNT\n)/, '\n');
  assert.equal(tailProse(header().replace(/^#define OMX_ORACLE_FULL_RATES_\w+ .+\n/gm, '').replace(/^#define OMX_PARAM_CONTRACT_UNITS_\w+ .+\n/gm, '').replace(/^#define OMX_STANDARD_QUANTA_\w+ .+\n/gm, '').replace(/^enum omx_instrument_families \{[\s\S]*?#define OMX_INSTRUMENT_FAMILIES_COUNT \d+u\n/m, '').replace(/^#define OMX_HRP_FAMILY_SEARCH_\w+ \S+\n/gm, '').replace(/^enum omx_biquad_kinds \{[\s\S]*?#define OMX_BIQUAD_KINDS_COUNT \d+u\n/m, '').replace(/^#define OMX_LOWPASS_HANDOVER_\w+ \S+\n/gm, '').replace(/^#define OMX_DELAY_TAP_FACTOR_(NUMS|DENS)_\w+ .+\n/gm, '').replace(/^#define OMX_MBC_\w+ .+\n/gm, '').replace(/^#define OMX_(DETECTOR_OVERSAMPLE_LATENCY_SAMPLES|DIVERGENCE_RANGE_\w+|FBS_(AUTO_BANDS|COOLDOWN_MINUTES|HOLD_SECONDS|MERGE_CENTS|SENSITIVITY)_RANGE_\w+|FBS_DEFAULT_(HOLD_SECONDS|SENSITIVITY)|HRP_AUTO_BANDS_RANGE_\w+|HRP_HARMONIC_COUNT|INPUT_ALIGN_DELAY_RANGE_\w+|OSCILLATOR_FREQ_RANGE_\w+|OUTPUT_DELAY_RANGE_\w+) \S+\n/gm, '').replace(/^enum omx_geq_(modes|readouts) \{\n[\s\S]*?\n\};\n(#define OMX_GEQ_\w+ \S+\n)+/gm, '').replace(/^#define OMX_DELAY_TAP_TAP0_GAIN_MIN [\s\S]*?OMX_DELAY_TAP_TAP3_PAN_DEFAULT \S+\n\n/m, '').replace(/^#define OMX_(HOSTED_STAGE|EQ_CURVE_GRID|DELAY_TAPS|FX_DELAY_TAPS_RANGE)_\w+ \S+\n/gm, '').replace(/^#define OMX_DRIVE_(CURVES|BANDS)_DEFAULT \S+\n/gm, '')), tailProse(old.replace(was, is).replace(/^#define OMX_RME_RATES_\w+ .+\n/gm, '').replace(EQ_DEFAULT_RULE, '').replace(ALIASES_RULE, '')));
});

test('perturbation: a moved corner moves the C value, and --check goes red', () => {
  const dir = scratchData();
  editData(dir, 'kernels/drive.json', (d, at) => { at('DRIVE_HF_ROLLOFFS').values[1] = 10000; });
  assert.match(header(dir), /^#define OMX_DRIVE_HF_ROLLOFFS_12000_VALUE 10000\.0f$/m);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', dir]), 1);
});
