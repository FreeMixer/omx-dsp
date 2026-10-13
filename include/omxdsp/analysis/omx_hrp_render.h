/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_hrp_render.h — HRP over a WHOLE RECORDED TRACK: the offline render kernel.
 *
 * Spec: docs/design/specs/2026-07-16-recording-vsc.md §0.5 (HRP+ renders offline, beside the
 * originals — "whole-file analysis with the same C engine, no live budget: polyphonic,
 * iterative, look-ahead"), over docs/design/specs/2026-08-18-hrp-polyphonic-architecture.md,
 * whose laws this obeys unchanged except where §0.5 lifts a LIVE constraint and this header
 * names the line that lifts it.
 *
 * THE SAME ENGINE, NOT A SECOND ONE. Every measurement below is made by the modules the live
 * corrector uses — `omx_rta_spectrum` (mix_dsp.h), `omx_hrp_detect` (hrp_pitch.h),
 * `omx_hrp_track_update` (hrp_track.h), `omx_hrp_attribute` (hrp_attribute.h),
 * `omx_hrp_baseline_learn` (hrp_baseline.h) — in the same order `hrp_analyze.c` runs them.
 * What is new here is only WHEN the decision is made and HOW MANY TIMES the audio is read.
 *
 * ## What offline lifts, and the line that permits it
 *
 *   1. THE RIPENING DELAY. §15 ("late analysis is intentional ... it only delays the
 *      ACTIVATION of HRP corrections") plus §22's `ripe_updates` mean a live corrector cannot
 *      cut a note until the model has heard that note hold for seconds. So the opening of every
 *      new note is, by construction, uncorrected live. Offline the whole file exists before one
 *      sample is written, so {@link omx_hrp_render_feed} is run TWICE: pass one teaches the
 *      per-note model from the entire performance, pass two measures every window — the first
 *      one included — against the model the whole file taught. §0.5: "whole-file analysis".
 *
 *   2. LOOK-AHEAD, AND A CORRECTION IN FORCE FROM SAMPLE ZERO. §2's "HRP adds ZERO intentional
 *      audio latency" governs the RT path, and §4 puts this library off it entirely. With no
 *      callback to be late for, a correction may be decided from evidence that extends PAST the
 *      sample it is applied to — which is what makes the render's first chorus correctable from
 *      the second chorus's evidence. §0.5: "look-ahead". The cost live pays for this is
 *      latency; offline the cost is a second read of the file, which is free.
 *
 *   3. THE PER-FRAME BUDGET. §57 measures HRP's cost so it never interferes with the RT
 *      callback. There is no callback here, so the search runs at the full {@link
 *      OMX_HRP_MAX_VOICES} ceiling with the caller's own sweep step rather than the one a live
 *      tick can afford. §0.5: "no live budget: polyphonic, iterative".
 *
 * ## What still binds, and is not negotiable offline
 *
 *   - §1: no second FFT, RTA, EQ or biquad. The spectrum is `omx_rta_spectrum`'s and the
 *     correction is `omx_biquad_cascade`'s. The curve maths stay in `@freemixer/core`'s
 *     `eqCoeffs` — this kernel is handed READY {b0,b1,b2,a1,a2} sets, exactly as the RT lane is
 *     (mix_lane.h: "designed in TS"). One home for the coefficient design, two consumers.
 *   - §4: no allocation, no globals, no logging, no I/O, no PipeWire, no N-API. Every buffer is
 *     the caller's. That is what lets `hrp_render.test.c` compile this standalone against
 *     closed-form oracles, and what keeps an LV2 shell possible: a host that can design its own
 *     coefficients links this header and needs nothing else from the console.
 *   - §5: the answer is a voice SET; nothing here indexes "the" f0.
 *   - §10: attribution stays the conservative harmonic window. No source separation, and no
 *     joint scoring — offline does the SAME arithmetic on MORE evidence, never different
 *     arithmetic.
 *   - §11 / §54: an ambiguous partial is not evidence. It teaches nothing and it is measured
 *     into nothing. A missed correction beats a wrong cut, and that is more true of a rendered
 *     file than of a live desk, because the file is what someone keeps.
 *   - §13 / §21: only a STABLE voice teaches, and only a stable voice is measured. An attack is
 *     not a resonance.
 *   - §55: the octave guard is `omx_hrp_detect`'s and is untouched.
 *
 * ## The report is SUMS, never averages
 *
 * Every derived number ({@link omx_hrp_render_excess_db}, {@link omx_hrp_render_duty},
 * {@link omx_hrp_render_confidence}) is a function of what was accumulated, not a field that
 * has to be recomputed at the right moment. So there is no "finish" step to forget and no field
 * whose meaning depends on which phase the caller is in.
 */
#ifndef OMX_HRP_RENDER_H
#define OMX_HRP_RENDER_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_attribute.h>
#include <omxdsp/analysis/omx_hrp_baseline.h>
#include <omxdsp/analysis/omx_hrp_correct.h>
#include <omxdsp/analysis/omx_hrp_pitch.h>
#include <omxdsp/analysis/omx_hrp_track.h>
#include <omxdsp/analysis/omx_rta.h>

/* Windows a (note, harmonic) must be measured over before it is evidence about the take.
 *
 * The BASELINE's own gate is 24 accepted updates (`omx_hrp_baseline_config_default`), and a cell
 * only counts a window once that row is ripe — so anything reported here has already been heard
 * 24 times in the teaching pass. This floor is the second, smaller one: it refuses a cell whose
 * note recurred in the teaching pass but barely sounds in the measured one, where a single
 * window's excess would otherwise be the whole story. Eight windows is ~0.34 s at the default
 * hop — long enough that a resonance rang rather than a transient ticked. */
#define OMX_HRP_RENDER_MIN_FRAMES 8u

/* ---- the three passes -----------------------------------------------------------------
 *
 * WHICH ONE A SHELL RUNS IS DECIDED BY HOW MANY TIMES IT CAN READ THE AUDIO, and that is the
 * only thing that separates the offline render from the live insert.
 *
 * The offline shell reads the file TWICE, so it can afford to keep teaching and measuring
 * apart: {@link OMX_HRP_RENDER_LEARN} over the whole take builds the model, and
 * {@link OMX_HRP_RENDER_MEASURE} over the whole take reads every window — the opening
 * included — against the model the ending helped teach. That separation IS §0.5's whole-file
 * privilege.
 *
 * A LIVE SHELL READS THE AUDIO ONCE. It has no second pass to measure in, so
 * {@link OMX_HRP_RENDER_LIVE} does both in one window, in the order that keeps the answer
 * honest: the deviation is read against the model AS IT STOOD BEFORE this window, and only
 * then does this window teach. The other order would let a resonance help set the baseline it
 * is about to be measured against — the model absorbing the very thing it exists to detect,
 * biasing every excess toward zero, and doing it most on the loudest resonances.
 *
 * Nothing else differs. The same detector, tracker, attribution and gates run in the same
 * order for all three, because a second spelling of detect->track->attribute is how two tiers
 * would come to disagree about what a window contains. The values are chosen so the offline
 * callers' literal 1 and 0 keep meaning exactly what they meant. */

/** Measure against the ripe model; teach nothing. Offline pass two. */
#define OMX_HRP_RENDER_MEASURE 0
/** Teach the model; measure nothing. Offline pass one. */
#define OMX_HRP_RENDER_LEARN 1
/** Teach AND measure, measuring first — the only pass a one-read shell can run. */
#define OMX_HRP_RENDER_LIVE 2

/* The most one accepted measurement may move the learned level offline, in dB.
 *
 * A FIFTH of the live default, and the fourth thing §0.5's "no live budget" buys. §22 asks for a
 * robust estimator; live sizes its step (0.5 dB) so a model becomes useful inside one song,
 * because a model that has not converged by the second chorus is no use at a show. Offline the
 * number of accepted updates is the whole take's, so the step can be small enough that a
 * resonance ringing for a quarter of the performance cannot drag the baseline up to meet it —
 * which is precisely the failure that would make a real resonance measure as normal timbre.
 * Same estimator, same gates, converging on the performance instead of on its last few seconds. */
#define OMX_HRP_RENDER_BASELINE_STEP_DB 0.1f

/* One (note, harmonic) cell of the whole-file measurement.
 *
 * MAGNITUDE AND STABILITY ARE SEPARATE, because §24 lists them separately: "positive deviation"
 * and "temporal stability" are two of the terms a resonance score considers, and folding them
 * into one mean would make a partial that rings 12 dB hot a quarter of the time indistinguishable
 * from one that sits 3 dB hot throughout. The first is a resonance; the second is the
 * instrument's timbre, which HRP must not touch. */
typedef struct {
  /** The learned level this cell was measured against, dB — the model's answer for this row. */
  float baseline_db;
  /** Σ over HOT windows of (measured − baseline). §23's positive deviations only: a partial
   *  QUIETER than its baseline is not a resonance and HRP never boosts (§25). "Hot" means
   *  above the baseline by more than the model's own step — see {@link omx_hrp_render_window}. */
  float sum_excess_db;
  /** Σ over hot windows of the partial's attribution confidence (§10's third output). */
  float sum_confidence;
  /** Σ over hot windows of the voice's own confidence — §24's "voice confidence" term. */
  float sum_voice_confidence;
  /** Σ over hot windows of the voice's MEASURED fundamental, Hz.
   *
   * Carried so the placement law can put the band where the instrument actually sounded,
   * rather than at the equal-tempered frequency of the note it was filed under. A voice may
   * sit up to fifty cents from its note — half a Q-5 bell's half-width — and a correction
   * placed on the tempered grid would be that far off the partial it was measured on. */
  float sum_f0_hz;
  /** The largest single positive deviation seen. Reported, never corrected on: one window is
   *  not a property of a take. It is what an operator reads to tell a ring from a clip. */
  float peak_excess_db;
  /** Windows where this partial was present, unambiguous, from a stable voice, over a ripe row. */
  uint32_t frames;
  /** Of those, the ones whose deviation was positive — the numerator of the duty. */
  uint32_t hot_frames;
} OmxHrpRenderCell;

/** What a whole file was measured to be doing, per note and per harmonic. */
typedef struct {
  OmxHrpRenderCell cell[OMX_HRP_BASELINE_NOTES][OMX_HRP_HARMONICS];
  /** Windows the measuring pass analysed — the denominator a caller reports progress against. */
  uint32_t windows;
  /** Windows in which at least one voice was tracked and stable. Zero means the track is not
   *  pitched material, which is a first-class answer and not a failure. */
  uint32_t pitched_windows;
} OmxHrpRenderReport;

/** The search this render runs — every tier the live chain configures, in one place. */
typedef struct {
  /** The take's sample rate. Carried, never assumed: §45 forbids hard-coding bin frequencies
   *  for one rate, and `bin_hz = rate / fft_size` is the one place the two meet. */
  uint32_t rate;
  /** Analysis window, a power of two. {@link omx_rta_fft_size_for} of the take's rate. */
  uint32_t fft_size;
  /** Frames between windows. `fft_size / 4` gives the 75 % overlap the default config uses. */
  uint32_t hop;
  OmxHrpConfig pitch;
  OmxHrpTrackConfig track;
  OmxHrpBaselineConfig baseline;
} OmxHrpRenderConfig;

/**
 * One track's render state: the model, the tracker, the measurement, and the caller's scratch.
 *
 * NO BUFFER IS OWNED HERE. {@link omx_hrp_render_init} takes five caller-provided arrays, so a
 * shell decides where the memory comes from — a malloc at the N-API boundary, a stack frame in
 * a test, an LV2 host's own pool. §4 forbids this library allocating, and a render that had to
 * allocate could not be the same code the live tier is held to.
 */
typedef struct {
  OmxHrpRenderConfig cfg;
  OmxHrpTracker tracker;
  /** The voice set the LAST analysed window projected out of `tracker` — this path's own
   *  marshalled answer about that window, by {@link omx_hrp_project_voices}, the one projection
   *  the live tier marshals a frame with too. Kept rather than local so what an offline window
   *  concluded is READABLE: a shell reports what it is hearing from it, and `hrp_render.test.c`
   *  compares it with the live chain's byte for byte. Empty until the first window, and emptied
   *  by {@link omx_hrp_render_rewind} with the tracker it came from. */
  OmxHrpVoiceSet voices;
  OmxHrpBaseline baseline;
  OmxHrpRenderReport report;
  /** The sliding analysis window: `cfg.fft_size` floats, filled by {@link omx_hrp_render_feed}. */
  float *carry;
  /** FFT scratch, `cfg.fft_size` floats each. */
  float *work_re;
  float *work_im;
  /** Magnitude spectrum and the detector's residual, `cfg.fft_size / 2` floats each. */
  float *mags;
  float *residual;
  /** Valid samples currently in `carry`. */
  uint32_t fill;
  /** Windows the TEACHING pass analysed — the progress denominator for pass one. */
  uint32_t learn_windows;
  /** Set once {@link omx_hrp_render_init} has been given a usable config and every buffer. */
  int ready;
} OmxHrpRender;

/**
 * The defaults a take of `rate` Hz is analysed with.
 *
 * The window LENGTH is the RTA's own, derived from the rate ({@link omx_rta_fft_size_for}) so
 * that a bin is about {@link OMX_RTA_TARGET_BIN_HZ} wide whatever the take was recorded at —
 * §45 forbids hard-coding bin frequencies for one rate, and deriving the length is how the live
 * tier already obeys it.
 *
 * The HOP is a quarter window. Live, the analysis tick is what the demand-held feed delivers;
 * offline the caller chooses, and three quarters of overlap is the point where a partial that
 * rings for one window length is seen by four measurements rather than one — evidence §24's
 * temporal-stability term can actually count.
 */
static inline OmxHrpRenderConfig omx_hrp_render_config_default(uint32_t rate) {
  OmxHrpRenderConfig c;
  c.rate = rate;
  c.fft_size = omx_rta_fft_size_for(rate);
  c.hop = c.fft_size / 4u;
  c.pitch = omx_hrp_config_default();
  c.track = omx_hrp_track_config_default();
  c.baseline = omx_hrp_baseline_config_default();
  c.baseline.step_db = OMX_HRP_RENDER_BASELINE_STEP_DB;
  return c;
}

/**
 * Bind a render to its config and the caller's five buffers. Returns 1 when the render is
 * usable, 0 when it is not — a zero window, a window that is not a power of two, a hop outside
 * (0, fft_size], or a missing buffer. A refused init leaves `ready` clear, and every entry
 * below is a no-op on a render that is not ready: absence is a fact, not a crash.
 *
 * `carry`, `work_re` and `work_im` hold `fft_size` floats; `mags` and `residual` hold
 * `fft_size / 2`.
 */
static inline int omx_hrp_render_init(OmxHrpRender *r, const OmxHrpRenderConfig *cfg,
                                      float *carry, float *work_re, float *work_im, float *mags,
                                      float *residual) {
  if (!r || !cfg) return 0;
  memset(r, 0, sizeof(*r));
  const uint32_t n = cfg->fft_size;
  if (n < 4u || (n & (n - 1u)) != 0u) return 0;
  if (cfg->hop == 0u || cfg->hop > n) return 0;
  if (!carry || !work_re || !work_im || !mags || !residual) return 0;
  r->cfg = *cfg;
  r->carry = carry;
  r->work_re = work_re;
  r->work_im = work_im;
  r->mags = mags;
  r->residual = residual;
  omx_hrp_tracker_init(&r->tracker);
  omx_hrp_baseline_init(&r->baseline);
  r->ready = 1;
  return 1;
}

/**
 * Return to the start of the audio WITHOUT forgetting what was learned.
 *
 * This is the seam between the two passes and the whole of the offline privilege in one call:
 * the window machine and the voice tracker are emptied (frame zero of pass two must not inherit
 * pass one's last window, or the file's opening would be measured against its ending), while the
 * BASELINE — everything the file taught — is kept. Calling it a second time before a third pass
 * would be equally safe.
 */
static inline void omx_hrp_render_rewind(OmxHrpRender *r) {
  if (!r || !r->ready) return;
  r->fill = 0;
  omx_hrp_tracker_init(&r->tracker);
  /* The last window's voice set goes with the tracker it was projected from: pass two must not
     open by reporting voices pass one heard at the END of the file. */
  memset(&r->voices, 0, sizeof(r->voices));
}

/** Forget the measurement, keeping the model — for a caller that re-measures under a new search. */
static inline void omx_hrp_render_reset_report(OmxHrpRender *r) {
  if (!r) return;
  memset(&r->report, 0, sizeof(r->report));
}

/* One analysis window: the live chain's own order, over the window sitting in `carry`.
 *
 * `learn` selects the pass. Teaching accepts a measurement under §21's gates exactly as
 * `hrp_analyze.c` does; measuring reads the ripe model and accumulates the deviation. The two
 * share this function rather than duplicating the chain, because a second spelling of
 * detect->track->attribute is how the two passes would come to disagree about what a window
 * contains. */
static inline void omx_hrp_render_window(OmxHrpRender *r, int pass) {
  const int do_learn = (pass != OMX_HRP_RENDER_MEASURE);
  const int do_measure = (pass != OMX_HRP_RENDER_LEARN);
  const uint32_t nbins = r->cfg.fft_size / 2u;
  const float bin_hz = (float)r->cfg.rate / (float)r->cfg.fft_size;

  omx_rta_spectrum(r->carry, r->cfg.fft_size, r->work_re, r->work_im, r->mags);

  OmxHrpVoiceSet detected;
  omx_hrp_detect(r->mags, nbins, bin_hz, &r->cfg.pitch, r->residual, &detected);
  omx_hrp_track_update(&r->tracker, &detected, &r->cfg.track);

  /* Attribution runs over the TRACKED voices, through the projection hrp_analyze.c marshals
     the live frame with — one `omx_hrp_project_voices`, so offline and live cannot come to
     disagree about what the same tracker contains. The grids must follow identity, or a voice's
     harmonic history would reshuffle every window with the detector's ordering. Ambiguity is a
     property of the ensemble, so the whole set goes in at once (§11). */
  /* Projected into the render's OWN field, not a local: the voice set a window projects is this
     path's marshalled answer about that window, and an answer that dies with the call can be
     neither reported by a shell nor compared with the live tier's. `hrp_render.test.c`'s parity
     claim reads exactly this. */
  OmxHrpVoiceSet *tracked = &r->voices;
  const OmxHrpTrack *by_slot[OMX_HRP_MAX_VOICES];
  omx_hrp_project_voices(&r->tracker, tracked, by_slot);

  OmxHrpVoiceHarmonics grids[OMX_HRP_MAX_VOICES];
  memset(grids, 0, sizeof(grids));
  omx_hrp_attribute(r->mags, nbins, bin_hz, tracked, &r->cfg.pitch, grids);

  int any_stable = 0;
  for (uint32_t i = 0; i < tracked->voice_count; i++) {
    const OmxHrpTrack *t = by_slot[i];
    /* §13 / §21: an unstable voice teaches nothing and is measured into nothing. An attack is
       not a resonance, and offline has no reason to be less careful than live about that. */
    if (!t->stable) continue;
    if (t->midi_note < 0 || t->midi_note >= OMX_HRP_BASELINE_NOTES) continue;
    any_stable = 1;

    for (uint32_t k = 0; k < grids[i].count; k++) {
      const OmxHrpPartial *p = &grids[i].partials[k];
      /* §11: energy two voices share is evidence about neither. */
      if (!p->present || p->ambiguous) continue;
      if (p->harmonic < 1 || p->harmonic > OMX_HRP_HARMONICS) continue;

      /* MEASURE FIRST, THEN TEACH — see the pass constants. On a LEARN pass there is nothing
         to measure; on a MEASURE pass there is nothing to teach; on a LIVE pass the order is
         what stops the model absorbing the resonance it is meant to find. */
      float base_db = 0.0f;
      /* An unripe row answers nothing, and nothing is what is done with it (§54). */
      const int ripe = omx_hrp_baseline_read(&r->baseline, t->midi_note, p->harmonic,
                                             &r->cfg.baseline, &base_db);
      if (do_measure && ripe) {
      OmxHrpRenderCell *cell = &r->report.cell[t->midi_note][p->harmonic - 1u];
      cell->baseline_db = base_db;
      cell->frames++;
      const float deviation = p->level_db - base_db;
      /* §23 / §25: only a POSITIVE deviation is a resonance candidate, and HRP never boosts —
         so a partial below its baseline contributes to the denominator (it is evidence about
         how often the resonance is NOT ringing) and to nothing else.
       *
       * THE FLOOR IS THE MODEL'S OWN STEP, and it is derived rather than typed: the estimator
       * moves by up to `step_db` per accepted update, so a deviation smaller than that is
       * inside the model's own wobble and says nothing about the take. Without it a partial
       * sitting exactly ON its baseline registers as ringing in about half its windows on
       * float rounding alone, which would put a duty near a half on a signal that never
       * moved — the temporal-stability term (§24) reading noise as evidence. */
      if (deviation > r->cfg.baseline.step_db) {
        cell->hot_frames++;
        cell->sum_excess_db += deviation;
        cell->sum_confidence += p->confidence;
        cell->sum_voice_confidence += t->confidence;
        cell->sum_f0_hz += t->f0_hz;
        if (deviation > cell->peak_excess_db) cell->peak_excess_db = deviation;
      }
      }
      if (do_learn) {
        omx_hrp_baseline_learn(&r->baseline, t->midi_note, p->harmonic, p->level_db,
                               &r->cfg.baseline);
      }
    }
  }

  if (do_learn) r->learn_windows++;
  if (do_measure) {
    r->report.windows++;
    if (any_stable) r->report.pitched_windows++;
  }
}

/**
 * Feed `n` frames of the track, in order, and analyse every window that completes.
 *
 * `pass` is one of {@link OMX_HRP_RENDER_MEASURE}, {@link OMX_HRP_RENDER_LEARN} or
 * {@link OMX_HRP_RENDER_LIVE} — see the pass constants for which shell runs which and why.
 * Returns the windows analysed by this call, so a shell can report progress without a second
 * accounting of the audio.
 *
 * THE WINDOW MACHINE IS A SLIDE, NOT A RING. `carry` fills, a window is taken, and the tail is
 * moved down by `hop`. A ring would save the move; it would also make the buffer handed to
 * `omx_rta_spectrum` discontiguous, and §1's one FFT takes a contiguous block. Offline the move
 * costs `fft_size` floats per hop and buys the simplest correct thing.
 *
 * THE TAIL IS NOT PADDED. Fewer than `fft_size` frames remain at the end of a file and no
 * window is taken over them. Zero-padding would put a window's worth of manufactured silence
 * into the measurement of a real partial's level — an invented number in the one place the
 * whole feature is about levels. The audio is untouched by this; only the last fraction of a
 * second is unmeasured, and a resonance is not a property of the last 170 ms of a show.
 */
static inline uint32_t omx_hrp_render_feed(OmxHrpRender *r, const float *in, uint32_t n,
                                           int pass) {
  if (!r || !r->ready || !in) return 0;
  const uint32_t win = r->cfg.fft_size;
  const uint32_t hop = r->cfg.hop;
  uint32_t windows = 0;
  uint32_t off = 0;

  while (off < n) {
    uint32_t take = win - r->fill;
    if (take > n - off) take = n - off;
    memcpy(r->carry + r->fill, in + off, (size_t)take * sizeof(float));
    r->fill += take;
    off += take;
    if (r->fill < win) break;

    omx_hrp_render_window(r, pass);
    windows++;
    memmove(r->carry, r->carry + hop, (size_t)(win - hop) * sizeof(float));
    r->fill = win - hop;
  }
  return windows;
}

/** The mean excess, in dB, over the windows this partial was actually ringing in — §24's
 *  "positive deviation" term. Zero for a cell with no hot window, and for one below
 *  {@link OMX_HRP_RENDER_MIN_FRAMES}, which is the same statement: not evidence. */
static inline float omx_hrp_render_excess_db(const OmxHrpRenderCell *c) {
  if (!c || c->frames < OMX_HRP_RENDER_MIN_FRAMES || c->hot_frames == 0u) return 0.0f;
  return c->sum_excess_db / (float)c->hot_frames;
}

/** How much of this partial's sounding time it spent above its baseline, 0..1 — §24's
 *  "temporal stability" term, and the one number that separates a resonance from a tick. */
static inline float omx_hrp_render_duty(const OmxHrpRenderCell *c) {
  if (!c || c->frames < OMX_HRP_RENDER_MIN_FRAMES) return 0.0f;
  return (float)c->hot_frames / (float)c->frames;
}

/** The largest single positive deviation seen, in dB. What an operator reads to tell a ring
 *  from a clip; never what a correction is sized from — one window is not a take. */
static inline float omx_hrp_render_peak_excess_db(const OmxHrpRenderCell *c) {
  if (!c || c->frames < OMX_HRP_RENDER_MIN_FRAMES) return 0.0f;
  return c->peak_excess_db;
}

/** Mean attribution confidence over the hot windows — §10's third output, aggregated. */
static inline float omx_hrp_render_confidence(const OmxHrpRenderCell *c) {
  if (!c || c->hot_frames == 0u) return 0.0f;
  return c->sum_confidence / (float)c->hot_frames;
}

/** Mean voice confidence over the hot windows — §24's "voice confidence" term, aggregated. */
static inline float omx_hrp_render_voice_confidence(const OmxHrpRenderCell *c) {
  if (!c || c->hot_frames == 0u) return 0.0f;
  return c->sum_voice_confidence / (float)c->hot_frames;
}

/** The MEASURED fundamental this partial belongs to, Hz — the mean over the windows it rang
 *  in, so the band lands on the note that sounded and not on the tempered grid. */
static inline float omx_hrp_render_f0_hz(const OmxHrpRenderCell *c) {
  if (!c || c->hot_frames == 0u) return 0.0f;
  return c->sum_f0_hz / (float)c->hot_frames;
}

/*
 * ---- the correcting half -------------------------------------------------------------
 *
 * A rendered file is the take's samples through an ordinary EQ cascade, and that cascade is
 * `hrp_correct.h`'s — the SAME four functions the live LV2 insert applies its corrections
 * through, over the same `omx_biquad_cascade` beneath them. "A rendered cut and a live cut are
 * the same filter" is therefore true because it is the same code, not because two files were
 * kept in step by hand.
 *
 * WHERE THE COEFFICIENTS COME FROM depends on what the shell can reach, and that is the only
 * difference. This shell runs behind N-API, so `@freemixer/core`'s `eqCoeffs` designs the curve
 * and the render is handed ready `{b0,b1,b2,a1,a2}` sets — the same division of labour the RT
 * lane runs under (mix_dsp.h: "the C here only RUNS the biquad cascade with those coeffs"). A
 * shell with no TypeScript on the other side designs its own with
 * {@link omx_hrp_peaking_coeffs}, which is the same matched section held equal to `eqCoeffs`
 * by a corpus both languages check. Neither shell owns a second EQ (§1).
 */

#endif /* OMX_HRP_RENDER_H */
