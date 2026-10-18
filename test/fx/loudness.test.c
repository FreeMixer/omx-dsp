// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The loudness kernels' oracle (omx_loudness.h, FreeMixer/omx-dsp#64): EBU Tech 3341's and Tech
 * 3342's test signals, generated here from their closed forms, at each of the nine oracle rates,
 * held to the tolerances the documents give:
 *
 *   momentary, short-term, integrated   ±0.1 LU
 *   true peak                           +0.2 / −0.4 dB
 *   loudness range                      ±1 LU
 *
 * Generated: Tech 3341 signals 1 to 6, 9, 12 and 15 to 19, and Tech 3342 signals 1 to 4. In place
 * of the burst signals 10 and 13, whose waveforms the document leaves to its files, a burst of
 * exactly one window (3 s, 400 ms) at −23 dBFS is placed at offsets off the 10 ms grid and the
 * largest reading must be −23.0 LUFS ±0.1. Not generated: 3341 signals 7 and 8 and 3342 signals 5
 * and 6 (programme recordings), 3341 signals 11 and 14 (burst series given only as files) and
 * 3341 signals 20 to 23 (true-peak waveforms given only as files).
 *
 * The signals are 1 kHz sines unless named otherwise; "x dBFS" is the sine's peak, so a stereo
 * tone at x dBFS reads x LUFS. The tone keeps its phase across level steps, as the files do.
 *
 * Beside the conformance signals: the K-weighting at 48 kHz against the coefficients BS.1770-4
 * prints, the design's stability and DC null at every rate, block-size invariance bit for bit,
 * silence and the absolute gate, reset, the leg weights, no allocation on the process path and
 * N threads byte-identical to one. Contracts are compiled in; every rate's ledger must be clean
 * but for the rate-is-declared notes outside the declaration (there are none: the kernel reads no
 * declared list).
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_loudness.h>

#include "fx_rates.h"

static int g_fail = 0, g_checks = 0;
static float g_sr = 48000.0f;

/* A band, because a one-sided bound on a measured quantity catches only one of its two ways of
 * being wrong. */
static void in_band(double v, double lo, double hi, const char *what) {
  g_checks++;
  if (!(v >= lo && v <= hi)) {
    g_fail++;
    fprintf(stderr, "FAIL @ %.0f Hz: %s — measured %.6f, wanted [%.6f, %.6f]\n", (double)g_sr, what, v, lo, hi);
  }
}

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL @ %.0f Hz: %s\n", (double)g_sr, what);
  }
}

/* ---- the allocation counter (linked with --wrap=malloc,calloc,realloc,free) ---------------- */

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

static int g_counting = 0;
static long g_allocs = 0;

void *__wrap_malloc(size_t n) {
  if (g_counting) g_allocs++;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t sz) {
  if (g_counting) g_allocs++;
  return __real_calloc(n, sz);
}
void *__wrap_realloc(void *p, size_t n) {
  if (g_counting) g_allocs++;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (g_counting) g_allocs++;
  __real_free(p);
}

/* ---- the signals ---------------------------------------------------------------------------- */

#define SILENT (-1000.0)
#define MAXSEG 64
#define CH OMX_LOUDNESS_MAX_CHANNELS

/* One stretch of a signal: its length and each leg's peak level, dBFS (SILENT: nothing). */
struct seg {
  double secs;
  double db[CH];
};

/* A sine of `hz` (or, when `frac` > 0, of `frac`·rate) at phase `phase` rad, whose level steps
 * through `segs`, faded in over its first `fade` seconds (raised cosine) when `fade` > 0. */
struct signal {
  uint32_t channels;
  double hz, frac, phase, fade;
  uint32_t nseg;
  struct seg seg[MAXSEG];
};

/* What a run records: the readings after every closed 10 ms, from `from` seconds on. */
struct record {
  double from;
  double m_min, m_max, s_min, s_max;
};

static struct loudness_box { struct omx_loudness s; } g_box; /* one state, off the stack */

static float g_buf[CH][OMX_LOUDNESS_SUBBLOCKS_PER_S * 2000u];

