// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The analysis engines' golden digests: every field FBS and the HRP chain answer, over fixed
 * spectra at the nine RME rates, hashed per engine and per rate and compared with
 * test/golden/analysis.sha256.
 *
 *   make test-analysis                        compare
 *   build/analysis_golden --write       print the lines test/golden/analysis.sha256 holds
 *
 * The committed digests were written by this program compiled against the OpenMixer engine's in-tree
 * fbs_detect.c and hrp_*.h, the code these headers moved from, so the moved code is held to the
 * engine's output bit for bit. The stimulus makes no libm call: the floors come from an integer
 * generator and every tone is placed at a bin computed by one division.
 *
 * HRP turns levels into dB with the libm's log10f, which is not one function: glibc 2.36's misrounds
 * some arguments by up to 2 ulp where 2.41 and later round them all correctly (BUILDING.md). This
 * program is linked with -Wl,--wrap=log10f and answers every such call with omx_log10f
 * (test/support), the correctly rounded log10f, so the digests are the same on every libm; it prints how many calls the
 * system's log10f would have answered differently. Where that count is zero (glibc 2.41 and later,
 * where the digests were written) the substitution changes nothing.
 *
 *   fbs      omx_fbs_push over 64 frames: a ring growing 2 dB a frame, a steady line, a line that
 *            appears and decays, over a noise floor; every result field of every frame
 *   hrp      detect, track, project, attribute and learn over 64 frames of a held G3, a D4 that
 *            enters at frame 16 with a loud fourth harmonic, and a noise floor; every voice, track
 *            and partial field, then every ripe baseline row
 *   correct  the family searches, the peaking bell over a grid of centre, Q and gain, a cascade of
 *            those bells over a noise block, the cut and score laws over a grid, and one ranked
 *            selection
 *
 * The rta, render and live digests cover the code that moved later (the engine kernels move). Their
 * stimulus is a ten-second take synthesised without the libm: a held A3 whose fourth harmonic
 * rings 10 dB hot one second in every two and a half after a four-second teaching stretch, an E4
 * from 6 to 8 s, and a noise floor. Their committed digests were written by
 * tools/analysis-engine-identity.sh, which builds this program with those three headers answered
 * by the engine's mix_dsp.h, hrp_render.h and hrp_lv2.h.
 *
 *   rta      the capture ring over uneven writes (fill count, gate, read back, a refused read, an
 *            oversized write) and the spectrum of every window of the take
 *   render   the learn pass, the rewind, the measure pass (every call's windows and voice set, the
 *            whole report and the learned rows), a live pass over a fresh model, and the refusals
 *   live     the live core driven by 256-frame quanta with the worker never late: every set it
 *            adopts and every output sample, engaged for eight seconds then bypassed, the slots,
 *            a worker a ring behind, and the not-ready identity
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/analysis/omx_fbs_detect.h>
#include <omxdsp/analysis/omx_hrp_attribute.h>
#include <omxdsp/analysis/omx_hrp_baseline.h>
#include <omxdsp/analysis/omx_hrp_correct.h>
#include <omxdsp/analysis/omx_hrp_lv2.h>
#include <omxdsp/analysis/omx_hrp_pitch.h>
#include <omxdsp/analysis/omx_hrp_render.h>
#include <omxdsp/analysis/omx_hrp_track.h>
#include <omxdsp/analysis/omx_rta.h>
#include <omxdsp/omx_units.h>

#include "../support/log10f_cr.h"
#include "analysis_rates.h"
#include "sha256.h"

/* ---- log10f, correctly rounded on every libm -------------------------------------------------- */

float __real_log10f(float x);
static unsigned long g_log10f_calls, g_log10f_differs;

float __wrap_log10f(float x) {
  const float cr = omx_log10f(x);
  const float sys = __real_log10f(x);
  g_log10f_calls++;
  if (memcmp(&cr, &sys, sizeof cr) != 0) g_log10f_differs++;
  return cr;
}

/* ---- the record ---------------------------------------------------------------------------- */

#define RECORD_BYTES (16u << 20)
static uint8_t g_rec[RECORD_BYTES];
static size_t g_len;
static int g_overflow;

static void put(const void *p, size_t n) {
  if (g_len + n > RECORD_BYTES) { g_overflow = 1; return; }
  memcpy(g_rec + g_len, p, n);
  g_len += n;
}
static void put_u32(uint32_t v) { put(&v, sizeof v); }
static void put_i32(int32_t v) { put(&v, sizeof v); }
static void put_u64(uint64_t v) { put(&v, sizeof v); }
static void put_f32(float v) { put(&v, sizeof v); }
static void put_f64(double v) { put(&v, sizeof v); }

