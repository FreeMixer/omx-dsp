/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_fbs_lv2.h — the feedback corrector AS A LIVE INSERT: omx-fbs's shell core, with no LV2 in it.
 *
 * The analogue of omx_hrp_lv2.h (spec 7.7 and 7.16). Plain C over the detector (omx_fbs_detect.h), the
 * placement (omx_fbs_place.h) and the RTA's ring and spectrum (omx_rta.h); the faces (an LV2 and a CLAP
 * plugin, in omx-plugins) add the ports and the host's worker and nothing else. No buffer is owned here:
 * {@link omx_fbs_lv2_init_block} partitions the caller's one block.
 *
 * THE ANALYSIS NEVER RUNS IN THE AUDIO CALLBACK. The split is the HRP shell's:
 *
 *     run()          (audio)  {@link omx_fbs_lv2_run}     the input into the ring; the planted bands on the
 *                                                         audio in `cut`, the audio untouched in `report`
 *     work()         (worker) {@link omx_fbs_lv2_answer}  one detector frame: the spectrum of the newest
 *                                                         window, the detector, the placement, the set
 *     work_response() (audio) {@link omx_fbs_lv2_adopt}   a fixed-size copy of the set
 *
 * One pass consumes one hop, the rate over FBS_WATCH_HZ (100 ms at every rate: the cadence the detector
 * gates were tuned at), counted by a written/consumed cursor pair, so WHICH audio a pass analyses never
 * depends on when the host runs its worker, and the placement's clock (the frames every pass consumed, in ms)
 * is the same whatever the host's block size.
 *
 * A HOST WITH NO WORKER never answers a pass: the plugin passes audio bit-exact through an empty cascade,
 * reports every slot free and `analysing` 0. That is the honest absence; analysing on the audio thread is
 * not a fallback.
 *
 * TWO MODES (spec 7.16). `cut` (the default: jalv and mod-host have no controller) runs the planted bands on
 * the audio. `report` keeps the audio bit-exact while the detector and the placement run on: a controller
 * sets `report`, reads it back, and plants the report into an omx-eq upstream of the detector, so the
 * detector hears the cuts it planted.
 *
 * THE REPORT: six read-only words per slot (omx_band_report.h) and `analysing`, read by value on the audio
 * thread after an adopt. Slot k is the placement's slot k: a merge or a release moves no other slot.
 *
 * MONO, one in and one out, as the console's corrector runs per channel.
 */
#ifndef OMX_FBS_LV2_H
#define OMX_FBS_LV2_H

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/analysis/omx_band_report.h>
#include <omxdsp/analysis/omx_fbs_detect.h>
#include <omxdsp/analysis/omx_fbs_place.h>
#include <omxdsp/analysis/omx_hrp_correct.h>
#include <omxdsp/analysis/omx_live_shell.h>
#include <omxdsp/analysis/omx_rta.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_port_int.h>

/** Audio channels in and out. */
#define OMX_FBS_LV2_CHANNELS 1

/** Frames of latency: none. The cascade has group delay and no delay line. */
#define OMX_FBS_LV2_LATENCY_FRAMES 0.0f

/** Report slots: the placement's. */
#define OMX_FBS_LV2_SLOTS OMX_FBS_PLACE_SLOTS

/** The modes, as the `mode` choice's positions (FBS_MODES). */
#define OMX_FBS_LV2_CUT OMX_FBS_MODES_CUT
#define OMX_FBS_LV2_REPORT OMX_FBS_MODES_REPORT

_Static_assert(OMX_FBS_LV2_SLOTS <= OMX_EQ_MAX_BANDS, "the cascade holds one section per report slot");
_Static_assert(OMX_FBS_MODES_COUNT == 2u, "the declared modes are cut and report");
_Static_assert((int)OMX_FBS_POOLS_FIXED == (int)OMX_FBS_POOL_FIXED && (int)OMX_FBS_POOLS_LIVE == (int)OMX_FBS_POOL_LIVE &&
                   OMX_FBS_POOLS_COUNT == 2u,
               "the declared pools are OmxFbsPool");

