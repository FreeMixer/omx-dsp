// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * rates.test.c — the HRP chain at the nine RME rates, and the no-allocation arm of both engines.
 *
 * The ported HRP oracles run at 48 kHz with a 4096-point frame. Here every rate builds its
 * spectra on the RTA's own grid for that rate (analysis_rates.h), so a bin width the detector
 * mishandles at 32 or 192 kHz shows. Each spectrum is placed by hand, one partial in its nearest
 * bin, so the expected answers follow from the placement:
 *
 *   1. a 440 Hz sine is one voice at 440 Hz, MIDI 69;
 *   2. a ten-partial series on 220 Hz reads 220 Hz, and with its fundamental removed still 220 Hz;
 *   3. A4 + C5 are both found;
 *   4. attribution measures each partial of an even series at the level placed there;
 *   5. the tracker promotes a held note to stable, and the baseline learns its partials' levels;
 *   6. the peaking bell equals the matched design recomputed here in double, to float32's seven
 *      figures, and that design's magnitude at the centre is the gain asked for, to 0.001 dB;
 *   7. the cascade takes the bell's closed-form |H| off a sine at the centre, to 0.05 dB.
 *
 * The no-allocation arm wraps malloc, calloc, realloc and free at link time and counts every call
 * made while the detectors run, from omx_fbs_init to the last omx_hrp_select. The count must be
 * zero: neither engine allocates, whatever the rate.
 */
#include <math.h>
#include <stddef.h>
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

#include "analysis_rates.h"

/* ---- the allocation counter ------------------------------------------------------------ */

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t s);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

static int g_counting = 0;
static long g_allocs = 0;

void *__wrap_malloc(size_t n) {
  if (g_counting) g_allocs++;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t s) {
  if (g_counting) g_allocs++;
  return __real_calloc(n, s);
}
void *__wrap_realloc(void *p, size_t n) {
  if (g_counting) g_allocs++;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (g_counting) g_allocs++;
  __real_free(p);
}

/* ---- checks ------------------------------------------------------------------------------ */

static int g_fail = 0;
static int g_checks = 0;
static double g_rate = 0.0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL @%.0f Hz: %s\n", g_rate, what);
  }
}

static void close_to(double got, double want, double tol, const char *what) {
  g_checks++;
  const double d = fabs(got - want);
  if (!(d <= tol)) {
    g_fail++;
    fprintf(stderr, "FAIL @%.0f Hz: %s (got %.9g, want %.9g, |d| %.3g > %.3g)\n", g_rate, what, got,
            want, d, tol);
  }
}

static float cents_off(float got_hz, float want_hz) {
  return fabsf(1200.0f * log2f(got_hz / want_hz));
}

static int found(const OmxHrpVoiceSet *v, float hz, float tolerance_cents) {
  for (uint32_t i = 0; i < v->voice_count; i++)
    if (v->voices[i].f0_hz > 0.0f && cents_off(v->voices[i].f0_hz, hz) <= tolerance_cents) return 1;
  return 0;
}

/* ---- spectra on the rate's own grid ------------------------------------------------------ */

#define MAX_BINS (OMX_RTA_FFT_SIZE_MAX / 2)

static float g_mag[MAX_BINS];
static float g_residual[MAX_BINS];
static uint32_t g_nbins;
static float g_bin_hz;

static void grid_for(double rate) {
  const uint32_t fft = omx_analysis_fft_for_rate(rate);
  g_nbins = fft / 2;
  g_bin_hz = (float)(rate / (double)fft);
}

static void clear(void) { memset(g_mag, 0, sizeof(g_mag)); }

static void place(float hz, float amp) {
  const int b = (int)(hz / g_bin_hz + 0.5f);
  if (b > 0 && (uint32_t)b < g_nbins) g_mag[b] = amp;
}

static void place_series(float f0, uint32_t first, uint32_t last, float amp) {
  for (uint32_t n = first; n <= last; n++) place((float)n * f0, amp / (float)n);
}

static void place_even(float f0, uint32_t last, float amp) {
  for (uint32_t n = 1; n <= last; n++) place((float)n * f0, amp);
}

