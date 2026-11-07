/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_fbs_place.c — the feedback corrector's placement (omx_fbs_place.h): the console's FbsCorrector rules
 * over fixed slots. Each rule below names the console's method it ports; ts/src/fbs-place.ts is the twin and test/analysis/fixtures/fbs_place.vectors holds both.
 */
#include <omxdsp/analysis/omx_fbs_place.h>

#include <math.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_track.h>

/* The console's clamp: a NaN passes through. */
static double clamp(double n, double lo, double hi) { return n < lo ? lo : (n > hi ? hi : n); }

/* Math.round: half up, exactly (x - floor(x) is exact in double). */
static double round_half_up(double x) {
  const double r = floor(x);
  return (x - r >= 0.5) ? r + 1.0 : r;
}

static int is_fbs(int origin) { return origin == OMX_CURVE_ORIGINS_FBS_FIXED || origin == OMX_CURVE_ORIGINS_FBS_LIVE; }

static void free_slot(OmxFbsPlaceSlot *s) {
  memset(s, 0, sizeof(*s));
  s->origin = OMX_CURVE_ORIGINS_USER;
  s->shape = OMX_CURVE_SHAPES_OFF;
}

static void emit(OmxFbsPlace *p, int action, double freq_hz, int slot) {
  if (p->n_log >= OMX_FBS_PLACE_LOG) {
    p->log_dropped++;
    return;
  }
  p->log[p->n_log].action = action;
  p->log[p->n_log].freq_hz = freq_hz;
  p->log[p->n_log].slot = slot;
  p->n_log++;
}

void omx_fbs_place_init(OmxFbsPlace *p, OmxFbsPool pool) {
  if (!p) return;
  memset(p, 0, sizeof(*p));
  p->pool = pool;
  p->hold_s = OMX_FBS_DEFAULT_HOLD_SECONDS;
  p->max_bands = OMX_FBS_DEFAULT_MAX_AUTO_BANDS;
  p->notch_q = OMX_FBS_DEFAULT_NOTCH_Q;
  p->merge_cents = OMX_FBS_DEFAULT_MERGE_CENTS;
  p->cooldown_s = OMX_FBS_DEFAULT_COOLDOWN_SECONDS;
  for (uint32_t k = 0; k < OMX_FBS_PLACE_SLOTS; k++) free_slot(&p->slot[k]);
}

/* FbsCorrector.set, through core's clampFbsHold, clampFbsMaxAutoBands, clampFbsNotchQ, clampFbsMergeCents and
 * clampFbsCooldownSeconds. */
void omx_fbs_place_set(OmxFbsPlace *p, double hold_s, double max_bands, double notch_q, double merge_cents,
                       double cooldown_s) {
  if (!p) return;
  if (hold_s == OMX_FBS_HOLD_NEVER)
    p->hold_s = OMX_FBS_HOLD_NEVER;
  else
    p->hold_s = isfinite(hold_s) ? clamp(hold_s, OMX_FBS_HOLD_RANGE_MIN, OMX_FBS_HOLD_RANGE_MAX)
                                 : (double)OMX_FBS_DEFAULT_HOLD_SECONDS;
  p->max_bands = isfinite(max_bands)
                     ? (uint32_t)clamp(round_half_up(max_bands), OMX_FBS_MIN_AUTO_BANDS, OMX_FBS_BANDS_RANGE_MAX)
                     : (uint32_t)OMX_FBS_DEFAULT_MAX_AUTO_BANDS;
  p->notch_q = isfinite(notch_q) ? clamp(notch_q, OMX_EQ_NOTCH_Q_RANGE_MIN_DOUBLE, OMX_EQ_NOTCH_Q_RANGE_MAX)
                                 : (double)OMX_FBS_DEFAULT_NOTCH_Q;
  p->merge_cents = isfinite(merge_cents)
                       ? clamp(round_half_up(merge_cents), OMX_FBS_MERGE_RANGE_MIN, OMX_FBS_MERGE_RANGE_MAX)
                       : (double)OMX_FBS_DEFAULT_MERGE_CENTS;
  p->cooldown_s = isfinite(cooldown_s)
                      ? clamp(round_half_up(cooldown_s), OMX_FBS_COOLDOWN_RANGE_MIN, OMX_FBS_COOLDOWN_RANGE_MAX)
                      : (double)OMX_FBS_DEFAULT_COOLDOWN_SECONDS;
}

void omx_fbs_place_set_pool(OmxFbsPlace *p, OmxFbsPool pool) {
  if (p) p->pool = pool;
}

