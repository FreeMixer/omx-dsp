// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// 2.5.0 (#40): the comp's detector topology is a control, detectorTopology (DETECTOR_TOPOLOGY: feedforward, feedback),
// and the feedback loop's one-sample delay is a constant, DYN_FB_LOOP_DELAY_SAMPLES. omx-dsp's feedback compressor
// (its defines kept under test/fixtures/dyn-topology/) defined both locally; its values
// are the render's. The set is named for its control, so C spells omx-dsp's OMX_DYN_TOPOLOGY_* as OMX_DETECTOR_TOPOLOGY_*.
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
const ts = (dir) => renderTs(resolved(dir)).map((f) => f.text).join('\n');
const dsp = new Map([...readFileSync(join(ROOT, 'test/fixtures/dyn-topology/omx_dyn_topology.h'), 'utf8')
  .matchAll(/^#define (OMX_\w+) (\d+)u?$/gm)].map((m) => [m[1], Number(m[2])]));
/** The value the c render gives a name: an enumerator's or a define's. */
const cValue = (h, name) => {
  const m = h.match(new RegExp(`^(?:#define ${name} |  ${name} = )(\\d+)u?,?$`, 'm'));
  return m ? Number(m[1]) : undefined;
};
/** omx-dsp's names whose value the render does not give its contract spelling. */
const disagree = (h) => [
  ['OMX_DYN_TOPOLOGY_FEEDFORWARD', 'OMX_DETECTOR_TOPOLOGY_FEEDFORWARD'],
  ['OMX_DYN_TOPOLOGY_FEEDBACK', 'OMX_DETECTOR_TOPOLOGY_FEEDBACK'],
  ['OMX_DYN_FB_LOOP_DELAY_SAMPLES', 'OMX_DYN_FB_LOOP_DELAY_SAMPLES'],
].filter(([d, c]) => dsp.get(d) !== cValue(h, c)).map(([d, c]) => `${d} ${dsp.get(d)}, ${c} ${cValue(h, c)}`);

test('comp: detectorTopology is the last control, a choice of DETECTOR_TOPOLOGY that comes up feedforward', () => {
  const c = json().kernels.comp.controls;
  assert.deepEqual(c.slice(-1), [{ name: 'detectorTopology', kind: 'choice', global: 'DETECTOR_TOPOLOGY' }]);
  assert.deepEqual(json().items.DETECTOR_TOPOLOGY.value, ['feedforward', 'feedback']);
  assert.equal(json().items.DETECTOR_TOPOLOGY.default, 'feedforward');
  assert.equal(json().items.DYN_FB_LOOP_DELAY_SAMPLES.value, 1);
  assert.equal(json().items.DYN_FB_LOOP_DELAY_SAMPLES.rel, 'data/kernels/comp.json');
});

test('every value omx-dsp\'s feedback compressor defined locally is the c render\'s', () => {
  assert.equal(dsp.size, 3, 'the frozen omx-dsp definitions: two topologies and the loop delay');
  assert.deepEqual(disagree(header()), []);
  const h = header();
  assert.match(h, /^enum omx_detector_topology \{\n {2}OMX_DETECTOR_TOPOLOGY_FEEDFORWARD = 0,\n {2}OMX_DETECTOR_TOPOLOGY_FEEDBACK = 1,\n\};$/m);
  assert.match(h, /^#define OMX_DETECTOR_TOPOLOGY_COUNT 2u$/m);
  assert.match(h, /^#define OMX_DETECTOR_TOPOLOGY_DEFAULT OMX_DETECTOR_TOPOLOGY_FEEDFORWARD$/m);
  assert.match(h, /^#define OMX_DYN_FB_LOOP_DELAY_SAMPLES 1$/m);
  assert.match(ts(), /^export const DETECTOR_TOPOLOGY = \[\n {2}"feedforward",\n {2}"feedback"\n\];$/m);
  assert.match(ts(), /^export const DYN_FB_LOOP_DELAY_SAMPLES = 1;$/m);
});

test('the C render keeps every 2.4.0 line, byte for byte and in order, and adds only the topology set and the loop delay', () => {
  const old = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.4.0/omx_contract_limits.h'), 'utf8').split('\n');
  // the released 2.5.0 header: the release the topology came in (later releases add their own, test/engine-kernels.test.mjs)
  const now = readFileSync(join(ROOT, 'test/fixtures/omx-contract-2.5.0/omx_contract_limits.h'), 'utf8').split('\n');
  let i = 0;
  const added = [];
  for (const line of now) {
    if (i < old.length && line === old[i]) i++;
    else added.push(line);
  }
  assert.equal(i, old.length, `2.4.0 line ${i + 1} is not in the render: ${old[i]}`);
  assert.deepEqual(added, [
    '#define OMX_DYN_FB_LOOP_DELAY_SAMPLES 1',
    'enum omx_detector_topology {',
    '  OMX_DETECTOR_TOPOLOGY_FEEDFORWARD = 0,',
    '  OMX_DETECTOR_TOPOLOGY_FEEDBACK = 1,',
    '};',
    '#define OMX_DETECTOR_TOPOLOGY_COUNT 2u',
    '#define OMX_DETECTOR_TOPOLOGY_DEFAULT OMX_DETECTOR_TOPOLOGY_FEEDFORWARD',
  ]);
});

test('perturbation: a moved loop delay or a reordered topology set disagrees with omx-dsp, and --check goes red', () => {
  const delay = scratchData();
  editData(delay, 'kernels/comp.json', (d, at) => { at('DYN_FB_LOOP_DELAY_SAMPLES').value = 2; });
  assert.deepEqual(disagree(header(delay)), ['OMX_DYN_FB_LOOP_DELAY_SAMPLES 1, OMX_DYN_FB_LOOP_DELAY_SAMPLES 2']);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', delay]), 1);
  const order = scratchData();
  editData(order, 'kernels/comp.json', (d, at) => { at('DETECTOR_TOPOLOGY').ids.reverse(); });
  assert.deepEqual(disagree(header(order)), ['OMX_DYN_TOPOLOGY_FEEDFORWARD 0, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD 1', 'OMX_DYN_TOPOLOGY_FEEDBACK 1, OMX_DETECTOR_TOPOLOGY_FEEDBACK 0']);
  assert.equal(main(['render', '--target', 'c', '--out', `${RENDERS}/include`, '--check', '--data', order]), 1);
});