/* ---- the stimulus -------------------------------------------------------------------------- */

static uint32_t g_lcg;
static uint32_t lcg_next(void) {
  g_lcg = g_lcg * 1664525u + 1013904223u;
  return g_lcg;
}
/* Uniform in [0, 1), from the top 24 bits. */
static double lcg_unit(void) { return (double)(lcg_next() >> 8) / 16777216.0; }

#define MAX_BINS (OMX_RTA_FFT_SIZE_MAX / 2)

static uint32_t bin_of(double hz, double bin_hz) { return (uint32_t)(hz / bin_hz + 0.5); }

/* ---- fbs ----------------------------------------------------------------------------------- */

static int32_t g_bin_slot[MAX_BINS];
static OmxFbsSlot g_slots[OMX_FBS_TRACK_SLOTS];
static uint32_t g_free_stack[OMX_FBS_TRACK_SLOTS];
static OmxFbsCandidate g_confirmed[OMX_FBS_TRACK_SLOTS], g_snapshot[OMX_FBS_TRACK_SLOTS];
static uint32_t g_active[OMX_FBS_TRACK_SLOTS];
static double g_dmag[MAX_BINS];

static void put_candidate(const OmxFbsCandidate *c) {
  put_u32(c->bin);
  put_f64(c->freq_hz);
  put_f64(c->depth_db);
  put_f64(c->q);
  put_u32(c->persistence);
  put_f64(c->level_db);
}

static void render_fbs(double rate, OmxFbsPool pool) {
  const uint32_t fft = omx_analysis_fft_for_rate(rate);
  const uint32_t nbins = fft / 2;
  const double bin_hz = rate / (double)fft;
  static OmxFbsDetector d;
  OmxFbsResult r;
  memset(&d, 0, sizeof d);
  memset(&r, 0, sizeof r);
  d.t = omx_fbs_thresholds_default();
  d.pool = pool;
  d.sample_rate = rate;
  d.fft_size = fft;
  d.bin_slot = g_bin_slot;
  d.nbins_cap = nbins;
  d.slots = g_slots;
  d.free_stack = g_free_stack;
  d.n_slots = OMX_FBS_TRACK_SLOTS;
  r.confirmed = g_confirmed;
  r.snapshot = g_snapshot;
  r.active = g_active;
  omx_fbs_init(&d);
  const uint32_t ring = bin_of(2500.0, bin_hz), line = bin_of(630.0, bin_hz), late = bin_of(7100.0, bin_hz);
  double ring_lin = 2e-4, late_lin = 0.05;
  g_lcg = 0x1234567u;
  for (uint32_t f = 0; f < 64u; f++) {
    for (uint32_t b = 0; b < nbins; b++) g_dmag[b] = 1e-5 * (0.5 + lcg_unit());
    g_dmag[ring] = ring_lin;
    g_dmag[line] = 0.02;
    if (f >= 20u) {
      g_dmag[late] = late_lin;
      late_lin *= 0.7943282347242815; /* -2 dB a frame */
    }
    if (f < 40u) ring_lin *= 1.2589254117941673; /* +2 dB a frame */
    omx_fbs_push(&d, g_dmag, nbins, &r);
    put_u32(r.n_confirmed);
    for (uint32_t i = 0; i < r.n_confirmed; i++) put_candidate(&r.confirmed[i]);
    put_u32(r.n_snapshot);
    for (uint32_t i = 0; i < r.n_snapshot; i++) put_candidate(&r.snapshot[i]);
    put_u32(r.n_active);
    for (uint32_t i = 0; i < r.n_active; i++) put_u32(r.active[i]);
    put_u32(r.untracked);
  }
  put_u64(d.frame);
  put_u64(d.seq);
  /* The two exported gates on their own. */
  double hist[OMX_GROWTH_WINDOW_FRAMES];
  for (uint32_t n = 0; n < OMX_GROWTH_WINDOW_FRAMES; n++) hist[n] = -60.0 + 1.5 * (double)n;
  for (uint32_t n = 1; n <= OMX_GROWTH_WINDOW_FRAMES; n++) put_i32(omx_fbs_is_growing(hist, n, &d.t));
  put_f64(omx_fbs_pshr(g_dmag, nbins, ring));
  put_f64(omx_fbs_pshr(g_dmag, nbins, line));
}

/* ---- hrp ----------------------------------------------------------------------------------- */

static float g_mag[MAX_BINS];
static float g_residual[MAX_BINS];