/* core's clampFbsCut for an fbs-* band, clampEqGain for every other origin: the strip store's arm. */
double omx_fbs_place_gain_cap(int origin, double db) {
  if (is_fbs(origin)) {
    if (isnan(db)) return 0.0;
    if (db < (double)OMX_FBS_MAX_CUT_DB) db = (double)OMX_FBS_MAX_CUT_DB;
    if (db > 0.0) db = 0.0;
    return db;
  }
  return clamp(db, OMX_EQ_GAIN_RANGE_MIN, OMX_EQ_GAIN_RANGE_MAX);
}

/* sameRingFreq */
int omx_fbs_place_same_ring(double a_hz, double b_hz) {
  const double hi = a_hz > b_hz ? a_hz : b_hz;
  const double share = OMX_FBS_SAME_RING_SHARE_DOUBLE * hi;
  const double window = share > (double)OMX_FBS_SAME_RING_HZ ? share : (double)OMX_FBS_SAME_RING_HZ;
  return fabs(a_hz - b_hz) <= window;
}

/* centsApart: the DSP door's omx_hrp_cents_between, in float */
double omx_fbs_place_cents(double a_hz, double b_hz) { return (double)omx_hrp_cents_between((float)a_hz, (float)b_hz); }

/* sameRegion: the same-ring window, or the merge radius (band first, find second) */
static int same_region(const OmxFbsPlace *p, double band_hz, double find_hz) {
  return omx_fbs_place_same_ring(band_hz, find_hz) || omx_fbs_place_cents(band_hz, find_hz) <= p->merge_cents;
}

uint32_t omx_fbs_place_count(const OmxFbsPlace *p) {
  if (!p) return 0;
  uint32_t n = 0;
  for (uint32_t k = 0; k < OMX_FBS_PLACE_SLOTS; k++)
    if (p->slot[k].origin != OMX_CURVE_ORIGINS_USER) n++;
  return n;
}

OmxBandReport omx_fbs_place_report(const OmxFbsPlace *p, uint32_t k) {
  if (!p || k >= OMX_FBS_PLACE_SLOTS || p->slot[k].origin == OMX_CURVE_ORIGINS_USER) return omx_band_report_free();
  const OmxFbsPlaceSlot *s = &p->slot[k];
  OmxBandReport r;
  r.freq_hz = (float)s->freq_hz;
  r.depth_db = (float)s->gain_db;
  r.q = (float)s->q;
  r.on = s->on ? 1.0f : 0.0f;
  r.origin = (float)s->origin;
  r.shape = (float)s->shape;
  return r;
}

/* The occupied slot after `after_seq` in plant order (the console's band order), or -1. */
static int next_by_seq(const OmxFbsPlace *p, uint64_t after_seq) {
  int best = -1;
  for (uint32_t k = 0; k < OMX_FBS_PLACE_SLOTS; k++) {
    const OmxFbsPlaceSlot *s = &p->slot[k];
    if (s->origin == OMX_CURVE_ORIGINS_USER || s->seq <= after_seq) continue;
    if (best < 0 || s->seq < p->slot[best].seq) best = (int)k;
  }
  return best;
}

/* remember: a full memory forgets its oldest entry, and counts it */
static void remember(OmxFbsPlace *p, double freq_hz, uint32_t reclamps) {
  if (p->n_memory >= OMX_FBS_RING_MEMORY_SLOTS) {
    memmove(&p->memory[0], &p->memory[1], (OMX_FBS_RING_MEMORY_SLOTS - 1u) * sizeof(p->memory[0]));
    p->n_memory--;
    p->memory_dropped++;
  }
  p->memory[p->n_memory].freq_hz = freq_hz;
  p->memory[p->n_memory].reclamps = reclamps;
  p->memory[p->n_memory].released_ms = p->now_ms;
  p->n_memory++;
}

static void forget(OmxFbsPlace *p, uint32_t i) {
  memmove(&p->memory[i], &p->memory[i + 1], (p->n_memory - i - 1u) * sizeof(p->memory[0]));
  p->n_memory--;
}

/* takeMemory: the newest un-expired entry for this region, consumed on read */
static uint32_t take_memory(OmxFbsPlace *p, double freq_hz) {
  if (p->cooldown_s <= 0.0) return 0;
  const double cutoff = p->now_ms - p->cooldown_s * 1000.0;
  for (uint32_t i = p->n_memory; i-- > 0;) {
    if (p->memory[i].released_ms < cutoff) {
      forget(p, i);
      continue;
    }
    if (same_region(p, p->memory[i].freq_hz, freq_hz)) {
      const uint32_t r = p->memory[i].reclamps;
      forget(p, i);
      return r;
    }
  }
  return 0;
}