static uint64_t seg_frames(double secs, float sr) { return (uint64_t)llround(secs * (double)sr); }

/* Render the signal into `s` in blocks of `block` frames (0: one 10 ms sub-block at a time),
 * reading momentary and short-term after every block that ends on a sub-block. */
static void run(struct omx_loudness *s, const struct signal *g, float sr, const float *weight, uint32_t block,
                struct record *rec) {
  omx_loudness_init(s, sr, g->channels, weight);
  const double f = g->frac > 0.0 ? g->frac * (double)sr : g->hz;
  const double w = 2.0 * M_PI * f / (double)sr;
  const uint32_t sub = s->sub_len;
  const uint64_t fade = seg_frames(g->fade, sr);
  if (rec) { rec->m_min = rec->s_min = HUGE_VAL; rec->m_max = rec->s_max = -HUGE_VAL; }
  uint64_t n = 0;
  for (uint32_t k = 0; k < g->nseg; k++) {
    const uint64_t end = n + seg_frames(g->seg[k].secs, sr);
    double amp[CH];
    for (uint32_t c = 0; c < g->channels; c++) amp[c] = g->seg[k].db[c] <= SILENT ? 0.0 : pow(10.0, g->seg[k].db[c] / 20.0);
    while (n < end) {
      const uint32_t want = block ? block : sub - s->fill; /* 10 ms at a time, on the sub-block grid */
      uint32_t len = (uint32_t)(end - n < want ? end - n : want);
      if (len > sizeof g_buf[0] / sizeof g_buf[0][0]) len = sizeof g_buf[0] / sizeof g_buf[0][0];
      for (uint32_t i = 0; i < len; i++) {
        double v = sin(w * (double)(n + i) + g->phase);
        if (n + i < fade) v *= 0.5 - 0.5 * cos(M_PI * (double)(n + i) / (double)fade);
        for (uint32_t c = 0; c < g->channels; c++) g_buf[c][i] = (float)(amp[c] * v);
      }
      const float *legs[CH];
      for (uint32_t c = 0; c < CH; c++) legs[c] = g_buf[c];
      omx_loudness_process(s, legs, len);
      n += len;
      if (rec && s->fill == 0u && (double)n >= rec->from * (double)sr) {
        const double m = omx_loudness_momentary(s), st = omx_loudness_short_term(s);
        if (m < rec->m_min) rec->m_min = m;
        if (m > rec->m_max) rec->m_max = m;
        if (st < rec->s_min) rec->s_min = st;
        if (st > rec->s_max) rec->s_max = st;
      }
    }
  }
}

static void stereo(struct signal *g, double hz) {
  memset(g, 0, sizeof *g);
  g->channels = 2u;
  g->hz = hz;
}

static void add(struct signal *g, double secs, double db) {
  struct seg *s = &g->seg[g->nseg++];
  s->secs = secs;
  for (uint32_t c = 0; c < CH; c++) s->db[c] = db;
}

/* ---- the K-weighting ------------------------------------------------------------------------ */

/* BS.1770-4 Table 1 and Table 2: the two sections at 48 kHz as the standard prints them. */
static void test_the_design_prints_the_standards_coefficients_at_48k(void) {
  static const double want[2][5] = {
      {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585},
      {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621},
  };
  double c[2][5];
  g_sr = 48000.0f;
  omx_loudness_kweight_design(48000.0, c);
  double worst = 0.0;
  for (int k = 0; k < 2; k++)
    for (int i = 0; i < 5; i++) {
      const double e = fabs(c[k][i] - want[k][i]);
      if (!(e <= worst)) worst = e;
    }
  /* the printed a-terms of the high-pass are float-rounded (…45483398, …25036621): 1e-8 covers them */
  in_band(worst, 0.0, 1e-8, "K-weighting at 48 kHz: BS.1770-4's printed coefficients");
}