/** What the audio thread hands one pass: the arming controls as they stood. Fixed size, no pointers. */
typedef struct {
  int pool; /**< OmxFbsPool, inside the declared travel */
  double hold_s;
  double max_bands;
  double notch_q;
  double merge_cents;
  double cooldown_s;
} OmxFbsLv2Ask;

/** What one pass hands back: the cascade by slot and the report by slot. Fixed size, no pointers. */
typedef struct {
  float coeffs[OMX_FBS_LV2_SLOTS][5];
  uint8_t enabled[OMX_FBS_LV2_SLOTS];
  OmxBandReport report[OMX_FBS_LV2_SLOTS];
} OmxFbsLv2Set;

/** One instance. Every pointer is into the caller's block. */
typedef struct {
  uint32_t rate;
  uint32_t fft;
  uint32_t hop;

  /* audio -> worker */
  OmxLiveFeed feed;

  /* the audio side */
  OmxHrpCascade live;
  int mode;
  int analysing;
  OmxBandReport report[OMX_FBS_LV2_SLOTS];

  /* the worker side */
  OmxFbsDetector det;
  OmxFbsResult res;
  OmxFbsPlace place;
  /** Frames every pass has consumed, ever: the placement's clock, kept across a restart. */
  uint64_t elapsed;
  float *frame;
  float *work_re;
  float *work_im;
  float *mags_f;
  double *mags;
  double *snap_hz;

  int ready;
} OmxFbsLv2;

/* the block's partition: doubles and 8-byte members first, then the 4-byte ones */
#define OMX_FBS_LV2_ALIGN_(n) (((n) + 7u) & ~(size_t)7u)

/**
 * @brief Bytes the caller's block holds at `rate` for {@link omx_fbs_lv2_init_block}.
 * @param rate The sample rate, Hz.
 * @return The detector's slots, results and bin map, the spectrum in double, the snapshot frequencies, the
 *         window and the FFT's work, each aligned to 8 bytes.
 * @thread control
 */
static inline size_t omx_fbs_lv2_block_bytes(uint32_t rate) {
  const size_t fft = omx_rta_fft_size_for(rate), bins = fft / 2u, n = OMX_FBS_TRACK_SLOTS;
  return OMX_FBS_LV2_ALIGN_(n * sizeof(OmxFbsSlot)) + 2u * OMX_FBS_LV2_ALIGN_(n * sizeof(OmxFbsCandidate)) +
         OMX_FBS_LV2_ALIGN_(bins * sizeof(double)) + OMX_FBS_LV2_ALIGN_(n * sizeof(double)) +
         OMX_FBS_LV2_ALIGN_(bins * sizeof(int32_t)) + 2u * OMX_FBS_LV2_ALIGN_(n * sizeof(uint32_t)) +
         3u * OMX_FBS_LV2_ALIGN_(fft * sizeof(float)) + OMX_FBS_LV2_ALIGN_(bins * sizeof(float));
}

/**
 * @brief Bind an instance to its rate and one block of {@link omx_fbs_lv2_block_bytes} bytes, 8-aligned.
 * @param s The instance.
 * @param rate The sample rate, Hz.
 * @param block The caller's block, or NULL.
 * @return 1 when usable; 0 (and `ready` clear) for a NULL block or a zero rate, after which the instance
 *         passes audio and corrects nothing.
 * @thread control
 */
static inline int omx_fbs_lv2_init_block(OmxFbsLv2 *s, uint32_t rate, void *block) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  omx_live_report_clear(s->report, OMX_FBS_LV2_SLOTS);
  if (rate == 0u || !block || ((uintptr_t)block & 7u) != 0u) return 0;
  const size_t fft = omx_rta_fft_size_for(rate), bins = fft / 2u, n = OMX_FBS_TRACK_SLOTS;
  unsigned char *b = (unsigned char *)block;
