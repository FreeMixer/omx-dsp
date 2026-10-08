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
 * The committed digests were written by this program compiled against openmixer's in-tree
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
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/analysis/omx_fbs_detect.h>
#include <omxdsp/analysis/omx_hrp_attribute.h>
#include <omxdsp/analysis/omx_hrp_baseline.h>
#include <omxdsp/analysis/omx_hrp_correct.h>
#include <omxdsp/analysis/omx_hrp_pitch.h>
#include <omxdsp/analysis/omx_hrp_track.h>
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

#define RECORD_BYTES (8u << 20)
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

/* ---- the driver ---------------------------------------------------------------------------- */

static void render_fbs_both(double rate) {
  render_fbs(rate, OMX_FBS_POOL_FIXED);
  render_fbs(rate, OMX_FBS_POOL_LIVE);
}

static const struct {
  const char *name;
  void (*render)(double rate);
} ENGINES[] = {{"fbs", render_fbs_both}, {"hrp", render_hrp}, {"correct", render_correct}};

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
