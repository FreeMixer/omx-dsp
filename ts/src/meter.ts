// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * The meter view law, the twin of mix/omx_meter_view.h: how a measured value is drawn, the same on every surface.
 * The fill of a dB scale from the floor to 0 dBFS, the peak marker that takes a louder value at once and falls by
 * elapsed time (the same seconds give the same fall at any draw rate, and it falls on silence), the clip word that
 * stays lit until cleared, the stepped count of an LED ladder with its hysteresis (a bar has no release), the
 * gain-reduction reading recovered by the time elapsed at the reader's tick (the twin of omx_meter_gr_step), and what a
 * decimating subscriber keeps. The numbers are METER_VIEW, declared in data/primitives/meter.json. Pinned by
 * test/meter.test.mjs: against the kernel through the oracle at the nine oracle rates, and against
 * vectors/meter-view.json, the file every copy of the law reads.
 */
import { LIN_TO_DB_FLOOR, METER_DECIMATION, METER_KINDS, METER_MEASUREMENTS, METER_UNITS, METER_UNITS_LABELS, METER_VIEW } from './definitions.js';

/** The wire words of the meter sets, in value order: `gain-reduction`, `true-peak`, `lufs-m`…; units as labelled. */
export const METER_WORDS = {
  kinds: METER_KINDS.map((id) => id.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)),
  measurements: METER_MEASUREMENTS.map((id) => id.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)),
  units: [...METER_UNITS_LABELS],
} as const;

/** A meter's kind, as METER_KINDS spells it. */
export type MeterKind = (typeof METER_KINDS)[number];

/** `x` held to 0..1; NaN reads 0. */
const unit = (x: number): number => (x > 0 ? (x < 1 ? x : 1) : 0);

/** The fill of a dB level, linear in dB from `floorDb` (0) to 0 dBFS (1); NaN and -Infinity read 0. Twin of omx_meter_view_fill_db. */
export function meterFillDb(db: number, floorDb: number = METER_VIEW.floorDb): number {
  if (!(db > floorDb)) return 0;
  if (db >= 0) return 1;
  return (db - floorDb) / -floorDb;
}

/** The fill of a linear peak (1 is 0 dBFS) on the same dB scale. Twin of omx_meter_view_fill. */
export function meterFill(level: number, floorDb: number = METER_VIEW.floorDb): number {
  if (!(level > 0)) return 0;
  if (level >= 1) return 1;
  return meterFillDb(20 * Math.log10(level), floorDb);
}

/** One meter's peak marker between draw ticks, the state of struct omx_meter_view. */
export interface MeterMarker {
  /** The fill the marker last took. */
  readonly peak: number;
  /** Seconds of draw ticks since it took it. */
  readonly elapsedS: number;
  /** The live fill of the last tick. */
  readonly live: number;
  /** The marker drawn at the last tick. */
  readonly marker: number;
}

/** A fresh marker: empty. */
export const METER_MARKER_EMPTY: MeterMarker = { peak: 0, elapsedS: 0, live: 0, marker: 0 };

/**
 * One draw tick of the peak marker: a fill at or above it is taken at once; below it the marker falls
 * `fallDbPerSecond` for each second elapsed since it took its value, never under the live fill. `dtS` is the time
 * since the last tick (negative or NaN counts as 0). Twin of omx_meter_view_step.
 */
export function meterMarker(prev: MeterMarker | undefined, fill: number, dtS: number, fallDbPerSecond: number = METER_VIEW.fallDbPerSecond, floorDb: number = METER_VIEW.floorDb): MeterMarker {
  const p = prev ?? METER_MARKER_EMPTY;
  const f = unit(fill);
  const elapsedS = p.elapsedS + (dtS > 0 ? dtS : 0);
  const m = p.peak - (fallDbPerSecond * elapsedS) / -floorDb;
  if (!(m > f)) return { peak: f, elapsedS: 0, live: f, marker: f };
  return { peak: p.peak, elapsedS, live: f, marker: m };
}

/** The clip word for one tick: lit by a clip, and lit until the surface clears it (sets it false). Twin of omx_meter_view_clip. */
export function meterClip(lit: boolean, clip: boolean): boolean {
  return lit || clip;
}

/**
 * A stepped display's count (an LED ladder of `steps` segments) with hysteresis against the count it showed last: it
 * holds while the unrounded value stays within `0.5 + hysteresis` steps of it. `prevStep` undefined (or negative) is a
 * fresh or reset strip: plain rounding. Twin of omx_meter_view_ladder.
 */