/* The same filter at every rate: stable, a double zero at DC, and the shelf's +4 dB plateau. */
static void test_the_design_at_every_rate(float sr) {
  double c[2][5];
  omx_loudness_kweight_design((double)sr, c);
  check(omx_biquad_stable(c[0]) && omx_biquad_stable(c[1]), "both K-weighting sections are stable");
  in_band(c[1][0] + c[1][1] + c[1][2], 0.0, 0.0, "the RLB high-pass nulls DC exactly");
  /* |H| at 1 kHz: the shelf and the high-pass together lift a 1 kHz tone by 0.691 dB, which the
   * −0.691 offset takes back (a stereo 1 kHz tone at x dBFS reads x LUFS) */
  const double w = 2.0 * M_PI * 997.0 / (double)sr;
  double mag = 1.0;
  for (int k = 0; k < 2; k++) {
    const double br = c[k][0] + c[k][1] * cos(w) + c[k][2] * cos(2 * w), bi = -c[k][1] * sin(w) - c[k][2] * sin(2 * w);
    const double ar = 1.0 + c[k][3] * cos(w) + c[k][4] * cos(2 * w), ai = -c[k][3] * sin(w) - c[k][4] * sin(2 * w);
    mag *= sqrt((br * br + bi * bi) / (ar * ar + ai * ai));
  }
  /* the bilinear transform bends the shelf a little differently at each rate: 0.03 dB covers it */
  in_band(20.0 * log10(mag), 0.691 - 0.03, 0.691 + 0.03, "the K-weighting lifts 997 Hz by 0.691 dB");
}

/* ---- EBU Tech 3341 ---------------------------------------------------------------------------- */

#define TOL 0.1

/* 1, 2: a stereo tone, 20 s; M, S and I read its level. M and S are steady after 3 s. */
static void test_3341_steady(float sr, double db, const char *name) {
  struct signal g;
  stereo(&g, 1000.0);
  add(&g, 20.0, db);
  struct record r = {.from = 3.0};
  run(&g_box.s, &g, sr, NULL, 0u, &r);
  char what[160];
  snprintf(what, sizeof what, "%s: M stays at %.1f LUFS", name, db);
  in_band(r.m_min, db - TOL, db + TOL, what);
  in_band(r.m_max, db - TOL, db + TOL, what);
  snprintf(what, sizeof what, "%s: S stays at %.1f LUFS", name, db);
  in_band(r.s_min, db - TOL, db + TOL, what);
  in_band(r.s_max, db - TOL, db + TOL, what);
  snprintf(what, sizeof what, "%s: I reads %.1f LUFS", name, db);
  in_band(omx_loudness_integrated(&g_box.s), db - TOL, db + TOL, what);
}

static void test_3341_integrated(float sr, const struct signal *g, const float *weight, const char *name) {
  run(&g_box.s, g, sr, weight, 0u, NULL);
  char what[160];
  snprintf(what, sizeof what, "%s: I reads -23.0 LUFS", name);
  in_band(omx_loudness_integrated(&g_box.s), -23.0 - TOL, -23.0 + TOL, what);
}

static void test_3341_gating(float sr) {
  struct signal g;
  /* 3: −36 for 10 s, −23 for 60 s, −36 for 10 s: the quiet ends fall under the relative gate */
  stereo(&g, 1000.0);
  add(&g, 10.0, -36.0);
  add(&g, 60.0, -23.0);
  add(&g, 10.0, -36.0);
  test_3341_integrated(sr, &g, NULL, "3341 signal 3");
  /* 4: −72, −36, −23, −36, −72: the −72 stretches fall under the absolute gate too */
  stereo(&g, 1000.0);
  add(&g, 10.0, -72.0);
  add(&g, 10.0, -36.0);
  add(&g, 60.0, -23.0);
  add(&g, 10.0, -36.0);
  add(&g, 10.0, -72.0);
  test_3341_integrated(sr, &g, NULL, "3341 signal 4");
  /* 5: −26 for 20 s, −20 for 20.1 s, −26 for 20 s: nothing gated, the energy mean is −23 */
  stereo(&g, 1000.0);
  add(&g, 20.0, -26.0);
  add(&g, 20.1, -20.0);
  add(&g, 20.0, -26.0);
  test_3341_integrated(sr, &g, NULL, "3341 signal 5");
}