#define OMX_FBS_LV2_TAKE_(ptr, type, count)                                                                    \
  do {                                                                                                        \
    (ptr) = (type *)(void *)b;                                                                                \
    b += OMX_FBS_LV2_ALIGN_((count) * sizeof(type));                                                          \
  } while (0)
  OmxFbsSlot *slots;
  OmxFbsCandidate *confirmed, *snapshot;
  int32_t *bin_slot;
  uint32_t *free_stack, *active;
  OMX_FBS_LV2_TAKE_(slots, OmxFbsSlot, n);
  OMX_FBS_LV2_TAKE_(confirmed, OmxFbsCandidate, n);
  OMX_FBS_LV2_TAKE_(snapshot, OmxFbsCandidate, n);
  OMX_FBS_LV2_TAKE_(s->mags, double, bins);
  OMX_FBS_LV2_TAKE_(s->snap_hz, double, n);
  OMX_FBS_LV2_TAKE_(bin_slot, int32_t, bins);
  OMX_FBS_LV2_TAKE_(free_stack, uint32_t, n);
  OMX_FBS_LV2_TAKE_(active, uint32_t, n);
  OMX_FBS_LV2_TAKE_(s->frame, float, fft);
  OMX_FBS_LV2_TAKE_(s->work_re, float, fft);
  OMX_FBS_LV2_TAKE_(s->work_im, float, fft);
  OMX_FBS_LV2_TAKE_(s->mags_f, float, bins);
#undef OMX_FBS_LV2_TAKE_
  s->rate = rate;
  s->fft = (uint32_t)fft;
  s->hop = rate / (uint32_t)OMX_FBS_WATCH_HZ;
  s->det.t = omx_fbs_thresholds_default();
  s->det.pool = OMX_FBS_POOL_LIVE;
  s->det.sample_rate = (double)rate;
  s->det.fft_size = (uint32_t)fft;
  s->det.bin_slot = bin_slot;
  s->det.nbins_cap = (uint32_t)bins;
  s->det.slots = slots;
  s->det.free_stack = free_stack;
  s->det.n_slots = (uint32_t)n;
  omx_fbs_init(&s->det);
  s->res.confirmed = confirmed;
  s->res.snapshot = snapshot;
  s->res.active = active;
  omx_fbs_place_init(&s->place, OMX_FBS_POOL_LIVE);
  omx_hrp_cascade_init(&s->live);
  s->live.count = OMX_FBS_LV2_SLOTS;
  s->mode = OMX_FBS_MODES_DEFAULT;
  s->ready = 1;
  return 1;
}

/**
 * @brief The `mode` control's word, resolved and held for the next run(): cut or report.
 * @param s The instance; NULL is a no-op.
 * @param word The control word: rounded to a declared mode's position, a non-finite word reading as cut.
 * @thread audio
 */
static inline void omx_fbs_lv2_set_mode(OmxFbsLv2 *s, float word) {
  if (s) s->mode = omx_port_int(word, 0, (int)OMX_FBS_MODES_COUNT - 1, OMX_FBS_MODES_DEFAULT);
}

/**
 * @brief The mode in force, as a controller reads it back before it plants.
 * @param s The instance.
 * @return OMX_FBS_LV2_CUT or OMX_FBS_LV2_REPORT; cut for NULL.
 * @thread audio
 */
static inline int omx_fbs_lv2_mode(const OmxFbsLv2 *s) { return s ? s->mode : OMX_FBS_LV2_CUT; }

/**
 * @brief The audio callback's whole share: the input into the ring, then the bands (cut) or nothing (report).
 * @param s The instance; NULL or not ready passes the audio bit-exact.
 * @param in The input block.
 * @param out The output block; may be `in`.
 * @param n Frames.
 * @note No FFT, no detector, no placement, no allocation, no lock: those are the worker's.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "fbs/lv2-run"
static inline void omx_fbs_lv2_run(OmxFbsLv2 *s, const float *in, float *out, uint32_t n) {
  if (!omx_live_block(s && s->ready, in, out, n)) return;
  omx_live_feed(&s->feed.ring, &s->feed.written, in, n);
  omx_live_out(&s->live, s->mode == OMX_FBS_LV2_REPORT, in, out, n);
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Is a hop's worth of audio unconsumed?
 * @param s The instance.
 * @return 1 when a pass is due, else 0 (never for NULL or not ready).
 * @thread audio
 */