static void put_voice(const OmxHrpVoice *v) {
  put_f32(v->f0_hz);
  put_f32(v->confidence);
  put_f32(v->salience);
  put_f32(v->amplitude_db);
  put_i32(v->midi_note);
  put_f32(v->cents);
  put_i32(v->valid);
}

static void put_track(const OmxHrpTrack *t) {
  put_u32(t->id);
  put_f32(t->f0_hz);
  put_f32(t->confidence);
  put_f32(t->salience);
  put_f32(t->amplitude_db);
  put_i32(t->midi_note);
  put_f32(t->cents);
  put_u32(t->age_frames);
  put_u32(t->missing_frames);
  put_i32(t->stable);
  put_i32(t->active);
  put_u32(t->octave_frames);
  put_i32(t->octave_dir);
}

static void put_partial(const OmxHrpPartial *p) {
  put_u32(p->harmonic);
  put_f32(p->freq_hz);
  put_f32(p->level_db);
  put_f32(p->confidence);
  put_i32(p->ambiguous);
  put_i32(p->present);
}

static void place(double hz, double bin_hz, uint32_t nbins, float amp) {
  const uint32_t b = bin_of(hz, bin_hz);
  if (b > 0 && b < nbins) g_mag[b] += amp;
}

static void render_hrp(double rate) {
  const uint32_t fft = omx_analysis_fft_for_rate(rate);
  const uint32_t nbins = fft / 2;
  const double bin_hz = rate / (double)fft;
  OmxHrpConfig cfg = omx_hrp_config_default();
  OmxHrpTrackConfig tcfg = omx_hrp_track_config_default();
  OmxHrpBaselineConfig bcfg = omx_hrp_baseline_config_default();
  static OmxHrpTracker tracker;
  static OmxHrpBaseline base;
  omx_hrp_tracker_init(&tracker);
  omx_hrp_baseline_init(&base);
  g_lcg = 0x7654321u;
  for (uint32_t f = 0; f < 64u; f++) {
    for (uint32_t b = 0; b < nbins; b++) g_mag[b] = (float)(1e-4 * lcg_unit());
    for (uint32_t n = 1; n <= 10; n++) place(196.0 * (double)n, bin_hz, nbins, 0.5f / (float)n);
    if (f >= 16u)
      for (uint32_t n = 1; n <= 8; n++)
        place(293.66 * (double)n, bin_hz, nbins, (n == 4 ? 1.2f : 0.4f) / (float)n);
    OmxHrpVoiceSet det, tracked;
    omx_hrp_detect(g_mag, nbins, (float)bin_hz, &cfg, g_residual, &det);
    put_u32(det.voice_count);
    for (uint32_t i = 0; i < det.voice_count; i++) put_voice(&det.voices[i]);
    omx_hrp_track_update(&tracker, &det, &tcfg);
    for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS; k++) put_track(&tracker.tracks[k]);
    put_u32(tracker.next_id);
    const OmxHrpTrack *by_slot[OMX_HRP_MAX_VOICES];
    omx_hrp_project_voices(&tracker, &tracked, by_slot);
    put_u32(tracked.voice_count);
    for (uint32_t i = 0; i < tracked.voice_count; i++) put_voice(&tracked.voices[i]);
    OmxHrpVoiceHarmonics grid[OMX_HRP_MAX_VOICES];
    memset(grid, 0, sizeof grid);
    omx_hrp_attribute(g_mag, nbins, (float)bin_hz, &tracked, &cfg, grid);
    for (uint32_t i = 0; i < tracked.voice_count; i++) {
      put_u32(grid[i].count);
      put_u32(omx_hrp_unambiguous_count(&grid[i]));
      for (uint32_t k = 0; k < grid[i].count; k++) put_partial(&grid[i].partials[k]);
      if (!by_slot[i]->stable) continue;
      for (uint32_t k = 0; k < grid[i].count; k++) {
        const OmxHrpPartial *p = &grid[i].partials[k];
        if (p->present && !p->ambiguous)
          omx_hrp_baseline_learn(&base, by_slot[i]->midi_note, p->harmonic, p->level_db, &bcfg);
      }
    }
    put_u32(omx_hrp_track_count(&tracker));
  }
  put_u32(omx_hrp_baseline_ripe_rows(&base, &bcfg));
  for (int note = 0; note < OMX_HRP_BASELINE_NOTES; note++)
    for (uint32_t h = 1; h <= OMX_HRP_HARMONICS; h++) {
      float db = 0.0f;
      if (omx_hrp_baseline_read(&base, note, h, &bcfg, &db)) {
        put_i32(note);
        put_u32(h);
        put_f32(db);
      }
    }
  /* The pitch helpers on their own. */
  for (uint32_t n = 0; n < 64u; n++) {
    float cents = 0.0f;
    const float hz = 27.5f + 31.0f * (float)n;
    put_i32(omx_hrp_midi_note(hz, &cents));
    put_f32(cents);
    put_f32(omx_hrp_bin_peak(g_mag, nbins, (float)bin_hz, hz));
    put_f32(omx_hrp_comb_score(g_mag, nbins, (float)bin_hz, hz, OMX_HRP_HARMONICS));
    put_f32(omx_hrp_cents_between(hz, 440.0f));
  }
  for (uint32_t v = 0; v <= OMX_HRP_MAX_VOICES + 1; v++) put_f32(omx_hrp_min_confidence_for(v));
}