/* reArm: abort the release, snap back to full depth; regrowth after a move deepens the cut and doubles the hold */
static void re_arm(OmxFbsPlace *p, uint32_t k) {
  OmxFbsPlaceSlot *s = &p->slot[k];
  s->ramping = 0;
  s->stage = 0;
  s->dwelling = 0;
  if (s->gain_db == s->planted_db) return;
  s->reclamps += 1u;
  const double deeper = s->planted_db - (double)OMX_FBS_RECLAMP_DEEPEN_DB;
  s->planted_db = deeper > (double)OMX_FBS_MAX_CUT_DB ? deeper : (double)OMX_FBS_MAX_CUT_DB;
  s->gain_db = omx_fbs_place_gain_cap(s->origin, s->planted_db);
  emit(p, OMX_FBS_RE_ARMED, s->freq_hz, (int)k);
}

/* beginStage: ease toward planted x ratio^stage over the ramp's steps */
static void begin_stage(OmxFbsPlace *p, uint32_t k, uint32_t stage) {
  OmxFbsPlaceSlot *s = &p->slot[k];
  s->dwelling = 0;
  s->ramping = 1;
  s->ramp_step = 0;
  s->ramp_stage = stage;
  s->ramp_from_db = s->gain_db;
  s->ramp_to_db = s->planted_db * pow(OMX_FBS_RELEASE_RATIO_DOUBLE, (double)stage);
  s->ramp_due_ms = p->now_ms + (double)OMX_FBS_LIFT_RAMP_MS / (double)OMX_FBS_LIFT_RAMP_STEPS;
  s->ramp_seq = ++p->timer_seq;
}

/* the interval's callback: one step, the last lands the stage or removes the band */
static void step_ramp(OmxFbsPlace *p, uint32_t k) {
  OmxFbsPlaceSlot *s = &p->slot[k];
  s->ramp_step++;
  if (s->ramp_step < OMX_FBS_LIFT_RAMP_STEPS) {
    s->gain_db = omx_fbs_place_gain_cap(s->origin, s->ramp_from_db + (s->ramp_to_db - s->ramp_from_db) *
                                                                         ((double)s->ramp_step / OMX_FBS_LIFT_RAMP_STEPS));
    s->ramp_due_ms += (double)OMX_FBS_LIFT_RAMP_MS / (double)OMX_FBS_LIFT_RAMP_STEPS;
    return;
  }
  s->ramping = 0;
  if (fabs(s->ramp_to_db) < (double)OMX_FBS_RELEASE_DONE_DB) {
    const double hz = s->freq_hz;
    if (s->reclamps > 0u) remember(p, hz, s->reclamps);
    free_slot(s);
    emit(p, OMX_FBS_LIFTED, hz, (int)k);
    return;
  }
  s->gain_db = omx_fbs_place_gain_cap(s->origin, s->ramp_to_db);
  s->stage = s->ramp_stage;
  s->dwelling = 1;
  s->dwell_start_ms = p->now_ms;
  emit(p, OMX_FBS_EASING, s->freq_hz, (int)k);
}

void omx_fbs_place_advance(OmxFbsPlace *p, double now_ms) {
  if (!p) return;
  for (;;) {
    int next = -1;
    for (uint32_t k = 0; k < OMX_FBS_PLACE_SLOTS; k++) {
      const OmxFbsPlaceSlot *s = &p->slot[k];
      if (!s->ramping || !(s->ramp_due_ms <= now_ms)) continue;
      if (next < 0 || s->ramp_due_ms < p->slot[next].ramp_due_ms ||
          (s->ramp_due_ms == p->slot[next].ramp_due_ms && s->ramp_seq < p->slot[next].ramp_seq))
        next = (int)k;
    }
    if (next < 0) break;
    p->now_ms = p->slot[next].ramp_due_ms;
    step_ramp(p, (uint32_t)next);
  }
  if (now_ms > p->now_ms) p->now_ms = now_ms;
}