static OmxHrpVoiceSet detect(void) {
  OmxHrpConfig cfg = omx_hrp_config_default();
  OmxHrpVoiceSet v;
  omx_hrp_detect(g_mag, g_nbins, g_bin_hz, &cfg, g_residual, &v);
  return v;
}

/* ---- 1-3: detection ---------------------------------------------------------------------- */

static void case_detection(void) {
  clear();
  place(440.0f, 1.0f);
  OmxHrpVoiceSet v = detect();
  check(v.voice_count == 1, "a pure sine is exactly one voice");
  check(found(&v, 440.0f, 40.0f), "a 440 Hz sine is heard at 440 Hz, not 220");
  check(v.voice_count >= 1 && v.voices[0].midi_note == 69, "440 Hz is MIDI 69");

  clear();
  place_series(220.0f, 1, 10, 1.0f);
  v = detect();
  check(v.voice_count >= 1 && cents_off(v.voices[0].f0_hz, 220.0f) <= 40.0f,
        "a ten-partial series reads its fundamental");

  clear();
  place_series(220.0f, 2, 8, 1.0f);
  v = detect();
  check(v.voice_count >= 1 && cents_off(v.voices[0].f0_hz, 220.0f) <= 40.0f,
        "a missing fundamental is recovered as 220 Hz, not read as 440 Hz");

  clear();
  place_series(440.0f, 1, 8, 1.0f);
  place_series(523.25f, 1, 8, 1.0f);
  v = detect();
  check(v.voice_count >= 2, "A4 + C5 is at least two voices");
  check(found(&v, 440.0f, 40.0f), "A4 found in the dyad");
  check(found(&v, 523.25f, 40.0f), "C5 found in the dyad");
}

/* ---- 4: attribution ---------------------------------------------------------------------- */

static void case_attribution(void) {
  /* Even partials: each one clears the presence yardstick (subharmonic_accept times the
   * twelve-slot mean), so all ten must be present. */
  clear();
  place_even(220.0f, 10, 0.5f);
  OmxHrpVoiceSet v = detect();
  check(v.voice_count == 1, "the series is one voice before attribution");
  if (v.voice_count != 1) return;
  OmxHrpConfig cfg = omx_hrp_config_default();
  OmxHrpVoiceHarmonics grid[OMX_HRP_MAX_VOICES];
  memset(grid, 0, sizeof(grid));
  omx_hrp_attribute(g_mag, g_nbins, g_bin_hz, &v, &cfg, grid);
  check(grid[0].count >= 10, "every placed partial lies on the grid below Nyquist");
  for (uint32_t n = 1; n <= 10; n++) {
    const OmxHrpPartial *p = omx_hrp_partial_of(&grid[0], n);
    check(p != NULL && p->present && !p->ambiguous, "a placed partial is present and unambiguous");
    if (p) close_to(p->level_db, 20.0 * log10(0.5), 0.01, "a partial is measured at its placed level");
  }
}

/* ---- 5: tracking and the baseline -------------------------------------------------------- */