/* ---- correct ------------------------------------------------------------------------------- */

#define BLOCK_N 16384u
static float g_in[BLOCK_N], g_out[BLOCK_N];

static void put_config(const OmxHrpConfig *c) {
  put_f32(c->f0_min_hz);
  put_f32(c->f0_max_hz);
  put_u32(c->harmonics);
  put_u32(c->max_voices);
  put_f32(c->min_confidence);
  put_f32(c->subharmonic_accept);
  put_f32(c->cents_step);
}

static void render_correct(double rate) {
  for (int fam = -1; fam <= OMX_HRP_FAMILY_COUNT; fam++) {
    OmxHrpConfig c = omx_hrp_family_search(fam);
    put_config(&c);
  }
  static const float freqs[] = {20.0f, 63.0f, 250.0f, 1000.0f, 3150.0f, 8000.0f, 12500.0f, 15000.0f};
  static const float qs[] = {0.7f, 2.0f, (float)OMX_HRP_BAND_Q, 9.0f};
  static const float gains[] = {-6.0f, -3.0f, -0.5f, 0.0f, 2.0f};
  static OmxHrpCascade cas;
  omx_hrp_cascade_init(&cas);
  for (size_t i = 0; i < sizeof freqs / sizeof freqs[0]; i++)
    for (size_t j = 0; j < sizeof qs / sizeof qs[0]; j++)
      for (size_t k = 0; k < sizeof gains / sizeof gains[0]; k++) {
        float c[5];
        omx_hrp_peaking_coeffs(freqs[i], qs[j], gains[k], (uint32_t)rate, c);
        put(c, sizeof c);
        if (j == 2 && k < 3) put_i32(omx_hrp_cascade_add(&cas, c, (int)((i + k) % 3 != 0)));
      }
  put_u32(omx_hrp_cascade_live(&cas));
  g_lcg = 0x1234567u;
  for (uint32_t n = 0; n < BLOCK_N; n++) g_in[n] = (float)(lcg_unit() - 0.5);
  for (uint32_t o = 0; o < BLOCK_N; o += 128u) omx_hrp_cascade_apply(&cas, g_in + o, g_out + o, 128u);
  put(g_out, sizeof g_out);

  for (int e = -10; e <= 40; e++)
    for (int a = 0; a <= 4; a++) {
      const float cut = omx_hrp_cut_db(0.5f * (float)e, 0.25f * (float)a, (float)OMX_HRP_MAX_CUT_DB);
      put_f32(cut);
      put_f32(omx_hrp_resonance_score(cut, 0.1f * (float)a, 0.05f * (float)(e + 10)));
    }
  OmxHrpCandidate cand[OMX_HRP_MAX_VOICES * OMX_HRP_HARMONICS];
  uint32_t n = 0;
  g_lcg = 0x55aa55aau;
  for (int32_t v = 1; v <= OMX_HRP_MAX_VOICES; v++)
    for (uint32_t h = 1; h <= OMX_HRP_HARMONICS; h++) {
      OmxHrpCandidate *c = &cand[n++];
      c->freq_hz = (float)(110.0 * (double)v * (double)h * (0.99 + 0.02 * lcg_unit()));
      c->cut_db = (float)(OMX_HRP_MAX_CUT_DB * lcg_unit());
      c->score = (lcg_next() & 7u) == 0u ? 0.0f : (float)lcg_unit();
      c->voice_id = v;
      c->harmonic = h;
    }
  const uint32_t kept = omx_hrp_select(cand, n, OMX_HRP_ATTRIBUTION_CENTS, OMX_HRP_DEFAULT_MAX_AUTO_BANDS);
  put_u32(kept);
  for (uint32_t i = 0; i < kept; i++) {
    put_f32(cand[i].freq_hz);
    put_f32(cand[i].cut_db);
    put_f32(cand[i].score);
    put_i32(cand[i].voice_id);
    put_u32(cand[i].harmonic);
  }
  put_f32(omx_hrp_cents_apart(440.0f, 466.16f));
}

