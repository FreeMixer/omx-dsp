// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * What moved in these definitions since the pins the proofs read (v0.1.3's header, the units 4108494 read), each with
 * the commit that moved it and why: one list, which tools/contract-proof.mjs and the declaration tests of
 * test/contract/ read. Each list is exact: a name on it that did not move is a failure of the proof.
 */

/**
 * The units whose value moved after v0.1.3, each with the reason. The definitions declare the value in use today;
 * the v0.1.3 header keeps the value of its day.
 */
export const MOVED_AFTER_PIN = new Map([
  ['OMX_INPUT_ALIGN_DELAY_RANGE_STEP', 'the input alignment step went from 0.01 ms to 0.0001 ms when the alignment line became a fractional read'],
  ['OMX_INPUT_ALIGN_DELAY_RANGE_STEP_DOUBLE', 'the same step, as a double'],
  ['OMX_EQ_NOTCH_Q_RANGE_STEP', 'dbcb2cb: 1 to 0.1, as a travel\'s step must reach its maximum and default from its minimum, and 0.3 + n never lands on 116'],
]);

/** The units a first-release item gained after v0.1.3 (a sheet's new field), each with the reason. */
export const ADDED_AFTER_PIN = new Map([
  ['OMX_PITCH_PREFILTER_NYQUIST_FRACTION', '1366d77: the pitch pre-filter corner is also capped at this fraction of Nyquist, so it stays off Nyquist at 32 kHz'],
  ['OMX_PITCH_PREFILTER_NYQUIST_FRACTION_DOUBLE', 'the same fraction, as a double'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_FAST_MS', 'b5bfafe: the release mode became fixed, opto or auto, and auto took its own profile'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_SLOW_MIN_MS', 'the same profile'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_SLOW_MAX_MS', 'the same profile'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_SHARE', 'the same profile'],
  ['OMX_PROGRAM_RELEASE_PROFILES_AUTO_SHARE_DOUBLE', 'the same profile, as a double'],
]);

/** The units v0.1.3 read that the definitions no longer carry, each with the reason. */
export const DROPPED_AFTER_PIN = new Map([
  ['OMX_DELAY_TIME_MS_MAX', 'e2a13a0: the c.alias blocks left the definitions and the header lost its alias section; omx_delay.h reads OMX_FX_DELAY_TIME_RANGE_MAX'],
]);

/**
 * The release line `since` is read in: omx-contract's, frozen at the move. An item first shipped before the definitions
 * moved here says the omx-contract release that carried it (1.0.0 to MOVED_AT); one first shipped after says AFTER_MOVE,
 * and its origin sentence names the omx-dsp release and commit. recipes/README.md states the rule.
 */
export const MOVED_AT = '2.14.0';
export const AFTER_MOVE = '3.0.0';