/* 6: five legs, L and R at −28, C at −24, Ls and Rs at −30 dBFS, the surrounds weighted 1.41. */
static void test_3341_surround(float sr) {
  struct signal g;
  memset(&g, 0, sizeof g);
  g.channels = 5u;
  g.hz = 1000.0;
  g.nseg = 1u;
  g.seg[0].secs = 20.0;
  const double db[5] = {-28.0, -28.0, -24.0, -30.0, -30.0};
  for (int c = 0; c < 5; c++) g.seg[0].db[c] = db[c];
  const float weight[5] = {1.0f, 1.0f, 1.0f, 1.41f, 1.41f};
  test_3341_integrated(sr, &g, weight, "3341 signal 6");
}

/* 9: 20 periods of 1.34 s at −20 and 1.66 s at −30 dBFS: every 3 s window holds one period, so S
 * reads −23.0 from 3 s on. 12: 25 periods of 0.18 s at −20 and 0.22 s at −30: every 400 ms window
 * holds one period, so M reads −23.0 from 1 s on. */
static void test_3341_windows(float sr) {
  struct signal g;
  stereo(&g, 1000.0);
  for (int k = 0; k < 20; k++) { add(&g, 1.34, -20.0); add(&g, 1.66, -30.0); }
  struct record r = {.from = 3.0};
  run(&g_box.s, &g, sr, NULL, 0u, &r);
  in_band(r.s_min, -23.0 - TOL, -23.0 + TOL, "3341 signal 9: S stays at -23.0 LUFS");
  in_band(r.s_max, -23.0 - TOL, -23.0 + TOL, "3341 signal 9: S stays at -23.0 LUFS");
  stereo(&g, 1000.0);
  for (int k = 0; k < 25; k++) { add(&g, 0.18, -20.0); add(&g, 0.22, -30.0); }
  r = (struct record){.from = 1.0};
  run(&g_box.s, &g, sr, NULL, 0u, &r);
  in_band(r.m_min, -23.0 - TOL, -23.0 + TOL, "3341 signal 12: M stays at -23.0 LUFS");
  in_band(r.m_max, -23.0 - TOL, -23.0 + TOL, "3341 signal 12: M stays at -23.0 LUFS");
}

/* In place of signals 10 and 13: one burst of exactly the window at −23 dBFS, after silence that
 * puts its start off the 10 ms grid, then silence; the largest reading is the burst's level. */
static void test_3341_bursts(float sr) {
  static const double offset_ms[] = {0.0, 1.3, 4.99, 5.0, 7.7, 9.99, 33.3, 61.0};
  for (size_t i = 0; i < sizeof offset_ms / sizeof offset_ms[0]; i++) {
    struct signal g;
    stereo(&g, 1000.0);
    add(&g, 1.0 + offset_ms[i] / 1000.0, SILENT);
    add(&g, 3.0, -23.0);
    add(&g, 1.0, SILENT);
    struct record r = {.from = 0.0};
    run(&g_box.s, &g, sr, NULL, 0u, &r);
    in_band(r.s_max, -23.0 - TOL, -23.0 + TOL, "a 3 s burst (signal 10): the largest S reads -23.0 LUFS");
    stereo(&g, 1000.0);
    add(&g, 1.0 + offset_ms[i] / 1000.0, SILENT);
    add(&g, 0.4, -23.0);
    add(&g, 1.0, SILENT);
    r = (struct record){.from = 0.0};
    run(&g_box.s, &g, sr, NULL, 0u, &r);
    in_band(r.m_max, -23.0 - TOL, -23.0 + TOL, "a 400 ms burst (signal 13): the largest M reads -23.0 LUFS");
  }
}

/* 15 to 19: true peak. A sine at a fraction of the rate whose phase puts the crest between samples;
 * 15 to 18 at half scale (−6.0 dBTP), 19 with every sample at ±1.0 (+3.0 dBTP). The tone is faded
 * in over 20 ms: a steady tone has no onset, and a sine switched on at full level is a step whose
 * reconstruction really does overshoot (by up to 0.7 dB at fs/8 through the ×4 interpolator). */