static void case_track_and_baseline(void) {
  clear();
  place_series(220.0f, 1, 6, 0.5f);
  OmxHrpConfig cfg = omx_hrp_config_default();
  OmxHrpTrackConfig tcfg = omx_hrp_track_config_default();
  OmxHrpBaselineConfig bcfg = omx_hrp_baseline_config_default();
  static OmxHrpTracker tracker;
  static OmxHrpBaseline base;
  omx_hrp_tracker_init(&tracker);
  omx_hrp_baseline_init(&base);
  const uint32_t frames = tcfg.promote_frames + (uint32_t)bcfg.ripe_updates + 4u;
  int stable_seen = 0;
  for (uint32_t f = 0; f < frames; f++) {
    OmxHrpVoiceSet d, tracked;
    omx_hrp_detect(g_mag, g_nbins, g_bin_hz, &cfg, g_residual, &d);
    omx_hrp_track_update(&tracker, &d, &tcfg);
    const OmxHrpTrack *by_slot[OMX_HRP_MAX_VOICES];
    omx_hrp_project_voices(&tracker, &tracked, by_slot);
    OmxHrpVoiceHarmonics grid[OMX_HRP_MAX_VOICES];
    memset(grid, 0, sizeof(grid));
    omx_hrp_attribute(g_mag, g_nbins, g_bin_hz, &tracked, &cfg, grid);
    for (uint32_t i = 0; i < tracked.voice_count; i++) {
      if (!by_slot[i]->stable) continue;
      stable_seen = 1;
      for (uint32_t k = 0; k < grid[i].count; k++) {
        const OmxHrpPartial *p = &grid[i].partials[k];
        if (p->present && !p->ambiguous)
          omx_hrp_baseline_learn(&base, by_slot[i]->midi_note, p->harmonic, p->level_db, &bcfg);
      }
    }
  }
  check(omx_hrp_track_count(&tracker) == 1, "a held note is one track");
  check(stable_seen, "a held note is promoted to stable");
  float h3 = 0.0f;
  check(omx_hrp_baseline_read(&base, 57, 3, &bcfg, &h3), "the held note's H3 row ripens");
  close_to(h3, 20.0 * log10(0.5 / 3.0), (double)bcfg.step_db, "the baseline learns H3's level");
}

/* ---- 6-7: the bell and the cascade ------------------------------------------------------- */

/* The matched peaking section in double, from the mathematics (hrp_correct.test.c's oracle). */
static void matched_pair_oracle(double wn, double zeta, double out[3]) {
  const double w = wn > M_PI ? M_PI : wn;
  if (zeta < 1.0) {
    const double e = exp(-zeta * w), th = sqrt(1.0 - zeta * zeta) * w;
    const double ec = e * cos(th), es = e * sin(th);
    out[0] = -2.0 * ec;
    out[1] = e * e;
    out[2] = (1.0 - ec) * (1.0 - ec) + es * es;
  } else {
    const double sq = sqrt(zeta * zeta - 1.0);
    const double z1 = exp(-w / (zeta + sq)), z2 = exp(-w * (zeta + sq));
    out[0] = -(z1 + z2);
    out[1] = z1 * z2;
    out[2] = (1.0 - z1) * (1.0 - z2);
  }
}

static void matched_peaking(double f, double q, double g, double rate, double out[5]) {
  const double nyq = rate * 0.5;
  double f0 = f < 1.0 ? 1.0 : f;
  if (f0 > nyq * 0.999) f0 = nyq * 0.999;
  const double qq = q <= 0.0 ? 1e-3 : q;
  const double w0 = 2.0 * M_PI * f0 / rate;
  const double A = pow(10.0, g / 40.0);
  double p[3], z[3];
  matched_pair_oracle(w0, 1.0 / (2.0 * A * qq), p);
  matched_pair_oracle(w0, A / (2.0 * qq), z);
  const double k = p[2] / z[2];
  out[0] = k;
  out[1] = k * z[0];
  out[2] = k * z[1];
  out[3] = p[0];
  out[4] = p[1];
}

