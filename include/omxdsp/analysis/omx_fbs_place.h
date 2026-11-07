/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_fbs_place.h — the feedback corrector's PLACEMENT in C: what a confirmed ring becomes.
 *
 * Spec 7.7: omx-fbs carries both pools with the staged fade, and its placement is C, held equal to
 * the console's by a twin test. This is the port of the console's feedback corrector rules (with its
 * clamps and the strip store's per-origin gain bound), without the store writes and the rows: a fixed
 * set of slots instead of an EQ list.
 * ts/src/fbs-place.ts is its TypeScript twin, and test/analysis/fixtures/fbs_place.vectors is the
 * one vector file the C test and the twin test both read.
 *
 * The rules, each the console's:
 *  - THE EDGE FIRST: a ring outside [FBS_RING_HZ_MIN, FBS_RING_HZ_MAX] is refused (rejected-edge),
 *    never clamped onto the EQ's edge.
 *  - ONE FIND, ONE BAND: a ring within the same-ring window or the merge radius (cents) of a band
 *    already planted is that band's find. It plants nothing; a live band's clean hold restarts, and a
 *    releasing one snaps back (re-armed).
 *  - THE POOL: a full pool (max_bands) plants nothing and says pool-full.
 *  - TWO POOLS: fixed (ring-out) plants a true notch at the notch Q that never lifts; live plants a
 *    bell at the find's depth, bounded by the bell's Q extent, which the staged release lifts.
 *  - THE STAGED RELEASE (live only): clean for the hold (doubled per past re-clamp, capped), the depth
 *    eases one stage, planted x FBS_RELEASE_RATIO^stage, over a FBS_LIFT_RAMP_STEPS-step ramp, then
 *    listens FBS_RELEASE_DWELL_MS before the next; a stage shallower than FBS_RELEASE_DONE_DB removes
 *    the band (lifted). Regrowth at any point re-clamps to full depth, DEEPENED by
 *    FBS_RECLAMP_DEEPEN_DB down to FBS_MAX_CUT_DB, and doubles the next hold.
 *  - RING MEMORY: a fully released find's re-clamp count is remembered for the cooldown, so a chronic
 *    ringer re-plants with its doubled holds.
 *  - THE GAIN CAP BY ORIGIN: an fbs-* band holds [FBS_MAX_CUT_DB, 0]; every other origin the musical
 *    EQ_GAIN_RANGE, as the strip's store does.
 *
 * SLOTS KEEP THEIR FINDS (spec 7.16): a plant takes the lowest free slot and a release frees its own,
 * so no other slot moves. Where several bands match one ring, the OLDEST plant answers, as the
 * console's band list (insertion order) does.
 *
 * TIME is the caller's, in ms. The console's ramp runs on interval timers between detector ticks;
 * here a ramp step is due at a time and fires when the clock reaches it ({@link omx_fbs_place_advance},
 * which every tick runs first): steps due at or before a tick fire before it, earliest first, ties in
 * the order the ramps began.
 *
 * Worker thread only: no allocation, no lock, no I/O, bounded loops over OMX_FBS_PLACE_SLOTS.
 */
#ifndef OMX_FBS_PLACE_H
#define OMX_FBS_PLACE_H

#include <stdint.h>

#include <omxdsp/analysis/omx_band_report.h>
#include <omxdsp/analysis/omx_fbs_detect.h>
#include <omxdsp/omxdsp_definitions.h>

/** Slots one placement holds: the band cap's ceiling, the EQ's band count. */
#define OMX_FBS_PLACE_SLOTS OMX_FBS_BANDS_RANGE_MAX

/** The hold that never releases (the console's `'never'`). */
#define OMX_FBS_HOLD_NEVER (-1.0)

/** Events one placement keeps until its caller clears them; a full log counts what it drops. */
#define OMX_FBS_PLACE_LOG 64u

/** What the placement did — the console's FbsAction, word for word. */
typedef enum {
  OMX_FBS_NOTCHED = 0,
  OMX_FBS_LIFTED = 1,
  OMX_FBS_POOL_FULL = 2,
  OMX_FBS_RE_ARMED = 3,
  OMX_FBS_EASING = 4,
  OMX_FBS_REJECTED_EDGE = 5,
  OMX_FBS_ACTIONS_COUNT = 6
} OmxFbsAction;

/** The wire words of OmxFbsAction, by value. */
static const char *const OMX_FBS_ACTION_WORDS[OMX_FBS_ACTIONS_COUNT] = {"notched",  "lifted", "pool-full",
                                                                         "re-armed", "easing", "rejected-edge"};

/** One slot: the band it holds (the report) and, for a live band, its release state. */
typedef struct {
  double freq_hz;
  double gain_db;
  double q;
  int on;
  int origin;   /**< enum omx_curve_origins; OMX_CURVE_ORIGINS_USER is a free slot */
  int shape;    /**< enum omx_curve_shapes */
  uint64_t seq; /**< plant order: the console's band order */
  /* the live band's release (the console's LiveBand) */
  int live;
  double planted_db;
  double last_ring_ms;
  uint32_t stage;
  int dwelling;
  double dwell_start_ms;
  uint32_t reclamps;
  /* the stage ramp in flight (the console's interval timer) */
  int ramping;
  uint32_t ramp_step;
  uint32_t ramp_stage;
  double ramp_from_db;
  double ramp_to_db;
  double ramp_due_ms;
  uint64_t ramp_seq;
} OmxFbsPlaceSlot;

/** A fully released find's escalation, kept for the cooldown. */
typedef struct {
  double freq_hz;
  uint32_t reclamps;
  double released_ms;
} OmxFbsMemory;

/** One action, as the console's onAction hook reports it, with the slot it concerned. */
typedef struct {
  int action; /**< OmxFbsAction */
  double freq_hz;
  int slot; /**< the slot, or -1 for a refusal that planted nothing */
} OmxFbsPlaceEvent;

/** One corrector's placement. Plain data: a copy is a copy. */
typedef struct {
  OmxFbsPool pool;
  double hold_s; /**< seconds, or OMX_FBS_HOLD_NEVER */
  uint32_t max_bands;
  double notch_q;
  double merge_cents;
  double cooldown_s;
  OmxFbsPlaceSlot slot[OMX_FBS_PLACE_SLOTS];
  OmxFbsMemory memory[OMX_FBS_RING_MEMORY_SLOTS];
  uint32_t n_memory;
  uint32_t memory_dropped;
  uint64_t seq;
  uint64_t timer_seq;
  double now_ms;
  OmxFbsPlaceEvent log[OMX_FBS_PLACE_LOG];
  uint32_t n_log;
  uint32_t log_dropped;
} OmxFbsPlace;

/**
 * @brief An empty placement on `pool`, at the declared defaults.
 * @param p The placement; NULL is a no-op.
 * @param pool The pool its finds go to.
 * @thread worker
 */
void omx_fbs_place_init(OmxFbsPlace *p, OmxFbsPool pool);

/**
 * @brief The arming values, each clamped as the console clamps it.
 * @param p The placement; NULL is a no-op.
 * @param hold_s The clean hold in seconds, held in the declared travel and not rounded;
 *        OMX_FBS_HOLD_NEVER never releases; a non-finite one reads as the default.
 * @param max_bands The pool, rounded half up and held in the declared travel.
 * @param notch_q The planted width, held in the notch shape's Q extent.
 * @param merge_cents The merge radius, rounded half up and held in its travel.
 * @param cooldown_s The ring memory, rounded half up and held in its travel.
 * @note Bands already planted keep their values; a non-finite value reads as its default.
 * @thread worker
 */
void omx_fbs_place_set(OmxFbsPlace *p, double hold_s, double max_bands, double notch_q, double merge_cents,
                       double cooldown_s);

/**
 * @brief Point the next finds at a pool (the console's mode, ring-out or live).
 * @param p The placement; NULL is a no-op.
 * @param pool The pool.
 * @thread worker
 */
void omx_fbs_place_set_pool(OmxFbsPlace *p, OmxFbsPool pool);

/**
 * @brief The gain a band of `origin` may hold: fbs-* within [FBS_MAX_CUT_DB, 0] (a NaN reads 0), every
 *        other origin within EQ_GAIN_RANGE (a NaN stays NaN), as the strip's store clamps.
 * @param origin An enum omx_curve_origins.
 * @param db The gain asked for, dB.
 * @return The gain held.
 * @thread worker
 */
double omx_fbs_place_gain_cap(int origin, double db);

/**
 * @brief Two readings are the same ringing frequency: within FBS_SAME_RING_HZ or FBS_SAME_RING_SHARE
 *        of the higher, whichever is wider.
 * @param a_hz One reading.
 * @param b_hz The other.
 * @return 1 or 0.
 * @thread worker
 */
int omx_fbs_place_same_ring(double a_hz, double b_hz);

/**
 * @brief The musical distance the merge radius is judged in: omx_hrp_cents_between() in float, as the
 *        console's DSP door computes it.
 * @param a_hz One frequency.
 * @param b_hz The other.
 * @return Cents; 1e9 when either is not positive.
 * @thread worker
 */
double omx_fbs_place_cents(double a_hz, double b_hz);

/**
 * @brief Run the ramp steps due by `now_ms`, earliest first.
 * @param p The placement; NULL is a no-op.
 * @param now_ms The clock, ms. A clock that runs backward fires nothing.
 * @thread worker
 */
void omx_fbs_place_advance(OmxFbsPlace *p, double now_ms);

/**
 * @brief One detector tick: the ramp steps due, then a band per newly confirmed ring, then the clean
 *        holds against the rings still sounding.
 * @param p The placement; NULL is a no-op.
 * @param now_ms The tick's clock, ms.
 * @param snapshot_hz The frequencies still ringing (the detector's snapshot); may be NULL when empty.
 * @param n_snapshot Their count.
 * @param confirmed The rings newly confirmed on this tick, in the detector's order; may be NULL.
 * @param n_confirmed Their count.
 * @thread worker
 */
void omx_fbs_place_tick(OmxFbsPlace *p, double now_ms, const double *snapshot_hz, uint32_t n_snapshot,
                        const OmxFbsCandidate *confirmed, uint32_t n_confirmed);

/**
 * @brief The bands planted (the pool in use), fixed and live together.
 * @param p The placement.
 * @return Occupied slots.
 * @thread worker
 */
uint32_t omx_fbs_place_count(const OmxFbsPlace *p);

/**
 * @brief Slot k's six report words, by value.
 * @param p The placement.
 * @param k The slot; one past the slots reads as a free slot.
 * @return The report.
 * @thread worker
 */
OmxBandReport omx_fbs_place_report(const OmxFbsPlace *p, uint32_t k);

#endif /* OMX_FBS_PLACE_H */