/* ---- the take: a held note whose fourth harmonic rings -------------------------------------- */

/* sin(2π·turns) without the libm: the turn is folded into [-1/4, 1/4] and the odd Taylor series to
 * x^13 run in double (error below 1e-9), so the take is the same bits on every libm. */
static double tone_sin(double turns) {
  double t = turns - (double)(int64_t)turns;
  if (t < 0.0) t += 1.0;
  if (t >= 0.5) return -tone_sin(t - 0.5);
  if (t > 0.25) t = 0.5 - t;
  const double x = 6.283185307179586 * t, x2 = x * x;
  double s = 1.0 / 6227020800.0;
  s = s * -x2 + 1.0 / 39916800.0;
  s = s * -x2 + 1.0 / 362880.0;
  s = s * -x2 + 1.0 / 5040.0;
  s = s * -x2 + 1.0 / 120.0;
  s = s * -x2 + 1.0 / 6.0;
  s = s * -x2 + 1.0;
  return x * s;
}

#define TAKE_SECONDS 10u
#define TAKE_MAX (192000u * TAKE_SECONDS)
static float g_take[TAKE_MAX];
static float g_wet[TAKE_MAX];

/* The ring's share of the fourth harmonic's gain, 0..1: steady for the first four seconds (the
 * model is taught), then ringing one second in every two and a half, ramped over 50 ms. */
static double take_ring(double t) {
  if (t < 4.0) return 0.0;
  double ph = t - 4.0;
  while (ph >= 2.5) ph -= 2.5;
  if (ph >= 1.0) return 0.0;
  if (ph < 0.05) return ph / 0.05;
  if (ph > 0.95) return (1.0 - ph) / 0.05;
  return 1.0;
}

/* A3 at 216.8 Hz, five harmonics, the fourth +10 dB while it rings; an E4 of three harmonics from
 * 6 to 8 s; a noise floor near -66 dBFS. */
static uint32_t synth_take(double rate) {
  static const double ladder[5] = {0.315, 0.14, 0.105, 0.07, 0.056};
  const double hot = 3.1622776601683795; /* +10 dB */
  const uint32_t frames = (uint32_t)rate * TAKE_SECONDS;
  g_lcg = 0x2468aceu;
  for (uint32_t i = 0; i < frames; i++) {
    const double t = (double)i / rate;
    const double ring = take_ring(t);
    double x = 0.0;
    for (uint32_t k = 1; k <= 5u; k++) {
      double amp = ladder[k - 1];
      if (k == 4u) amp *= 1.0 + ring * (hot - 1.0);
      x += amp * tone_sin(216.796875 * (double)k * t);
    }
    if (t >= 6.0 && t < 8.0)
      for (uint32_t k = 1; k <= 3u; k++) x += (0.12 / (double)k) * tone_sin(329.63 * (double)k * t);
    x += 1e-3 * (lcg_unit() - 0.5);
    g_take[i] = (float)x;
  }
  return frames;
}

static float g_carry[OMX_RTA_FFT_SIZE_MAX], g_wre[OMX_RTA_FFT_SIZE_MAX], g_wim[OMX_RTA_FFT_SIZE_MAX];
static float g_hop[OMX_RTA_FFT_SIZE_MAX];

/* ---- rta ----------------------------------------------------------------------------------- */

static struct omx_ring g_ring;
static float g_block[OMX_RTA_FFT_SIZE_MAX];