static inline int omx_fbs_lv2_due(const OmxFbsLv2 *s) {
  if (!s || !s->ready) return 0;
  return omx_live_unconsumed(&s->feed.written, &s->feed.consumed) >= (uint64_t)s->hop;
}

/**
 * @brief The arming controls a face read, as one pass's snapshot.
 * @param pool The pool word: rounded inside the declared travel, a non-finite word reading as its default.
 * @param hold The hold word, seconds.
 * @param bands The band cap word.
 * @param notch_q The width word.
 * @param merge The merge radius word, cents.
 * @param cooldown The ring memory word, seconds.
 * @return The snapshot; the placement clamps the rest when the worker installs it.
 * @thread audio
 */
static inline OmxFbsLv2Ask omx_fbs_lv2_ask_of(float pool, float hold, float bands, float notch_q, float merge,
                                              float cooldown) {
  OmxFbsLv2Ask a;
  a.pool = omx_port_int(pool, 0, (int)OMX_FBS_POOLS_COUNT - 1, OMX_FBS_POOLS_DEFAULT);
  a.hold_s = (double)hold;
  a.max_bands = (double)bands;
  a.notch_q = (double)notch_q;
  a.merge_cents = (double)merge;
  a.cooldown_s = (double)cooldown;
  return a;
}

/**
 * @brief Count one pass as asked for (after the face handed the snapshot to the worker).
 * @param s The instance; NULL is a no-op.
 * @thread audio
 */
static inline void omx_fbs_lv2_mark_asked(OmxFbsLv2 *s) {
  if (s) omx_live_count(&s->feed.asked);
}

/**
 * @brief Count one pass as answered (once its set is where the audio thread adopts it).
 * @param s The instance; NULL is a no-op.
 * @thread worker
 */
static inline void omx_fbs_lv2_mark_answered(OmxFbsLv2 *s) {
  if (s) omx_live_count(&s->feed.answered);
}

/**
 * @brief Is a pass in flight: asked for and not yet answered?
 * @param s The instance.
 * @return 1 or 0.
 * @thread audio
 */
static inline int omx_fbs_lv2_in_flight(const OmxFbsLv2 *s) {
  return s ? omx_live_in_flight(&s->feed.asked, &s->feed.answered) : 0;
}

/**
 * @brief One pass on the worker: install the snapshot, take the oldest unconsumed hop, run the detector on the
 *        newest window ending there, tick the placement on the hop's clock, and write the set.
 * @param s The instance.
 * @param a The snapshot; NULL keeps the arming in force.
 * @param out The set; zeroed, then every slot's coefficients and report. Nothing is consumed when no whole hop
 *        is unconsumed, and the set then repeats the placement as it stands.
 * @note A worker that fell a ring behind skips to the newest whole hop (late is fine; a backlog is not).
 * @thread worker
 */