static void test_3341_true_peak(float sr) {
  static const struct { double frac, phase_deg, amp, want; const char *what; } tp[] = {
      {0.25, 0.0, 0.5, -6.0, "3341 signal 15: fs/4, 0 deg, reads -6.0 dBTP"},
      {0.25, 45.0, 0.5, -6.0, "3341 signal 16: fs/4, 45 deg, reads -6.0 dBTP"},
      {1.0 / 6.0, 60.0, 0.5, -6.0, "3341 signal 17: fs/6, 60 deg, reads -6.0 dBTP"},
      {0.125, 67.5, 0.5, -6.0, "3341 signal 18: fs/8, 67.5 deg, reads -6.0 dBTP"},
      {0.25, 45.0, M_SQRT2, 3.0, "3341 signal 19: fs/4, 45 deg, samples at full scale, reads +3.0 dBTP"},
  };
  for (size_t i = 0; i < sizeof tp / sizeof tp[0]; i++) {
    struct signal g;
    stereo(&g, 0.0);
    g.frac = tp[i].frac;
    g.phase = tp[i].phase_deg * M_PI / 180.0;
    g.fade = 0.02;
    add(&g, 1.0, 20.0 * log10(tp[i].amp));
    run(&g_box.s, &g, sr, NULL, 0u, NULL);
    in_band(omx_loudness_true_peak_db(&g_box.s), tp[i].want - 0.4, tp[i].want + 0.2, tp[i].what);
  }
}

/* ---- EBU Tech 3342 ---------------------------------------------------------------------------- */

static void test_3342_range(float sr) {
  static const struct { int n; double db[5]; double want; const char *what; } lra[] = {
      {2, {-20.0, -30.0}, 10.0, "3342 signal 1: -20, -30 dBFS, LRA 10 LU"},
      {2, {-20.0, -15.0}, 5.0, "3342 signal 2: -20, -15 dBFS, LRA 5 LU"},
      {2, {-40.0, -20.0}, 20.0, "3342 signal 3: -40, -20 dBFS, LRA 20 LU"},
      {5, {-50.0, -35.0, -20.0, -35.0, -50.0}, 15.0, "3342 signal 4: -50, -35, -20, -35, -50 dBFS, LRA 15 LU"},
  };
  for (size_t i = 0; i < sizeof lra / sizeof lra[0]; i++) {
    struct signal g;
    stereo(&g, 1000.0);
    for (int k = 0; k < lra[i].n; k++) add(&g, 20.0, lra[i].db[k]);
    run(&g_box.s, &g, sr, NULL, 0u, NULL);
    in_band(omx_loudness_range(&g_box.s), lra[i].want - 1.0, lra[i].want + 1.0, lra[i].what);
  }
}

/* ---- the kernel's own laws ------------------------------------------------------------------- */

/* Noise in blocks of 1, 7, 441, 4096 frames and in 10 ms sub-blocks: every reading bit for bit. */
static uint32_t g_lcg = 1u;
static float noise(void) {
  g_lcg = g_lcg * 1664525u + 1013904223u;
  return (float)(int32_t)(g_lcg >> 8) / 16777216.0f - 0.25f;
}

#define NOISE_S 5u
static float g_noise[2][192000u * NOISE_S];

static void readings(const struct omx_loudness *s, double out[7]) {
  out[0] = omx_loudness_momentary_ms(s);
  out[1] = omx_loudness_short_term_ms(s);
  out[2] = omx_loudness_integrated_ms(s);
  omx_loudness_range_ms(s, &out[3], &out[4]);
  out[5] = (double)omx_loudness_true_peak(s);
  out[6] = (double)s->subs;
}

static void feed(struct omx_loudness *s, float sr, uint32_t block) {
  const uint32_t n = (uint32_t)sr * NOISE_S;
  omx_loudness_init(s, sr, 2u, NULL);
  for (uint32_t done = 0; done < n;) {
    const uint32_t k = n - done < block ? n - done : block;
    const float *legs[2] = {g_noise[0] + done, g_noise[1] + done};
    omx_loudness_process(s, legs, k);
    done += k;
  }
}