static void render_rta(double rate) {
  const uint32_t n = omx_rta_fft_size_for((uint32_t)rate);
  put_u32(n);
  const uint32_t frames = synth_take(rate);
  memset(&g_ring, 0, sizeof g_ring);
  omx_ring_reset(&g_ring);
  uint32_t off = 0;
  for (uint32_t q = 0; off < 4u * n; q++) {
    const uint32_t len = 61u + (q * 37u) % 500u;
    omx_ring_write(&g_ring, g_take + off, len);
    off += len;
    put_u32(omx_ring_filled(&g_ring));
    put_i32(omx_ring_has(&g_ring, n));
  }
  omx_ring_read_latest(&g_ring, g_block, n);
  put(g_block, (size_t)n * sizeof(float));
  omx_rta_spectrum(g_block, n, g_wre, g_wim, g_mag);
  put(g_mag, (size_t)(n / 2u) * sizeof(float));
  omx_ring_read_at(&g_ring, g_block, n, 1000u);
  omx_rta_spectrum(g_block, n, g_wre, g_wim, g_mag);
  put(g_mag, (size_t)(n / 2u) * sizeof(float));
  /* Past the capacity a read is refused: zeros. */
  omx_ring_read_at(&g_ring, g_block, n, OMX_RTA_RING_SIZE);
  put(g_block, (size_t)n * sizeof(float));
  /* An oversized write keeps only the tail. */
  omx_ring_write(&g_ring, g_take, frames < OMX_RTA_RING_SIZE + 777u ? frames : OMX_RTA_RING_SIZE + 777u);
  put_u32(omx_ring_filled(&g_ring));
  put_u32(atomic_load_explicit(&g_ring.pos, memory_order_relaxed));
  omx_ring_read_at(&g_ring, g_block, 64u, 5u);
  put(g_block, 64u * sizeof(float));
  /* Every window of the take, the spectrum the analysis engines read. */
  for (uint32_t s = 0; s + n <= frames; s += n) {
    omx_rta_spectrum(g_take + s, n, g_wre, g_wim, g_mag);
    put(g_mag, (size_t)(n / 2u) * sizeof(float));
  }
  for (uint32_t m = 2; m <= 16u; m *= 2) {
    omx_rta_spectrum(g_take, m, g_wre, g_wim, g_mag);
    put(g_mag, (size_t)(m / 2u) * sizeof(float));
  }
}

/* ---- render -------------------------------------------------------------------------------- */

static OmxHrpRender g_render;

static void put_render_config(const OmxHrpRenderConfig *c) {
  put_u32(c->rate);
  put_u32(c->fft_size);
  put_u32(c->hop);
  put_config(&c->pitch);
  put_f32(c->track.match_cents);
  put_f32(c->track.steady_cents);
  put_u32(c->track.promote_frames);
  put_u32(c->track.retire_frames);
  put_u32(c->track.octave_hold_frames);
  put_f32(c->baseline.step_db);
  put_u32(c->baseline.ripe_updates);
}

static void put_voice_set(const OmxHrpVoiceSet *s) {
  put_u32(s->voice_count);
  for (uint32_t i = 0; i < s->voice_count; i++) put_voice(&s->voices[i]);
}

static void put_report(const OmxHrpRenderReport *r) {
  put_u32(r->windows);
  put_u32(r->pitched_windows);
  for (int note = 0; note < OMX_HRP_BASELINE_NOTES; note++)
    for (uint32_t h = 0; h < OMX_HRP_HARMONICS; h++) {
      const OmxHrpRenderCell *c = &r->cell[note][h];
      if (c->frames == 0u) continue;
      put_i32(note);
      put_u32(h);
      put_f32(c->baseline_db);
      put_f32(c->sum_excess_db);
      put_f32(c->sum_confidence);
      put_f32(c->sum_voice_confidence);
      put_f32(c->sum_f0_hz);
      put_f32(c->peak_excess_db);
      put_u32(c->frames);
      put_u32(c->hot_frames);
      put_f32(omx_hrp_render_excess_db(c));
      put_f32(omx_hrp_render_duty(c));
      put_f32(omx_hrp_render_peak_excess_db(c));
      put_f32(omx_hrp_render_confidence(c));
      put_f32(omx_hrp_render_voice_confidence(c));
      put_f32(omx_hrp_render_f0_hz(c));
    }
}

/* Feed the take in uneven blocks, recording what each call analysed and the voice set it left. */
static void feed_take(uint32_t frames, int pass) {
  uint32_t off = 0;
  for (uint32_t q = 0; off < frames; q++) {
    uint32_t len = 1000u + (q * 613u) % 4000u;
    if (len > frames - off) len = frames - off;
    put_u32(omx_hrp_render_feed(&g_render, g_take + off, len, pass));
    put_voice_set(&g_render.voices);
    off += len;
  }
}