static double biquad_mag_d(const double c[5], double f_hz, double rate) {
  const double w = 2.0 * M_PI * f_hz / rate;
  const double cw = cos(w), sw = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  const double nr = c[0] + c[1] * cw + c[2] * c2, ni = -(c[1] * sw + c[2] * s2);
  const double dr = 1.0 + c[3] * cw + c[4] * c2, di = -(c[3] * sw + c[4] * s2);
  return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

static double biquad_mag(const float c[5], double f_hz, double rate) {
  const double w = 2.0 * M_PI * f_hz / rate;
  const double cw = cos(w), sw = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  const double nr = c[0] + c[1] * cw + c[2] * c2, ni = -(c[1] * sw + c[2] * s2);
  const double dr = 1.0 + c[3] * cw + c[4] * c2, di = -(c[3] * sw + c[4] * s2);
  return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

static void case_bell(double rate) {
  static const float freqs[] = {40.0f, 250.0f, 1000.0f, 4000.0f, 10000.0f};
  static const float gains[] = {-12.0f, -6.0f, -1.5f, 0.0f};
  for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
    /* The matched map cannot reach a bell's full depth near Nyquist; stay a decade below it. */
    if ((double)freqs[i] > rate / 20.0) continue;
    for (size_t j = 0; j < sizeof(gains) / sizeof(gains[0]); j++) {
      float got[5];
      double want[5];
      omx_hrp_peaking_coeffs(freqs[i], OMX_HRP_BAND_Q, gains[j], (uint32_t)rate, got);
      matched_peaking(freqs[i], OMX_HRP_BAND_Q, gains[j], rate, want);
      for (int k = 0; k < 5; k++)
        close_to(got[k], want[k], 1e-6 * (fabs(want[k]) + 1.0), "the bell is the matched design");
      close_to(20.0 * log10(biquad_mag_d(want, freqs[i], rate)), gains[j], 0.001,
               "the matched design's magnitude at its centre is the gain asked for");
    }
  }
}

static double rms(const float *x, uint32_t n) {
  double s = 0.0;
  for (uint32_t i = 0; i < n; i++) s += (double)x[i] * (double)x[i];
  return sqrt(s / (double)n);
}

#define CASCADE_N 65536u
static float g_in[CASCADE_N], g_out[CASCADE_N];

static void case_cascade(double rate) {
  for (uint32_t i = 0; i < CASCADE_N; i++)
    g_in[i] = (float)sin(2.0 * M_PI * 1000.0 * (double)i / rate);
  float c[5];
  omx_hrp_peaking_coeffs(1000.0f, OMX_HRP_BAND_Q, -6.0f, (uint32_t)rate, c);
  static OmxHrpCascade cas;
  omx_hrp_cascade_init(&cas);
  check(omx_hrp_cascade_add(&cas, c, 1) == 1, "the bell is added");
  for (uint32_t o = 0; o < CASCADE_N; o += 256u) omx_hrp_cascade_apply(&cas, g_in + o, g_out + o, 256u);
  const uint32_t skip = CASCADE_N / 2u;
  close_to(20.0 * log10(rms(g_out + skip, CASCADE_N - skip) / rms(g_in + skip, CASCADE_N - skip)),
           20.0 * log10(biquad_mag(c, 1000.0, rate)), 0.05,
           "the cascade takes the bell's closed-form |H| off a sine at its centre");
}

/* ---- the no-allocation arm --------------------------------------------------------------- */

static int32_t g_bin_slot[MAX_BINS];
static OmxFbsSlot g_slots[OMX_FBS_TRACK_SLOTS];
static uint32_t g_free_stack[OMX_FBS_TRACK_SLOTS];
static OmxFbsCandidate g_confirmed[OMX_FBS_TRACK_SLOTS], g_snapshot[OMX_FBS_TRACK_SLOTS];
static uint32_t g_active[OMX_FBS_TRACK_SLOTS];
static double g_dmag[MAX_BINS];

static void case_no_allocation(double rate) {
  const uint32_t fft = omx_analysis_fft_for_rate(rate);
  static OmxFbsDetector d;
  static OmxHrpTracker tracker;
  static OmxHrpBaseline base;
  OmxFbsResult r;
  memset(&d, 0, sizeof(d));
  memset(&r, 0, sizeof(r));
  d.t = omx_fbs_thresholds_default();
  d.pool = OMX_FBS_POOL_FIXED;
  d.sample_rate = rate;
  d.fft_size = fft;
  d.bin_slot = g_bin_slot;
  d.nbins_cap = g_nbins;
  d.slots = g_slots;
  d.free_stack = g_free_stack;
  d.n_slots = OMX_FBS_TRACK_SLOTS;
  r.confirmed = g_confirmed;
  r.snapshot = g_snapshot;
  r.active = g_active;
  clear();
  place_series(220.0f, 1, 8, 0.5f);
  place_series(330.0f, 1, 8, 0.25f);
  OmxHrpConfig cfg = omx_hrp_family_search(OMX_HRP_FAMILY_VOCALS);
  OmxHrpTrackConfig tcfg = omx_hrp_track_config_default();
  OmxHrpBaselineConfig bcfg = omx_hrp_baseline_config_default();
  const uint32_t ring = (uint32_t)(2500.0 / (rate / (double)fft));

  g_allocs = 0;
  g_counting = 1;
  omx_fbs_init(&d);
  omx_hrp_tracker_init(&tracker);
  omx_hrp_baseline_init(&base);
  double level = 1e-3;
  uint32_t confirmed = 0;
  for (uint32_t f = 0; f < 24u; f++) {
    for (uint32_t b = 0; b < g_nbins; b++) g_dmag[b] = 1e-5;
    g_dmag[ring] = level;
    level *= 1.4125375446227544; /* +3 dB a frame: a ring growing out of the floor */
    omx_fbs_push(&d, g_dmag, g_nbins, &r);
    confirmed += r.n_confirmed;

    OmxHrpVoiceSet det, tracked;
    omx_hrp_detect(g_mag, g_nbins, g_bin_hz, &cfg, g_residual, &det);
    omx_hrp_track_update(&tracker, &det, &tcfg);
    const OmxHrpTrack *by_slot[OMX_HRP_MAX_VOICES];
    omx_hrp_project_voices(&tracker, &tracked, by_slot);
    OmxHrpVoiceHarmonics grid[OMX_HRP_MAX_VOICES];
    memset(grid, 0, sizeof(grid));
    omx_hrp_attribute(g_mag, g_nbins, g_bin_hz, &tracked, &cfg, grid);
    OmxHrpCandidate cand[OMX_HRP_MAX_VOICES * OMX_HRP_HARMONICS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < tracked.voice_count; i++) {
      for (uint32_t k = 0; k < grid[i].count; k++) {
        const OmxHrpPartial *p = &grid[i].partials[k];
        if (by_slot[i]->stable && p->present && !p->ambiguous)
          omx_hrp_baseline_learn(&base, by_slot[i]->midi_note, p->harmonic, p->level_db, &bcfg);
        const float cut = omx_hrp_cut_db(p->level_db + 20.0f, OMX_HRP_DEFAULT_AMOUNT, OMX_HRP_MAX_CUT_DB);
        cand[n].freq_hz = p->freq_hz;
        cand[n].cut_db = cut;
        cand[n].score = omx_hrp_resonance_score(cut, tracked.voices[i].confidence, p->confidence);
        cand[n].voice_id = (int32_t)by_slot[i]->id;
        cand[n].harmonic = p->harmonic;
        n++;
      }
    }
    omx_hrp_select(cand, n, OMX_HRP_ATTRIBUTION_CENTS, OMX_HRP_DEFAULT_MAX_AUTO_BANDS);
  }
  g_counting = 0;
  check(confirmed >= 1, "the growing ring is confirmed (the arm ran the detector for real)");
  check(omx_hrp_track_count(&tracker) >= 1, "the HRP chain followed a voice (the arm ran it for real)");
  check(g_allocs == 0, "neither engine allocates while it analyses");
}

int main(void) {
  omx_analysis_require_rates();
  for (int k = 0; k < OMX_ANALYSIS_RATE_COUNT; k++) {
    g_rate = OMX_ANALYSIS_RATES[k];
    grid_for(g_rate);
    case_detection();
    case_attribution();
    case_track_and_baseline();
    case_bell(g_rate);
    case_cascade(g_rate);
    case_no_allocation(g_rate);
  }
  /* The counter's positive control: a malloc made while counting is seen. */
  void *(*volatile alloc)(size_t) = malloc;
  void (*volatile release)(void *) = free;
  g_allocs = 0;
  g_counting = 1;
  void *p = alloc(16);
  g_counting = 0;
  release(p);
  g_rate = 0.0;
  check(g_allocs == 1, "the allocation counter sees a malloc (positive control)");
  printf("analysis rates: %d checks over %d rates, %d failed\n", g_checks, OMX_ANALYSIS_RATE_COUNT, g_fail);
  return g_fail ? 1 : 0;
}