/* correct: the edge, one find one band, the pool, then the pool's shape */
static void correct(OmxFbsPlace *p, const OmxFbsCandidate *c) {
  if (!(c->freq_hz >= (double)OMX_FBS_RING_HZ_MIN && c->freq_hz <= (double)OMX_FBS_RING_HZ_MAX)) {
    emit(p, OMX_FBS_REJECTED_EDGE, c->freq_hz, -1);
    return;
  }
  for (int k = next_by_seq(p, 0); k >= 0; k = next_by_seq(p, p->slot[k].seq)) {
    OmxFbsPlaceSlot *s = &p->slot[k];
    if (!same_region(p, s->freq_hz, c->freq_hz)) continue;
    if (s->live) {
      s->last_ring_ms = p->now_ms;
      if (s->ramping || s->stage > 0u) re_arm(p, (uint32_t)k);
    }
    return;
  }
  if (omx_fbs_place_count(p) >= p->max_bands) {
    emit(p, OMX_FBS_POOL_FULL, c->freq_hz, -1);
    return;
  }
  uint32_t k = 0;
  while (k < OMX_FBS_PLACE_SLOTS && p->slot[k].origin != OMX_CURVE_ORIGINS_USER) k++;
  if (k >= OMX_FBS_PLACE_SLOTS) return;
  const int fixed = p->pool == OMX_FBS_POOL_FIXED;
  OmxFbsPlaceSlot *s = &p->slot[k];
  free_slot(s);
  s->origin = fixed ? OMX_CURVE_ORIGINS_FBS_FIXED : OMX_CURVE_ORIGINS_FBS_LIVE;
  s->freq_hz = clamp(c->freq_hz, OMX_EQ_FREQ_RANGE_MIN, OMX_EQ_FREQ_RANGE_MAX);
  s->shape = fixed ? OMX_CURVE_SHAPES_NOTCH : OMX_CURVE_SHAPES_BELL;
  s->gain_db = omx_fbs_place_gain_cap(s->origin, fixed ? 0.0 : c->depth_db);
  s->q = fixed ? clamp(p->notch_q, OMX_EQ_NOTCH_Q_RANGE_MIN_DOUBLE, OMX_EQ_NOTCH_Q_RANGE_MAX)
               : clamp(p->notch_q, OMX_EQ_Q_RANGE_MIN_DOUBLE, OMX_EQ_Q_RANGE_MAX);
  s->on = 1;
  s->seq = ++p->seq;
  s->live = !fixed;
  s->planted_db = s->gain_db;
  s->last_ring_ms = p->now_ms;
  s->reclamps = fixed ? 0u : take_memory(p, s->freq_hz);
  emit(p, OMX_FBS_NOTCHED, c->freq_hz, (int)k);
}

/* holdSweep: a ring still sounding restarts the hold (and re-arms a release); clean past the hold, ease a stage */
static void hold_sweep(OmxFbsPlace *p, const double *snapshot_hz, uint32_t n_snapshot) {
  for (int k = next_by_seq(p, 0); k >= 0; k = next_by_seq(p, p->slot[k].seq)) {
    OmxFbsPlaceSlot *s = &p->slot[k];
    if (!s->live) continue;
    int ringing = 0;
    for (uint32_t i = 0; i < n_snapshot && !ringing; i++) ringing = omx_fbs_place_same_ring(snapshot_hz[i], s->freq_hz);
    if (ringing) {
      s->last_ring_ms = p->now_ms;
      if (s->ramping || s->stage > 0u) re_arm(p, (uint32_t)k);
      continue;
    }
    if (s->ramping) continue;
    if (p->hold_s == OMX_FBS_HOLD_NEVER) continue;
    if (s->stage == 0u) {
      const uint32_t doublings = s->reclamps < OMX_FBS_RECLAMP_BACKOFF_CAP ? s->reclamps : OMX_FBS_RECLAMP_BACKOFF_CAP;
      const double hold_ms = p->hold_s * 1000.0 * (double)(1u << doublings);
      if (p->now_ms - s->last_ring_ms >= hold_ms) begin_stage(p, (uint32_t)k, 1u);
    } else if (s->dwelling && p->now_ms - s->dwell_start_ms >= (double)OMX_FBS_RELEASE_DWELL_MS) {
      begin_stage(p, (uint32_t)k, s->stage + 1u);
    }
  }
}

void omx_fbs_place_tick(OmxFbsPlace *p, double now_ms, const double *snapshot_hz, uint32_t n_snapshot,
                        const OmxFbsCandidate *confirmed, uint32_t n_confirmed) {
  if (!p) return;
  omx_fbs_place_advance(p, now_ms);
  for (uint32_t i = 0; confirmed && i < n_confirmed; i++) correct(p, &confirmed[i]);
  hold_sweep(p, snapshot_hz ? snapshot_hz : (const double *)0, snapshot_hz ? n_snapshot : 0u);
}
