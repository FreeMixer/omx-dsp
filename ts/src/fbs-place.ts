// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * FbsPlace, the twin of omx_fbs_place.h (src/omx_fbs_place.c): the feedback corrector's placement, the console's
 * FbsCorrector rules over a fixed set of slots. The C placement and this one read the same vector file
 * (test/analysis/fixtures/fbs_place.vectors) in test/analysis/fbs_place.test.c and test/fbs-place.test.mjs, so the
 * two are held to the same values: bit-equal where the rule is ordering or integer, within the file's stated
 * tolerance where it is float (a gain stage's power).
 *
 * The rules are the header's, one method each, so a test can break one rule at a time. Time is the caller's, in
 * ms; a release ramp's steps fire when the clock reaches them, before the tick at that time.
 */
import {
  CURVE_ORIGINS,
  CURVE_SHAPES,
  EQ_FREQ_RANGE,
  EQ_GAIN_RANGE,
  EQ_NOTCH_Q_RANGE,
  EQ_Q_RANGE,
  FBS_BANDS_RANGE,
  FBS_COOLDOWN_RANGE,
  FBS_DEFAULT_COOLDOWN_SECONDS,
  FBS_DEFAULT_HOLD_SECONDS,
  FBS_DEFAULT_MAX_AUTO_BANDS,
  FBS_DEFAULT_MERGE_CENTS,
  FBS_DEFAULT_NOTCH_Q,
  FBS_HOLD_RANGE,
  FBS_LIFT_RAMP_MS,
  FBS_LIFT_RAMP_STEPS,
  FBS_MAX_CUT_DB,
  FBS_MERGE_RANGE,
  FBS_RECLAMP_BACKOFF_CAP,
  FBS_RECLAMP_DEEPEN_DB,
  FBS_RELEASE_DONE_DB,
  FBS_RELEASE_DWELL_MS,
  FBS_RELEASE_RATIO,
  FBS_RING_HZ_MAX,
  FBS_RING_HZ_MIN,
  FBS_RING_MEMORY_SLOTS,
  FBS_SAME_RING_HZ,
  FBS_SAME_RING_SHARE,
} from './definitions.js';

/** The pools: fixed (ring-out) and live, as OmxFbsPool numbers them. */
export type FbsPool = 'fixed' | 'live';

/** The console's FbsAction words, in OmxFbsAction's order. */
export const FBS_ACTIONS = ['notched', 'lifted', 'pool-full', 're-armed', 'easing', 'rejected-edge'] as const;
export type FbsAction = (typeof FBS_ACTIONS)[number];

/** The hold that never releases. */
export const FBS_HOLD_NEVER = 'never' as const;

/** The CURVE_ORIGINS wire words, by index. */
export const ORIGIN_WORDS: readonly string[] = CURVE_ORIGINS.map((id) => id.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`));
/** The CURVE_SHAPES words, by index (the unshaped band is `-`). */
export const SHAPE_WORDS: readonly string[] = CURVE_SHAPES.map((id) => (id === 'unshaped' ? '-' : id));

const origin = (word: string): number => ORIGIN_WORDS.indexOf(word);
const shape = (word: string): number => SHAPE_WORDS.indexOf(word);
export const ORIGIN = { user: origin('user'), fbsFixed: origin('fbs-fixed'), fbsLive: origin('fbs-live') } as const;
export const SHAPE = { off: shape('off'), bell: shape('bell'), notch: shape('notch') } as const;

/** Slots one placement holds: the band cap's ceiling. */
export const FBS_PLACE_SLOTS = FBS_BANDS_RANGE.max;

/** One report slot's six words, as omx_band_report.h lays them out. */
export interface BandReport {
  readonly freqHz: number;
  readonly depthDb: number;
  readonly q: number;
  readonly on: number;
  readonly origin: number;
  readonly shape: number;
}

/** One confirmed ring: the detector's frequency and depth. */
export interface FbsFind {
  readonly freqHz: number;
  readonly depthDb: number;
}

/** One action, with the slot it concerned (-1 for a refusal). */
export interface FbsEvent {
  readonly action: FbsAction;
  readonly freqHz: number;
  readonly slot: number;
}

interface Slot {
  freqHz: number;
  gainDb: number;
  q: number;
  on: boolean;
  origin: number;
  shape: number;
  seq: number;
  live: boolean;
  plantedDb: number;
  lastRingMs: number;
  stage: number;
  dwellStartMs: number | null;
  reclamps: number;
  ramp: { step: number; stage: number; fromDb: number; toDb: number; dueMs: number; seq: number } | null;
}

const freeSlot = (): Slot => ({
  freqHz: 0, gainDb: 0, q: 0, on: false, origin: ORIGIN.user, shape: SHAPE.off, seq: 0,
  live: false, plantedDb: 0, lastRingMs: 0, stage: 0, dwellStartMs: null, reclamps: 0, ramp: null,
});

/** `n < lo ? lo : n > hi ? hi : n`: core's clamp, a NaN passing through. */
const clamp = (n: number, lo: number, hi: number): number => (n < lo ? lo : n > hi ? hi : n);

/** The gain a band of `origin` may hold: fbs-* in [FBS_MAX_CUT_DB, 0] (NaN reads 0), every other origin in
 * EQ_GAIN_RANGE (NaN stays NaN) — the strip store's arm. */
export function fbsGainCap(originIndex: number, db: number): number {
  if (originIndex === ORIGIN.fbsFixed || originIndex === ORIGIN.fbsLive) {
    if (Number.isNaN(db)) return 0;
    return Math.min(0, Math.max(FBS_MAX_CUT_DB, db));
  }
  return clamp(db, EQ_GAIN_RANGE.min, EQ_GAIN_RANGE.max);
}

/** Two readings are the same ringing frequency: within FBS_SAME_RING_HZ or FBS_SAME_RING_SHARE of the higher. */
export function sameRingFreq(aHz: number, bHz: number): boolean {
  return Math.abs(aHz - bHz) <= Math.max(FBS_SAME_RING_HZ, FBS_SAME_RING_SHARE * Math.max(aHz, bHz));
}

/** omx_hrp_cents_between in float: `fabsf(1200 * log2f(a / b))`, 1e9 when either is not positive. */
export function fbsCents(aHz: number, bHz: number): number {
  const a = Math.fround(aHz);
  const b = Math.fround(bHz);
  if (!(a > 0) || !(b > 0)) return Math.fround(1e9);
  return Math.abs(Math.fround(1200 * Math.fround(Math.log2(Math.fround(a / b)))));
}

/** Math.round's half-up rounding. */
const round = (x: number): number => Math.round(x);

/** One corrector's placement: the console's FbsCorrector rules over fixed slots. */
export class FbsPlace {
  pool: FbsPool;
  holdS: number | typeof FBS_HOLD_NEVER = FBS_DEFAULT_HOLD_SECONDS;
  maxBands: number = FBS_DEFAULT_MAX_AUTO_BANDS;
  notchQ: number = FBS_DEFAULT_NOTCH_Q;
  mergeCents: number = FBS_DEFAULT_MERGE_CENTS;
  cooldownS: number = FBS_DEFAULT_COOLDOWN_SECONDS;
  nowMs = 0;
  memoryDropped = 0;
  protected slots: Slot[] = Array.from({ length: FBS_PLACE_SLOTS }, freeSlot);
  protected memory: Array<{ freqHz: number; reclamps: number; releasedMs: number }> = [];
  protected seq = 0;
  protected timerSeq = 0;
  readonly log: FbsEvent[] = [];

  constructor(pool: FbsPool) {
    this.pool = pool;
  }

  /** The arming values, each clamped as the console clamps it. */
  set(holdS: number | typeof FBS_HOLD_NEVER, maxBands: number, notchQ: number, mergeCents: number, cooldownS: number): void {
    this.holdS = holdS === FBS_HOLD_NEVER ? FBS_HOLD_NEVER : Number.isFinite(holdS) ? clamp(holdS, FBS_HOLD_RANGE.min, FBS_HOLD_RANGE.max) : FBS_DEFAULT_HOLD_SECONDS;
    this.maxBands = Number.isFinite(maxBands) ? clamp(round(maxBands), FBS_BANDS_RANGE.min, FBS_BANDS_RANGE.max) : FBS_DEFAULT_MAX_AUTO_BANDS;
    this.notchQ = Number.isFinite(notchQ) ? clamp(notchQ, EQ_NOTCH_Q_RANGE.min, EQ_NOTCH_Q_RANGE.max) : FBS_DEFAULT_NOTCH_Q;
    this.mergeCents = Number.isFinite(mergeCents) ? clamp(round(mergeCents), FBS_MERGE_RANGE.min, FBS_MERGE_RANGE.max) : FBS_DEFAULT_MERGE_CENTS;
    this.cooldownS = Number.isFinite(cooldownS) ? clamp(round(cooldownS), FBS_COOLDOWN_RANGE.min, FBS_COOLDOWN_RANGE.max) : FBS_DEFAULT_COOLDOWN_SECONDS;
  }

  /** The bands planted, fixed and live together. */
  count(): number {
    return this.slots.filter((s) => s.origin !== ORIGIN.user).length;
  }

  /** Slot k's six report words. */
  report(k: number): BandReport {
    const s = this.slots[k];
    if (!s || s.origin === ORIGIN.user) return { freqHz: 0, depthDb: 0, q: 0, on: 0, origin: ORIGIN.user, shape: SHAPE.off };
    return { freqHz: s.freqHz, depthDb: s.gainDb, q: s.q, on: s.on ? 1 : 0, origin: s.origin, shape: s.shape };
  }

  /** The ramp steps due by `nowMs`, earliest first, ties in the order the ramps began. */
  advance(nowMs: number): void {
    for (;;) {
      let next = -1;
      for (let k = 0; k < this.slots.length; k++) {
        const r = this.slots[k]!.ramp;
        if (!r || r.dueMs > nowMs) continue;
        const best = next < 0 ? null : this.slots[next]!.ramp!;
        if (!best || r.dueMs < best.dueMs || (r.dueMs === best.dueMs && r.seq < best.seq)) next = k;
      }
      if (next < 0) break;
      this.nowMs = this.slots[next]!.ramp!.dueMs;
      this.rampStep(next);
    }
    if (nowMs > this.nowMs) this.nowMs = nowMs;
  }

  /** One detector tick: the ramp steps due, a band per newly confirmed ring, then the clean holds. */
  tick(nowMs: number, snapshotHz: readonly number[], confirmed: readonly FbsFind[]): void {
    this.advance(nowMs);
    for (const c of confirmed) this.correct(c);
    this.holdSweep(snapshotHz);
  }

  // ---- the rules, one method each ----------------------------------------------------------------------------

  /** One find or another: the same-ring window, or the merge radius in cents (band first, find second). */
  protected sameRegion(bandHz: number, findHz: number): boolean {
    return sameRingFreq(bandHz, findHz) || fbsCents(bandHz, findHz) <= this.mergeCents;
  }

  /** The pool a find goes to. */
  protected poolOfFind(): FbsPool {
    return this.pool;
  }

  /** The gain cap the placement writes through. */
  protected capGain(originIndex: number, db: number): number {
    return fbsGainCap(originIndex, db);
  }

  /** The stage a release eases to after `stage`. */
  protected nextStage(stage: number): number {
    return stage + 1;
  }

  /** Free slot k: it reports free and no other slot moves. */
  protected release(k: number): void {
    this.slots[k] = freeSlot();
  }

  protected emit(action: FbsAction, freqHz: number, slot: number): void {
    this.log.push({ action, freqHz, slot });
  }

  /** Occupied slots in plant order: the console's band order. */
  protected bySeq(): number[] {
    return this.slots.map((s, k) => ({ s, k })).filter(({ s }) => s.origin !== ORIGIN.user).sort((a, b) => a.s.seq - b.s.seq).map(({ k }) => k);
  }

  protected correct(c: FbsFind): void {
    if (!(c.freqHz >= FBS_RING_HZ_MIN && c.freqHz <= FBS_RING_HZ_MAX)) {
      this.emit('rejected-edge', c.freqHz, -1);
      return;
    }
    const existing = this.bySeq().find((k) => this.sameRegion(this.slots[k]!.freqHz, c.freqHz));
    if (existing !== undefined) {
      const s = this.slots[existing]!;
      if (s.live) {
        s.lastRingMs = this.nowMs;
        if (s.ramp !== null || s.stage > 0) this.reArm(existing);
      }
      return;
    }
    if (this.count() >= this.maxBands) {
      this.emit('pool-full', c.freqHz, -1);
      return;
    }
    const k = this.slots.findIndex((s) => s.origin === ORIGIN.user);
    if (k < 0) return;
    const fixed = this.poolOfFind() === 'fixed';
    const o = fixed ? ORIGIN.fbsFixed : ORIGIN.fbsLive;
    const s = this.slots[k]!;
    s.freqHz = clamp(c.freqHz, EQ_FREQ_RANGE.min, EQ_FREQ_RANGE.max);
    s.shape = fixed ? SHAPE.notch : SHAPE.bell;
    s.gainDb = this.capGain(o, fixed ? 0 : c.depthDb);
    s.q = fixed ? clamp(this.notchQ, EQ_NOTCH_Q_RANGE.min, EQ_NOTCH_Q_RANGE.max) : clamp(this.notchQ, EQ_Q_RANGE.min, EQ_Q_RANGE.max);
    s.on = true;
    s.origin = o;
    s.seq = ++this.seq;
    s.live = !fixed;
    s.plantedDb = s.gainDb;
    s.lastRingMs = this.nowMs;
    s.stage = 0;
    s.dwellStartMs = null;
    s.ramp = null;
    s.reclamps = fixed ? 0 : this.takeMemory(s.freqHz);
    this.emit('notched', c.freqHz, k);
  }

  protected holdSweep(snapshotHz: readonly number[]): void {
    for (const k of this.bySeq()) {
      const s = this.slots[k]!;
      if (!s.live) continue;
      if (snapshotHz.some((hz) => sameRingFreq(hz, s.freqHz))) {
        s.lastRingMs = this.nowMs;
        if (s.ramp !== null || s.stage > 0) this.reArm(k);
        continue;
      }
      if (s.ramp !== null) continue;
      if (this.holdS === FBS_HOLD_NEVER) continue;
      if (s.stage === 0) {
        const holdMs = this.holdS * 1000 * 2 ** Math.min(s.reclamps, FBS_RECLAMP_BACKOFF_CAP);
        if (this.nowMs - s.lastRingMs >= holdMs) this.beginStage(k, this.nextStage(0));
      } else if (s.dwellStartMs !== null && this.nowMs - s.dwellStartMs >= FBS_RELEASE_DWELL_MS) {
        this.beginStage(k, this.nextStage(s.stage));
      }
    }
  }

  protected reArm(k: number): void {
    const s = this.slots[k]!;
    s.ramp = null;
    s.stage = 0;
    s.dwellStartMs = null;
    if (s.gainDb === s.plantedDb) return;
    s.reclamps += 1;
    s.plantedDb = Math.max(FBS_MAX_CUT_DB, s.plantedDb - FBS_RECLAMP_DEEPEN_DB);
    s.gainDb = this.capGain(s.origin, s.plantedDb);
    this.emit('re-armed', s.freqHz, k);
  }

  protected beginStage(k: number, stage: number): void {
    const s = this.slots[k]!;
    s.dwellStartMs = null;
    s.ramp = { step: 0, stage, fromDb: s.gainDb, toDb: s.plantedDb * FBS_RELEASE_RATIO ** stage, dueMs: this.nowMs + FBS_LIFT_RAMP_MS / FBS_LIFT_RAMP_STEPS, seq: ++this.timerSeq };
  }

  protected rampStep(k: number): void {
    const s = this.slots[k]!;
    const r = s.ramp!;
    r.step++;
    if (r.step < FBS_LIFT_RAMP_STEPS) {
      s.gainDb = this.capGain(s.origin, r.fromDb + (r.toDb - r.fromDb) * (r.step / FBS_LIFT_RAMP_STEPS));
      r.dueMs += FBS_LIFT_RAMP_MS / FBS_LIFT_RAMP_STEPS;
      return;
    }
    s.ramp = null;
    if (Math.abs(r.toDb) < FBS_RELEASE_DONE_DB) {
      if (s.reclamps > 0) this.remember(s.freqHz, s.reclamps);
      const hz = s.freqHz;
      this.release(k);
      this.emit('lifted', hz, k);
      return;
    }
    s.gainDb = this.capGain(s.origin, r.toDb);
    s.stage = r.stage;
    s.dwellStartMs = this.nowMs;
    this.emit('easing', s.freqHz, k);
  }

  protected remember(freqHz: number, reclamps: number): void {
    if (this.memory.length >= FBS_RING_MEMORY_SLOTS) {
      this.memory.shift();
      this.memoryDropped++;
    }
    this.memory.push({ freqHz, reclamps, releasedMs: this.nowMs });
  }

  protected takeMemory(freqHz: number): number {
    if (this.cooldownS <= 0) return 0;
    const cutoff = this.nowMs - this.cooldownS * 1000;
    for (let i = this.memory.length - 1; i >= 0; i--) {
      const m = this.memory[i]!;
      if (m.releasedMs < cutoff) {
        this.memory.splice(i, 1);
        continue;
      }
      if (this.sameRegion(m.freqHz, freqHz)) {
        this.memory.splice(i, 1);
        return m.reclamps;
      }
    }
    return 0;
  }
}