static void test_block_size_invariance(float sr) {
  g_lcg = 0x1234567u;
  const uint32_t n = (uint32_t)sr * NOISE_S;
  for (uint32_t i = 0; i < n; i++) {
    const float v = noise();
    g_noise[0][i] = v * (i < n / 2 ? 0.5f : 0.05f);
    g_noise[1][i] = -v * 0.25f;
  }
  static const uint32_t blocks[] = {1u, 7u, 441u, 4096u};
  double ref[7], got[7];
  feed(&g_box.s, sr, (uint32_t)sr / 100u);
  readings(&g_box.s, ref);
  for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
    feed(&g_box.s, sr, blocks[b]);
    readings(&g_box.s, got);
    check(memcmp(ref, got, sizeof ref) == 0, "every reading is the same bit for bit whatever the block size");
  }
  check(ref[2] > 0.0 && ref[4] >= ref[3] && ref[3] > 0.0 && ref[5] > 0.0, "the noise run reads a level, a range and a peak");
}

/* Silence reads silence; a tone at −75 dBFS (under the absolute gate) has M and S but no I. */
static void test_silence_and_the_absolute_gate(float sr) {
  struct signal g;
  stereo(&g, 1000.0);
  add(&g, 5.0, SILENT);
  run(&g_box.s, &g, sr, NULL, 0u, NULL);
  check(omx_loudness_momentary(&g_box.s) == OMX_LOUDNESS_SILENCE, "silence: M reads silence");
  check(omx_loudness_short_term(&g_box.s) == OMX_LOUDNESS_SILENCE, "silence: S reads silence");
  check(omx_loudness_integrated(&g_box.s) == OMX_LOUDNESS_SILENCE, "silence: I reads silence");
  check(omx_loudness_range(&g_box.s) == 0.0, "silence: LRA reads 0");
  check(omx_loudness_true_peak_db(&g_box.s) == OMX_LOUDNESS_SILENCE, "silence: TP reads silence");
  stereo(&g, 1000.0);
  add(&g, 5.0, -75.0);
  run(&g_box.s, &g, sr, NULL, 0u, NULL);
  in_band(omx_loudness_momentary(&g_box.s), -75.0 - TOL, -75.0 + TOL, "a -75 dBFS tone: M reads -75 LUFS");
  check(omx_loudness_integrated(&g_box.s) == OMX_LOUDNESS_SILENCE, "a -75 dBFS tone: under the absolute gate, I reads silence");
  /* the momentary window not yet full counts the missing time as silence: 200 ms of a tone reads 3 dB down */
  stereo(&g, 1000.0);
  add(&g, 0.2, -23.0);
  run(&g_box.s, &g, sr, NULL, 0u, NULL);
  in_band(omx_loudness_momentary(&g_box.s), -26.01 - TOL, -26.01 + TOL, "200 ms into a -23 dBFS tone M reads half the energy");
}

/* reset: the same signal after a reset reads what a fresh state reads, bit for bit. */
static void test_reset(float sr) {
  double a[7], b[7];
  feed(&g_box.s, sr, 1024u);
  readings(&g_box.s, a);
  omx_loudness_reset(&g_box.s);
  check(g_box.s.subs == 0u && omx_loudness_integrated_ms(&g_box.s) == 0.0 && omx_loudness_true_peak(&g_box.s) == 0.0f,
        "reset clears every window, histogram and peak");
  const uint32_t n = (uint32_t)sr * NOISE_S;
  for (uint32_t done = 0; done < n;) {
    const uint32_t k = n - done < 1024u ? n - done : 1024u;
    const float *legs[2] = {g_noise[0] + done, g_noise[1] + done};
    omx_loudness_process(&g_box.s, legs, k);
    done += k;
  }
  readings(&g_box.s, b);
  check(memcmp(a, b, sizeof a) == 0, "a reset state reads what a fresh one reads, bit for bit");
}