static inline void omx_fbs_lv2_answer(OmxFbsLv2 *s, const OmxFbsLv2Ask *a, OmxFbsLv2Set *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  omx_live_report_clear(out->report, OMX_FBS_LV2_SLOTS);
  if (!s || !s->ready) return;
  if (a) {
    omx_fbs_place_set(&s->place, a->hold_s, a->max_bands, a->notch_q, a->merge_cents, a->cooldown_s);
    omx_fbs_place_set_pool(&s->place, (OmxFbsPool)a->pool);
    s->det.pool = (OmxFbsPool)a->pool;
  }
  if (omx_live_take(&s->feed, s->frame, s->fft, s->hop)) {
    omx_rta_spectrum(s->frame, s->fft, s->work_re, s->work_im, s->mags_f);
    const uint32_t bins = s->fft / 2u;
    for (uint32_t i = 0; i < bins; i++) s->mags[i] = (double)s->mags_f[i];
    omx_fbs_push(&s->det, s->mags, bins, &s->res);
    for (uint32_t i = 0; i < s->res.n_snapshot; i++) s->snap_hz[i] = s->res.snapshot[i].freq_hz;
    s->elapsed += s->hop;
    s->place.n_log = 0u;
    omx_fbs_place_tick(&s->place, (double)s->elapsed * 1000.0 / (double)s->rate, s->snap_hz, s->res.n_snapshot,
                       s->res.confirmed, s->res.n_confirmed);
  }
  for (uint32_t k = 0; k < OMX_FBS_LV2_SLOTS; k++) {
    const OmxBandReport r = omx_fbs_place_report(&s->place, k);
    out->report[k] = r;
    if (!(r.on > 0.5f)) continue;
    const enum omx_eq_kind kind = (int)r.shape == OMX_CURVE_SHAPES_NOTCH ? OMX_EQ_NOTCH : OMX_EQ_PEAKING;
    omx_eq_design_f(kind, s->place.slot[k].freq_hz, s->place.slot[k].q, s->place.slot[k].gain_db, (double)s->rate,
                    out->coeffs[k]);
    out->enabled[k] = 1u;
  }
}

/**
 * @brief Install a set on the audio thread: the coefficients and the report by slot.
 * @param s The instance; NULL or not ready is a no-op.
 * @param set The set; NULL is a no-op.
 * @note A section that comes on starts from zero state; one that stays on keeps its history (a coefficient
 *       move is the ramp's, at the control rate). Constant time. Marks the instance as analysing.
 * @thread audio
 */
static inline void omx_fbs_lv2_adopt(OmxFbsLv2 *s, const OmxFbsLv2Set *set) {
  if (!s || !s->ready || !set) return;
  for (uint32_t k = 0; k < OMX_FBS_LV2_SLOTS; k++) {
    if (set->enabled[k] && !s->live.enabled[k]) memset(s->live.state[k], 0, sizeof(s->live.state[k]));
    memcpy(s->live.coeffs[k], set->coeffs[k], 5u * sizeof(float));
    s->live.enabled[k] = set->enabled[k] ? 1u : 0u;
    s->report[k] = set->report[k];
  }
  s->analysing = 1;
}

/**
 * @brief Slot k's six report words, by value.
 * @param s The instance.
 * @param k The slot; one past the slots, or NULL, reads as a free slot.
 * @return The report as last adopted.
 * @thread audio
 */
static inline OmxBandReport omx_fbs_lv2_report(const OmxFbsLv2 *s, uint32_t k) {
  return s ? omx_live_report_at(s->report, OMX_FBS_LV2_SLOTS, k) : omx_band_report_free();
}

/**
 * @brief The read-only `analysing` flag: 1 once a worker has answered a pass, 0 on a host with no worker.
 * @param s The instance.
 * @return 1 or 0.
 * @thread audio
 */
static inline int omx_fbs_lv2_analysing(const OmxFbsLv2 *s) { return s ? s->analysing : 0; }

/**
 * @brief A new stream at activation: the cascade's history and both cursors back to zero, the detector's
 *        tracking fresh. The placement, its bands and the report are kept: a transport stop is not a new room.
 * @param s The instance; NULL is a no-op.
 * @note Never while a pass is in flight: the caller holds the worker idle.
 * @thread control
 */
static inline void omx_fbs_lv2_restart(OmxFbsLv2 *s) {
  if (!s || !s->ready) return;
  memset(s->live.state, 0, sizeof(s->live.state));
  omx_ring_reset(&s->feed.ring);
  atomic_store_explicit(&s->feed.written, 0u, memory_order_relaxed);
  atomic_store_explicit(&s->feed.consumed, 0u, memory_order_relaxed);
  omx_fbs_init(&s->det);
}

#endif /* OMX_FBS_LV2_H */