export function meterStep(prevStep: number | undefined, fill: number, steps: number, hysteresis: number = METER_VIEW.stepHysteresis): number {
  const raw = unit(fill) * steps;
  if (prevStep !== undefined && prevStep >= 0 && Math.abs(raw - prevStep) <= 0.5 + hysteresis) return prevStep;
  return Math.floor(raw + 0.5);
}

/** A stepped BAR for one tick: a linear peak to its fill, then its count against the last one. No release. Twin of omx_meter_view_bar_step. */
export function meterBarStep(prevStep: number | undefined, level: number, steps: number, hysteresis: number = METER_VIEW.stepHysteresis): number {
  return meterStep(prevStep, meterFill(level), steps, hysteresis);
}

/** What a decimating subscriber keeps of two frames (METER_DECIMATION): a level the peak, a gain reduction the deepest; a NaN frame is never kept. Twin of omx_meter_view_decimate. */
export function meterDecimate(kind: MeterKind, kept: number, value: number): number {
  if (METER_DECIMATION[METER_KINDS.indexOf(kind)] === 'deepest') return value < kept ? value : kept;
  return value > kept ? value : kept;
}

/** A level's measurement, as METER_MEASUREMENTS spells it. */
export type MeterMeasurement = (typeof METER_MEASUREMENTS)[number];
/** A meter's unit, as METER_UNITS spells it (its label in METER_UNITS_LABELS). */
export type MeterUnit = (typeof METER_UNITS)[number];

const UNIT_OF: Record<MeterMeasurement, MeterUnit> = { peak: 'dbfs', truePeak: 'dbtp', lufsM: 'lufs', lufsS: 'lufs', lufsI: 'lufs', lra: 'lu' };

/** The unit a meter's values are in: a level's from its measurement, a gain reduction's dB. Twin of omx_meter_view_unit_of. */
export function meterUnitOf(kind: MeterKind, measurement: MeterMeasurement): MeterUnit {
  return kind === 'gainReduction' ? 'db' : UNIT_OF[measurement];
}

/** The gain-reduction reading's hold, ms: OMX_METER_GR_HOLD_MS of mix/omx_meter.h (the oracle pins it). */
export const METER_GR_HOLD_MS = 300;

/** One gain-reduction reading, the state of struct omx_meter_gr: the reading (dB, <= 0) and the hold still owed (s). */
export interface MeterGr {
  readonly db: number;
  readonly holdLeftS: number;
}

/** No reduction, no hold. */
export const METER_GR_EMPTY: MeterGr = { db: 0, holdLeftS: 0 };

const f32 = Math.fround;

/**
 * One step of a gain-reduction reading over `n` frames at `sr`: a block as deep or deeper replaces it and restarts the
 * hold; a shallower one spends the hold, then the reading closes on it as a linear gain by `releasePole^n`, and lands
 * within 0.001 dB. Float arithmetic, as the kernel's. Twin of omx_meter_gr_step.
 */
export function meterGrStep(m: MeterGr, blockDb: number, n: number, sr: number, releasePole: number): MeterGr {
  const dt = f32(f32(n) / f32(sr));
  const b = f32(blockDb);
  if (b <= m.db) return { db: b, holdLeftS: f32(f32(METER_GR_HOLD_MS) * f32(0.001)) };
  if (m.holdLeftS > 0) return { db: m.db, holdLeftS: f32(m.holdLeftS - dt) };
  const pole = f32(releasePole);
  const k = pole >= 0 && pole < 1 ? f32(Math.pow(pole, f32(n))) : 0;
  const lin = (db: number): number => f32(Math.pow(10, f32(db * f32(0.05))));
  const want = lin(b);
  const g = f32(want + f32(f32(lin(m.db) - want) * k));
  let db = f32(f32(20) * f32(Math.log10(g < f32(LIN_TO_DB_FLOOR.lin) ? f32(LIN_TO_DB_FLOOR.lin) : g)));
  if (!(db < f32(b - f32(0.001)))) db = b;
  return { db, holdLeftS: m.holdLeftS };
}

/** The gain-reduction reading at the reader's tick: meterGrStep over the frames `dtS` seconds cover at `sr`. Twin of omx_meter_view_gr. */
export function meterGrView(m: MeterGr, deepestDb: number, dtS: number, sr: number, releasePole: number): MeterGr {
  return meterGrStep(m, deepestDb, dtS > 0 ? Math.floor(dtS * sr + 0.5) : 0, sr, releasePole);
}