static void render_render(double rate) {
  const uint32_t frames = synth_take(rate);
  OmxHrpRenderConfig cfg = omx_hrp_render_config_default((uint32_t)rate);
  put_render_config(&cfg);
  cfg.pitch.f0_min_hz = 150.0f;
  cfg.pitch.f0_max_hz = 400.0f;
  put_i32(omx_hrp_render_init(&g_render, &cfg, g_carry, g_wre, g_wim, g_mag, g_residual));
  feed_take(frames, OMX_HRP_RENDER_LEARN);
  put_u32(g_render.learn_windows);
  put_u32(g_render.fill);
  omx_hrp_render_rewind(&g_render);
  put_u32(g_render.fill);
  put_voice_set(&g_render.voices);
  feed_take(frames, OMX_HRP_RENDER_MEASURE);
  put_report(&g_render.report);
  for (int note = 0; note < OMX_HRP_BASELINE_NOTES; note++)
    for (uint32_t h = 1; h <= OMX_HRP_HARMONICS; h++) {
      float db = 0.0f;
      if (omx_hrp_baseline_read(&g_render.baseline, note, h, &cfg.baseline, &db)) {
        put_i32(note);
        put_u32(h);
        put_f32(db);
      }
    }
  /* The one-read pass over a fresh model: measure first, then teach. */
  put_i32(omx_hrp_render_init(&g_render, &cfg, g_carry, g_wre, g_wim, g_mag, g_residual));
  feed_take(frames, OMX_HRP_RENDER_LIVE);
  put_report(&g_render.report);
  put_u32(g_render.learn_windows);
  omx_hrp_render_reset_report(&g_render);
  put_report(&g_render.report);
  /* A refused init is not ready, and feeding it analyses nothing. */
  OmxHrpRenderConfig bad = cfg;
  bad.fft_size = 3000u;
  put_i32(omx_hrp_render_init(&g_render, &bad, g_carry, g_wre, g_wim, g_mag, g_residual));
  bad = cfg;
  bad.hop = 0u;
  put_i32(omx_hrp_render_init(&g_render, &bad, g_carry, g_wre, g_wim, g_mag, g_residual));
  bad.hop = cfg.fft_size + 1u;
  put_i32(omx_hrp_render_init(&g_render, &bad, g_carry, g_wre, g_wim, g_mag, g_residual));
  put_i32(omx_hrp_render_init(&g_render, &cfg, g_carry, NULL, g_wim, g_mag, g_residual));
  put_u32(omx_hrp_render_feed(&g_render, g_take, frames, OMX_HRP_RENDER_LIVE));
}

/* ---- live ---------------------------------------------------------------------------------- */

static OmxHrpLv2 g_live;

static void put_set(const OmxHrpLv2Set *s) {
  put_u32(s->count);
  for (uint32_t b = 0; b < s->count && b < OMX_HRP_LV2_MAX_BANDS; b++) {
    put(s->coeffs[b], sizeof s->coeffs[b]);
    put_f32(s->freq_hz[b]);
    put_f32(s->cut_db[b]);
  }
}

/* Quanta through run(), a worker pass whenever one is due, the set adopted at once: a host's
 * schedule with the worker never late. Records every set and every output sample. */
static void drive_live(uint32_t from, uint32_t frames, int bypass) {
  for (uint32_t off = from; off < frames; off += 256u) {
    const uint32_t n = frames - off < 256u ? frames - off : 256u;
    omx_hrp_lv2_run(&g_live, g_take + off, g_wet + off, n);
    if (omx_hrp_lv2_due(&g_live, bypass)) {
      OmxHrpLv2Set set;
      omx_hrp_lv2_analyse(&g_live, bypass, &set);
      put_set(&set);
      omx_hrp_lv2_adopt(&g_live, &set);
      put_u32(omx_hrp_cascade_live(&g_live.live));
    }
  }
  put(g_wet + from, (size_t)(frames - from) * sizeof(float));
}