/* A leg weighted 0 adds nothing (the LFE); a NULL leg is skipped. */
static void test_leg_weights(float sr) {
  struct signal g;
  memset(&g, 0, sizeof g);
  g.channels = 3u;
  g.hz = 1000.0;
  g.nseg = 1u;
  g.seg[0].secs = 5.0;
  g.seg[0].db[0] = -23.0;
  g.seg[0].db[1] = -23.0;
  g.seg[0].db[2] = 0.0;
  const float weight[3] = {1.0f, 1.0f, 0.0f};
  run(&g_box.s, &g, sr, weight, 0u, NULL);
  in_band(omx_loudness_integrated(&g_box.s), -23.0 - TOL, -23.0 + TOL, "a leg weighted 0 adds nothing to the loudness");
  in_band(omx_loudness_true_peak_db(&g_box.s), -0.4, 0.2, "a leg weighted 0 still reads its true peak");
}

/* ---- no allocation, N threads ---------------------------------------------------------------- */

static void test_no_allocation(float sr) {
  g_allocs = 0;
  g_counting = 1;
  feed(&g_box.s, sr, 777u);
  double r[7];
  readings(&g_box.s, r);
  (void)omx_loudness_range(&g_box.s);
  g_counting = 0;
  check(g_allocs == 0, "init, process and every reading allocate nothing");
}

#define THREADS 4
struct worker {
  float sr;
  struct omx_loudness s;
  double out[7];
};

static void *worker_main(void *arg) {
  struct worker *w = arg;
  const uint32_t n = (uint32_t)w->sr * NOISE_S;
  omx_loudness_init(&w->s, w->sr, 2u, NULL);
  for (uint32_t done = 0; done < n;) {
    const uint32_t k = n - done < 512u ? n - done : 512u;
    const float *legs[2] = {g_noise[0] + done, g_noise[1] + done};
    omx_loudness_process(&w->s, legs, k);
    done += k;
  }
  readings(&w->s, w->out);
  return NULL;
}

static void test_threads(float sr) {
  static struct worker ref, ws[THREADS];
  ref.sr = sr;
  worker_main(&ref);
  pthread_t t[THREADS];
  for (int i = 0; i < THREADS; i++) {
    ws[i].sr = sr;
    if (pthread_create(&t[i], NULL, worker_main, &ws[i]) != 0) abort();
  }
  for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
  for (int i = 0; i < THREADS; i++) check(memcmp(ws[i].out, ref.out, sizeof ref.out) == 0, "every thread's readings equal one thread's");
}

/* With a rate as its argument the oracle runs that rate alone (loudness-perturb.sh's quick arm). */
int main(int argc, char **argv) {
  omx_fx_require_rate_floor();
  const float only = argc > 1 ? (float)atof(argv[1]) : 0.0f;
  check(omx_loudness_state_size() == sizeof(struct omx_loudness), "the state layout is exported");
  test_the_design_prints_the_standards_coefficients_at_48k();
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    if (only > 0.0f && g_sr != only) continue;
    test_the_design_at_every_rate(g_sr);
    test_3341_steady(g_sr, -23.0, "3341 signal 1");
    test_3341_steady(g_sr, -33.0, "3341 signal 2");
    test_3341_gating(g_sr);
    test_3341_surround(g_sr);
    test_3341_windows(g_sr);
    test_3341_bursts(g_sr);
    test_3341_true_peak(g_sr);
    test_3342_range(g_sr);
    test_block_size_invariance(g_sr);
    test_silence_and_the_absolute_gate(g_sr);
    test_reset(g_sr);
    test_leg_weights(g_sr);
    test_no_allocation(g_sr);
    test_threads(g_sr);
    const uint32_t v = omx_fx_drain_ledger(g_sr);
    check(v == 0u, "the contract ledger came out empty at this rate");
  }
  printf("fx/loudness: %d checks, %d failures, %s\n", g_checks, g_fail, only > 0.0f ? "one rate" : "nine oracle rates");
  return g_fail == 0 ? 0 : 1;
}
