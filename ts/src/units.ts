// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/** The dB and linear-amplitude twins of omx_units.h (omx_db_to_lin, omx_lin_to_db, omx_lin_to_db_d), moved verbatim from the console (its output routes). */

/**
 * dB → linear amplitude (0 dB → 1, −6 dB → ~0.501). The trim/fold gain the engine applies.
 *
 * `-Infinity` dB → 0 falls out of the exponential itself (`10 ** -Infinity === 0`), so
 * callers need no silence guard of their own.
 */
export function dbToLinearGain(db: number): number {
  return 10 ** (db / 20);
}

/**
 * Linear amplitude → dB — the exact inverse of {@link dbToLinearGain}, and the reason this
 * one exists: without it the inverse direction was hand-rolled at every call site, which is
 * how three different silence conventions (`1e-12`, `1e-7`, `-60`) grew up elsewhere in the
 * tree for what is nominally the same conversion.
 *
 * Silence and any non-positive amplitude read `-Infinity`, not NaN: `Math.log10` of a
 * negative is NaN, and an amplitude below zero is a phase-inverted sample's magnitude
 * mistake, not a level. Note this is the LEVEL conversion — an analyser floors its own
 * spectrum at a chosen epsilon instead, deliberately, and should keep doing so.
 */
export function linearGainToDb(linear: number): number {
  return linear <= 0 ? -Infinity : 20 * Math.log10(linear);
}

/**
 * The analysers' magnitude floor: an FFT bin or an envelope at exact zero reads −240 dB, below
 * any real signal and finite for the growth and ratio arithmetic built on it (was the epsilon
 * `1e-12` inside three analysers' logarithms).
 */
export const ANALYSIS_FLOOR_DB = -240;

/**
 * Linear amplitude → dB, never below `floorDb` — the conversion for a site whose floor is
 * deliberate (an analyser's magnitude, a plot's axis, a meter that must stay finite). The floor
 * is a named dB value, never an epsilon hidden inside the logarithm (`Math.max(x, 1e-12)` is
 * `linearToDbFloored(x, -240)`); silence reads `floorDb`, not `-Infinity`.
 */
export function linearToDbFloored(linear: number, floorDb: number): number {
  return Math.max(floorDb, linearGainToDb(linear));
}
