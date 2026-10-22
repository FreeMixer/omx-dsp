// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @openmixer/omx-dsp: the TypeScript twins of omx-dsp kernels, the ones that stay in TypeScript because doing
 * the work in C would be silly (a display, a unit conversion a browser runs). Each is pinned to its kernel by an
 * agreement test in test/ that feeds both the same grid.
 */
export { ANALYSIS_FLOOR_DB, dbToLinearGain, linearGainToDb, linearToDbFloored } from './units.js';
export { octavesToQ } from './eq.js';
export { fftSizeForRate } from './rta.js';
