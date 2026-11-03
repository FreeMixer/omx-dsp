// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// The derived values these definitions declare evaluate to the numbers the kernels compute with, and a reader follows
// its target: the Butterworth Q (1/√2) and the three sheets that read it, the fractional-delay read's L1 norm, and the
// graphic EQ's band count, the length of its centre list. (omx-contract held these over its 2.14.0 copy of this data.)
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { lagrangeReadL1 } from './oc.mjs';
import { relOf, resolvedOf, texts } from './helpers.mjs';

test('the derivations evaluate to the values the kernels compute with', () => {
  const r = resolvedOf(texts());
  assert.equal(r.get('BUTTERWORTH_Q').value, Math.SQRT1_2);
  assert.equal(r.get('DRIVE_BIAS_MAX').value, Math.SQRT1_2);
  assert.equal(r.get('XOVER_LIMITS').value.lr4SectionQ, Math.SQRT1_2);
  assert.equal(r.get('PITCH_KERNEL').value.prefilterQ, Math.SQRT1_2);
  assert.equal(r.get('FDELAY_READ_L1_NORM').value, lagrangeReadL1(r.get('FDELAY_MOD_READ_ORDER').value));
  assert.equal(r.get('FDELAY_READ_L1_NORM').value, 1.25);
  assert.equal(r.get('GEQ_BANDS').value, r.get('ISO_THIRD_OCTAVE_CENTRES_HZ').value.length);
});

test('a reference follows its target: move BUTTERWORTH_Q and its readers move', () => {
  const r = resolvedOf(texts((d) => { d[relOf('BUTTERWORTH_Q')].BUTTERWORTH_Q.value = { derive: 'sqrt', of: 0.49 }; }));
  assert.equal(r.get('XOVER_LIMITS').value.lr4SectionQ, 0.7);
  assert.equal(r.get('PITCH_KERNEL').value.prefilterQ, 0.7);
});