static void render_live(double rate) {
  const uint32_t frames = synth_take(rate);
  put_i32(omx_hrp_lv2_init(&g_live, (uint32_t)rate, g_carry, g_wre, g_wim, g_mag, g_residual, g_hop));
  put_u32(omx_hrp_lv2_hop(&g_live));
  omx_hrp_lv2_set_family(&g_live, OMX_HRP_FAMILY_STRINGS);
  put_config(&g_live.render.cfg.pitch);
  g_live.render.cfg.pitch.f0_min_hz = 150.0f;
  g_live.render.cfg.pitch.f0_max_hz = 400.0f;
  omx_hrp_lv2_set_amount(&g_live, 1.0f, OMX_HRP_LV2_MAX_BANDS);
  put_f32(g_live.amount);
  put_u32(g_live.max_bands);
  /* Engaged for the first eight seconds, then bypassed: the bands walk out. */
  const uint32_t engaged = (uint32_t)rate * 8u;
  drive_live(0, engaged, 0);
  drive_live(engaged, frames, 1);
  for (uint32_t i = 0; i < OMX_HRP_LV2_MAX_BANDS; i++) {
    put_i32(g_live.slot[i].voice_id);
    put_u32(g_live.slot[i].harmonic);
    put_f32(g_live.slot[i].freq_hz);
    put_f32(g_live.slot[i].applied_db);
  }
  put_u32(g_live.passes);
  put_report(&g_live.render.report);
  /* A worker that fell a ring behind skips to the newest hop. */
  omx_hrp_lv2_set_amount(&g_live, 0.7f, 3u);
  for (uint32_t off = 0; off < OMX_RTA_RING_SIZE + 3000u; off += 1024u)
    omx_hrp_lv2_run(&g_live, g_take + off % (frames - 1024u), g_wet, 1024u);
  OmxHrpLv2Set set;
  omx_hrp_lv2_analyse(&g_live, 0, &set);
  put_set(&set);
  put_u64(atomic_load_explicit(&g_live.written, memory_order_relaxed));
  put_u64(atomic_load_explicit(&g_live.consumed, memory_order_relaxed));
  /* An instance that is not ready is the identity. */
  put_i32(omx_hrp_lv2_init(&g_live, 0u, g_carry, g_wre, g_wim, g_mag, g_residual, g_hop));
  omx_hrp_lv2_run(&g_live, g_take, g_wet, 512u);
  put_i32(memcmp(g_take, g_wet, 512u * sizeof(float)) == 0);
  put_i32(omx_hrp_lv2_due(&g_live, 0));
}

/* ---- the driver ---------------------------------------------------------------------------- */

static void render_fbs_both(double rate) {
  render_fbs(rate, OMX_FBS_POOL_FIXED);
  render_fbs(rate, OMX_FBS_POOL_LIVE);
}

static const struct {
  const char *name;
  void (*render)(double rate);
} ENGINES[] = {{"fbs", render_fbs_both}, {"hrp", render_hrp}, {"correct", render_correct},
               {"rta", render_rta},    {"render", render_render}, {"live", render_live}};

int main(int argc, char **argv) {
  omx_analysis_require_rates();
  char probe[65];
  omx_sha256_hex("abc", 3, probe);
  if (strcmp(probe, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
    fprintf(stderr, "analysis_golden: the SHA-256 self-test failed (%s)\n", probe);
    return 2;
  }
  const int write = argc > 1 && strcmp(argv[1], "--write") == 0;
  const char *path = argc > 1 && !write ? argv[1] : "test/golden/analysis.sha256";
  FILE *f = write ? NULL : fopen(path, "r");
  if (!write && !f) {
    fprintf(stderr, "analysis_golden: cannot read %s\n", path);
    return 2;
  }
  int fail = 0, checked = 0;
  for (size_t e = 0; e < sizeof ENGINES / sizeof ENGINES[0]; e++) {
    for (int k = 0; k < OMX_ANALYSIS_RATE_COUNT; k++) {
      const double rate = OMX_ANALYSIS_RATES[k];
      g_len = 0;
      g_overflow = 0;
      ENGINES[e].render(rate);
      if (g_overflow) {
        fprintf(stderr, "analysis_golden: %s at %.0f Hz overflows the record\n", ENGINES[e].name, rate);
        return 2;
      }
      char hex[65];
      omx_sha256_hex(g_rec, g_len, hex);
      if (write) {
        printf("%s %.0f %s\n", ENGINES[e].name, rate, hex);
        continue;
      }
      char name[16] = {0}, want[65] = {0};
      double r = 0.0;
      int found = 0;
      rewind(f);
      while (fscanf(f, "%15s %lf %64s", name, &r, want) == 3)
        if (strcmp(name, ENGINES[e].name) == 0 && r == rate) { found = 1; break; }
      checked++;
      if (!found) {
        fprintf(stderr, "FAIL: %s at %.0f Hz has no golden digest\n", ENGINES[e].name, rate);
        fail++;
      } else if (strcmp(want, hex) != 0) {
        fprintf(stderr, "FAIL: %s at %.0f Hz moved: %s, golden %s (%zu bytes)\n", ENGINES[e].name, rate,
                hex, want, g_len);
        fail++;
      }
    }
  }
  if (f) fclose(f);
  if (g_log10f_calls == 0) {
    fprintf(stderr, "analysis_golden: log10f was never called through the wrap; link with -Wl,--wrap=log10f\n");
    return 2;
  }
  if (!write)
    printf("analysis_golden: %d digests, %d moved (the system's log10f differs on %lu of %lu calls)\n",
           checked, fail, g_log10f_differs, g_log10f_calls);
  return fail == 0 ? 0 : 1;
}
