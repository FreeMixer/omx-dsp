// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * hrp_lv2_rates.test.c — the live HRP shell (omx_hrp_lv2.h) at the nine RME rates.
 *
 * hrp_lv2.test.c holds the shell's behaviour on real material at 48 kHz, and rates.test.c holds the
 * HRP chain beneath it at every rate. This holds what the shell itself decides, at every rate, to
 * answers that follow from the rate alone:
 *
 *   1. the hop is a quarter of the RTA window for the rate (analysis_rates.h's rule, / 4), and
 *      the shell asks for its worker exactly when a whole hop is unconsumed: not at hop - 1 frames,
 *      at hop frames; and a bypassed shell with nothing to walk out asks for nothing;
 *   2. an idle shell is a wire: a second of audio in uneven blocks comes out byte for byte;
 *   3. the worker takes the OLDEST unconsumed hop, one hop per pass, so after k passes over 3 hops
 *      the consumed cursor is k hops; and once the writer has lapped the ring, one pass skips to
 *      the newest hop (consumed = written);
 *   4. an adopted band is that band's transfer function: a sine at rate/48 (the band's centre) and
 *      one at rate/12, each over whole periods once the section has settled, come out at
 *      |H(e^jw)| of the published coefficients to 0.005 dB, and |H| at the centre is the cut;
 *   5. a bypassed shell walks a band out in steps of OMX_HRP_GAIN_STEP_DB: a 2.5-step cut publishes
 *      1.5 steps, then 0.5, then nothing, three passes, each published section cutting exactly its
 *      reported depth at its centre; adopting the empty set parks the cascade and the output is the
 *      input again, byte for byte;
 *
 * and the calls a plugin face drives the shell through (FreeMixer/omx-dsp#80):
 *
 *   6. the analysis block is five RTA windows (four windows and two half-windows), and
 *      omx_hrp_lv2_init_block partitions it in init's argument order with no gap and no overlap:
 *      an instance bound to the block answers bit for bit what one bound to six buffers answers;
 *   7. omx_hrp_lv2_ask_of holds the family and the band budget inside their declared travels and
 *      rounds half away from zero (2.5 -> 3, -0.6 -> -1), a non-finite word reading as the
 *      declared default; bypass is a word above one half; install_ask writes exactly that into
 *      the instance, the amount clamped into its travel;
 *   8. asked and answered count passes, and "in flight" is their difference, across the 2^32 wrap;
 *   9. wants is due() under the snapshot's bypass, and answer is install_ask then analyse: a twin
 *      fed the same audio and driven by the two calls ends in the same bits;
 *  10. clear empties the cascade at once (the output is the input, byte for byte) and leaves the
 *      worker's slots; restart also frees every slot and brings both cursors to zero (the next
 *      pass is due after exactly one hop again), keeping the learned model and the counters.
 */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_lv2.h>

#include "analysis_rates.h"

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what, double rate) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL @ %.0f Hz: %s\n", rate, what);
  }
}

static void close_to(double got, double want, double tol, const char *what, double rate) {
  g_checks++;
  if (!(fabs(got - want) <= tol)) {
    g_fail++;
    fprintf(stderr, "FAIL @ %.0f Hz: %s (got %.9g, want %.9g, tol %.3g)\n", rate, what, got, want, tol);
  }
}

#define FFT_MAX OMX_RTA_FFT_SIZE_MAX
#define BUF_FRAMES 192000u
/* A host's uneven quanta: every block a different length, none a divisor of the hop. */
static const uint32_t QUANTA[] = {64u, 37u, 256u, 1u, 129u, 512u, 300u};
#define N_QUANTA (sizeof(QUANTA) / sizeof(QUANTA[0]))

static float carry[FFT_MAX], wre[FFT_MAX], wim[FFT_MAX], mags[FFT_MAX / 2], resid[FFT_MAX / 2], hopbuf[FFT_MAX];
static OmxHrpLv2 g_s;
static float g_in[BUF_FRAMES], g_out[BUF_FRAMES];

static int fresh(uint32_t rate) {
  return omx_hrp_lv2_init(&g_s, rate, carry, wre, wim, mags, resid, hopbuf);
}

/* An integer generator: the stimulus is the same bits on every machine. */
static void fill_lcg(float *x, uint32_t n, uint32_t seed) {
  for (uint32_t i = 0; i < n; i++) {
    seed = seed * 1664525u + 1013904223u;
    x[i] = (float)((int32_t)(seed >> 8) - (1 << 23)) / (float)(1 << 24);
  }
}

/* Feeds `n` frames of `in` through run() in the uneven quanta; `out` may be NULL (discarded). */
static void run_frames(const float *in, float *out, uint32_t n) {
  static float scratch[512];
  uint32_t at = 0, q = 0;
  while (at < n) {
    uint32_t b = QUANTA[q++ % N_QUANTA];
    if (b > n - at) b = n - at;
    omx_hrp_lv2_run(&g_s, in + at, out ? out + at : scratch, b);
    at += b;
  }
}

static void run_silence(uint32_t n) {
  static float zeros[4096];
  while (n) {
    const uint32_t b = n < 4096u ? n : 4096u;
    run_frames(zeros, NULL, b);
    n -= b;
  }
}

/* |H(e^jw)| of one {b0, b1, b2, a1, a2} section, in double. */
static double section_mag(const float c[5], double w) {
  const double cr1 = cos(w), si1 = sin(w), cr2 = cos(2.0 * w), si2 = sin(2.0 * w);
  const double nr = c[0] + c[1] * cr1 + c[2] * cr2, ni = -(c[1] * si1 + c[2] * si2);
  const double dr = 1.0 + c[3] * cr1 + c[4] * cr2, di = -(c[3] * si1 + c[4] * si2);
  return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

static double db(double x) { return 20.0 * log10(x); }

/* ---- 1. the hop and when the worker is asked for -------------------------------------- */

static void arm_hop_and_due(uint32_t rate) {
  check(fresh(rate), "init accepts the rate", rate);
  const uint32_t hop = omx_analysis_fft_for_rate(rate) / 4u;
  check(omx_hrp_lv2_hop(&g_s) == hop, "hop = a quarter of the RTA window for the rate", rate);
  check(!omx_hrp_lv2_due(&g_s, 0), "nothing written: not due", rate);
  run_silence(hop - 1u);
  check(!omx_hrp_lv2_due(&g_s, 0), "hop - 1 frames: not due", rate);
  run_silence(1u);
  check(omx_hrp_lv2_due(&g_s, 0) == 1, "hop frames: due", rate);
  check(!omx_hrp_lv2_due(&g_s, 1), "bypassed with an empty cascade: never due", rate);
  check(OMX_HRP_LV2_LATENCY_FRAMES == 0.0f, "latency declared 0", rate);
}

/* ---- 2. idle is a wire ------------------------------------------------------------------ */

static void arm_idle_identity(uint32_t rate) {
  fresh(rate);
  fill_lcg(g_in, rate, rate);
  run_frames(g_in, g_out, rate);
  check(memcmp(g_in, g_out, (size_t)rate * sizeof(float)) == 0, "idle shell: one second out byte for byte", rate);
}

/* ---- 3. the worker's cursor ------------------------------------------------------------- */

static void arm_cursor(uint32_t rate) {
  OmxHrpLv2Set set;
  fresh(rate);
  const uint32_t hop = omx_hrp_lv2_hop(&g_s);
  fill_lcg(g_in, 3u * hop, 7u);
  run_frames(g_in, NULL, 3u * hop);
  for (uint32_t k = 1; k <= 3u; k++) {
    omx_hrp_lv2_analyse(&g_s, 0, &set);
    check(atomic_load(&g_s.consumed) == (uint64_t)k * hop, "k passes over 3 hops consume k hops", rate);
  }
  check(!omx_hrp_lv2_due(&g_s, 0), "all three hops consumed: not due", rate);
  omx_hrp_lv2_analyse(&g_s, 0, &set);
  check(atomic_load(&g_s.consumed) == 3ull * hop && set.count == 0u, "a pass with no whole hop takes nothing", rate);

  fresh(rate);
  const uint32_t lap = OMX_RTA_RING_SIZE + 2u * hop + 7u;
  run_silence(lap);
  omx_hrp_lv2_analyse(&g_s, 0, &set);
  check(atomic_load(&g_s.consumed) == (uint64_t)lap, "a lapped ring: one pass skips to the newest hop", rate);
}

/* ---- 4. an adopted band is its transfer function ---------------------------------------- */

/* Out/in RMS, in dB, of a unit sine of `period` samples through the shell, over whole periods once
 * a quarter second has settled the section. */
static double measured_db(uint32_t rate, uint32_t period) {
  const uint32_t settle = (rate / 4u / period) * period, span = (rate / 2u / period) * period;
  for (uint32_t i = 0; i < settle + span; i++) g_in[i] = (float)sin(2.0 * M_PI * (double)(i % period) / (double)period);
  run_frames(g_in, g_out, settle + span);
  double ei = 0.0, eo = 0.0;
  for (uint32_t i = settle; i < settle + span; i++) {
    ei += (double)g_in[i] * g_in[i];
    eo += (double)g_out[i] * g_out[i];
  }
  return 10.0 * log10(eo / ei);
}

static void arm_band(uint32_t rate) {
  const float cut = 6.0f;
  const uint32_t pc = 48u, po = 12u; /* rate/48 and rate/12: whole periods at every rate */
  OmxHrpLv2Set set;
  memset(&set, 0, sizeof(set));
  set.count = 1u;
  set.freq_hz[0] = (float)rate / (float)pc;
  set.cut_db[0] = cut;
  omx_hrp_peaking_coeffs(set.freq_hz[0], OMX_HRP_BAND_Q, -cut, rate, set.coeffs[0]);

  close_to(db(section_mag(set.coeffs[0], 2.0 * M_PI / pc)), -cut, 1e-3, "|H| at the centre is the cut", rate);

  fresh(rate);
  omx_hrp_lv2_adopt(&g_s, &set);
  check(omx_hrp_cascade_live(&g_s.live) == 1u, "adopted: one live section", rate);
  close_to(measured_db(rate, pc), db(section_mag(set.coeffs[0], 2.0 * M_PI / pc)), 0.005,
           "a sine at the centre comes out at |H|", rate);
  fresh(rate);
  omx_hrp_lv2_adopt(&g_s, &set);
  close_to(measured_db(rate, po), db(section_mag(set.coeffs[0], 2.0 * M_PI / po)), 0.005,
           "a sine at rate/12 comes out at |H|", rate);
}

/* ---- 5. bypass walks a band out, in bounded steps --------------------------------------- */

static void arm_walk_out(uint32_t rate) {
  const float step = (float)OMX_HRP_GAIN_STEP_DB;
  const uint32_t pc = 48u;
  OmxHrpLv2Set set;
  fresh(rate);
  const uint32_t hop = omx_hrp_lv2_hop(&g_s);
  g_s.slot[0] = (OmxHrpLv2Slot){.voice_id = 60, .harmonic = 2u, .freq_hz = (float)rate / (float)pc,
                                .applied_db = 2.5f * step};
  const float want[3] = {1.5f * step, 0.5f * step, 0.0f};
  for (int k = 0; k < 3; k++) {
    run_silence(hop);
    check(omx_hrp_lv2_due(&g_s, 1) == (k > 0), "bypassed: due only while a live section walks out", rate);
    omx_hrp_lv2_analyse(&g_s, 1, &set);
    if (want[k] > 0.0f) {
      check(set.count == 1u, "a walking band is still published", rate);
      close_to(set.cut_db[0], want[k], 4.0 * FLT_EPSILON * want[k], "the walk moves one step per pass", rate);
      close_to(db(section_mag(set.coeffs[0], 2.0 * M_PI / pc)), -want[k], 1e-3,
               "the published section cuts its reported depth at its centre", rate);
    } else {
      check(set.count == 0u, "at 0 dB the band is gone", rate);
      check(g_s.slot[0].applied_db == 0.0f, "the slot arrives at exactly 0 dB and is free", rate);
    }
    omx_hrp_lv2_adopt(&g_s, &set);
  }
  check(omx_hrp_cascade_live(&g_s.live) == 0u, "the empty set parks the cascade", rate);
  fill_lcg(g_in, rate / 4u, 11u);
  run_frames(g_in, g_out, rate / 4u);
  check(memcmp(g_in, g_out, (size_t)(rate / 4u) * sizeof(float)) == 0, "walked out: the output is the input, byte for byte",
        rate);
}

/* ---- 6. the analysis block -------------------------------------------------------------- */

#define BLOCK_MAX (5u * FFT_MAX)
static float g_block[BLOCK_MAX + 1u];
static OmxHrpLv2 g_t;
static float carry2[FFT_MAX], wre2[FFT_MAX], wim2[FFT_MAX], mags2[FFT_MAX / 2], resid2[FFT_MAX / 2], hop2[FFT_MAX];

static void arm_block(uint32_t rate) {
  const uint32_t fft = omx_analysis_fft_for_rate(rate), half = fft / 2u;
  check(omx_hrp_lv2_block_floats(rate) == 5u * fft, "block_floats = five windows", rate);
  check(!omx_hrp_lv2_init_block(&g_t, rate, NULL), "init_block refuses a NULL block", rate);
  const float canary = 1234.5f;
  g_block[5u * fft] = canary;
  check(omx_hrp_lv2_init_block(&g_t, rate, g_block) == 1, "init_block accepts the rate", rate);
  check(g_t.render.carry == g_block && g_t.render.work_re == g_block + fft && g_t.render.work_im == g_block + 2u * fft &&
            g_t.render.mags == g_block + 3u * fft && g_t.render.residual == g_block + 3u * fft + half,
        "the block holds carry, work_re, work_im, mags, residual in init's order", rate);
  check(g_t.hop == g_block + 4u * fft, "the hop buffer is the block's last window", rate);
  check(omx_hrp_lv2_init(&g_s, rate, carry2, wre2, wim2, mags2, resid2, hop2) == 1, "init accepts the rate", rate);

  const uint32_t hop = omx_hrp_lv2_hop(&g_s);
  check(omx_hrp_lv2_hop(&g_t) == hop, "a block-bound instance has the same hop", rate);
  OmxHrpLv2Set a, b;
  fill_lcg(g_in, 6u * hop, 23u);
  for (uint32_t k = 0; k < 6u; k++) {
    omx_hrp_lv2_run(&g_s, g_in + k * hop, g_out, hop);
    omx_hrp_lv2_run(&g_t, g_in + k * hop, g_out, hop);
    omx_hrp_lv2_analyse(&g_s, 0, &a);
    omx_hrp_lv2_analyse(&g_t, 0, &b);
  }
  check(memcmp(&a, &b, sizeof(a)) == 0 && memcmp(g_s.slot, g_t.slot, sizeof(g_s.slot)) == 0 &&
            memcmp(&g_s.render.baseline, &g_t.render.baseline, sizeof(g_s.render.baseline)) == 0,
        "block-bound and six-buffer instances answer the same bits", rate);
  check(memcmp(g_t.render.mags, mags2, (size_t)half * sizeof(float)) == 0 &&
            memcmp(g_t.render.residual, resid2, (size_t)half * sizeof(float)) == 0,
        "the block's half-windows hold what the six buffers hold", rate);
  check(g_block[5u * fft] == canary, "nothing is written past the block", rate);
}

/* ---- 7. the snapshot and its install ------------------------------------------------------ */

static void arm_ask(uint32_t rate) {
  const float nan = NAN, inf = INFINITY;
  const float fw[] = {-1.0f, -1.4f, -0.6f, 0.0f, 2.5f, 3.49f, 9.0f, 9.6f, 100.0f, -5.0f, nan, inf, -inf};
  const int fwant[] = {-1, -1, -1, 0, 3, 3, 9, 9, 9, -1, OMX_HRP_FAMILY_RANGE_DEFAULT, OMX_HRP_FAMILY_RANGE_DEFAULT,
                       OMX_HRP_FAMILY_RANGE_DEFAULT};
  int ok = 1;
  for (size_t i = 0; i < sizeof(fw) / sizeof(fw[0]); i++) ok &= omx_hrp_lv2_ask_of(0.0f, fw[i], 0.5f, 4.0f).family == fwant[i];
  check(ok, "ask_of: family held in its travel, rounded half away from zero, non-finite = default", rate);
  check(omx_hrp_lv2_ask_of(0.0f, nan, 0.5f, 4.0f).family == OMX_HRP_FAMILY_NONE, "ask_of: a NaN family is no family",
        rate);

  const float bw[] = {1.0f, 0.0f, -3.0f, 3.5f, 4.49f, 8.0f, 8.4f, 50.0f, nan, inf, -inf};
  const uint32_t bwant[] = {1u, 1u, 1u, 4u, 4u, 8u, 8u, 8u, OMX_HRP_BANDS_RANGE_DEFAULT, OMX_HRP_BANDS_RANGE_DEFAULT,
                            OMX_HRP_BANDS_RANGE_DEFAULT};
  ok = 1;
  for (size_t i = 0; i < sizeof(bw) / sizeof(bw[0]); i++) ok &= omx_hrp_lv2_ask_of(0.0f, -1.0f, 0.5f, bw[i]).max_bands == bwant[i];
  check(ok, "ask_of: band budget held in its travel, rounded half away from zero, non-finite = default", rate);
  check(OMX_HRP_BANDS_RANGE_DEFAULT == OMX_HRP_LV2_MAX_BANDS, "the band budget comes up at the answer set's size", rate);

  check(omx_hrp_lv2_ask_of(0.5f, 0.0f, 0.5f, 1.0f).bypass == 0, "bypass 0.5 is not bypassed", rate);
  check(omx_hrp_lv2_ask_of(0.5000001f, 0.0f, 0.5f, 1.0f).bypass == 1, "bypass above one half is bypassed", rate);
  check(omx_hrp_lv2_ask_of(nan, 0.0f, 0.5f, 1.0f).bypass == 0, "a NaN bypass is not bypassed", rate);
  check(omx_hrp_lv2_ask_of(0.0f, 0.0f, 0.37f, 1.0f).amount == 0.37f, "ask_of carries the amount as written", rate);

  fresh(rate);
  const OmxHrpLv2Ask g = omx_hrp_lv2_ask_of(0.0f, (float)OMX_HRP_FAMILY_GUITAR, 0.25f, 3.0f);
  omx_hrp_lv2_install_ask(&g_s, &g);
  const OmxHrpConfig guitar = omx_hrp_family_search(OMX_HRP_FAMILY_GUITAR);
  check(g_s.family == OMX_HRP_FAMILY_GUITAR && memcmp(&g_s.render.cfg.pitch, &guitar, sizeof(guitar)) == 0,
        "install: the family and its search are installed", rate);
  check(g_s.amount == 0.25f && g_s.max_bands == 3u, "install: amount and band budget installed", rate);
  OmxHrpLv2Ask x = g;
  x.amount = 1.7f;
  omx_hrp_lv2_install_ask(&g_s, &x);
  check(g_s.amount == (float)OMX_HRP_AMOUNT_RANGE_MAX, "install: an amount past the travel reads as its max", rate);
  x.amount = nan;
  omx_hrp_lv2_install_ask(&g_s, &x);
  check(g_s.amount == (float)OMX_HRP_AMOUNT_RANGE_MIN, "install: a NaN amount reads as the travel's min", rate);
  x.family = OMX_HRP_FAMILY_NONE;
  omx_hrp_lv2_install_ask(&g_s, &x);
  const OmxHrpConfig wide = omx_hrp_family_search(OMX_HRP_FAMILY_NONE);
  check(g_s.family == OMX_HRP_FAMILY_NONE && memcmp(&g_s.render.cfg.pitch, &wide, sizeof(wide)) == 0,
        "install: back to no family, the wide search", rate);
  omx_hrp_lv2_install_ask(&g_s, NULL);
  check(g_s.family == OMX_HRP_FAMILY_NONE && g_s.max_bands == 3u, "install of no snapshot changes nothing", rate);
}

/* ---- 8. the pass counters ----------------------------------------------------------------- */

static void arm_counters(uint32_t rate) {
  fresh(rate);
  check(!omx_hrp_lv2_in_flight(&g_s), "fresh: no pass in flight", rate);
  omx_hrp_lv2_mark_asked(&g_s);
  check(omx_hrp_lv2_in_flight(&g_s) == 1 && atomic_load(&g_s.asked) == 1u, "one ask: in flight", rate);
  omx_hrp_lv2_mark_answered(&g_s);
  check(!omx_hrp_lv2_in_flight(&g_s) && atomic_load(&g_s.answered) == 1u, "answered: not in flight", rate);
  atomic_store(&g_s.asked, UINT32_MAX);
  atomic_store(&g_s.answered, UINT32_MAX);
  omx_hrp_lv2_mark_asked(&g_s);
  check(atomic_load(&g_s.asked) == 0u && omx_hrp_lv2_in_flight(&g_s) == 1, "wrapped counters: in flight", rate);
  omx_hrp_lv2_mark_answered(&g_s);
  check(!omx_hrp_lv2_in_flight(&g_s), "wrapped counters: answered", rate);
  check(!omx_hrp_lv2_in_flight(NULL), "no instance: nothing in flight", rate);
}

/* ---- 9. wants and answer ------------------------------------------------------------------ */

static void arm_wants_answer(uint32_t rate) {
  fresh(rate);
  const uint32_t hop = omx_hrp_lv2_hop(&g_s);
  const OmxHrpLv2Ask on = omx_hrp_lv2_ask_of(0.0f, -1.0f, 0.5f, 8.0f), off = omx_hrp_lv2_ask_of(1.0f, -1.0f, 0.5f, 8.0f);
  run_silence(hop);
  check(omx_hrp_lv2_wants(&g_s, &on) == 1 && omx_hrp_lv2_wants(&g_s, &off) == 0,
        "wants follows the snapshot's bypass (an empty cascade)", rate);
  check(!omx_hrp_lv2_wants(&g_s, NULL), "wants with no snapshot: never", rate);

  /* Twins: g_s driven by answer(), g_t by install_ask() + analyse(), over the same audio. */
  omx_hrp_lv2_init(&g_s, rate, carry, wre, wim, mags, resid, hopbuf);
  omx_hrp_lv2_init(&g_t, rate, carry2, wre2, wim2, mags2, resid2, hop2);
  const OmxHrpLv2Slot planted = {.voice_id = 60, .harmonic = 2u, .freq_hz = (float)rate / 48.0f,
                                 .applied_db = 2.5f * (float)OMX_HRP_GAIN_STEP_DB};
  g_s.slot[0] = planted;
  g_t.slot[0] = planted;
  const OmxHrpLv2Ask asks[3] = {omx_hrp_lv2_ask_of(1.0f, 3.0f, 0.75f, 3.0f), omx_hrp_lv2_ask_of(0.0f, 3.0f, 0.75f, 3.0f),
                                omx_hrp_lv2_ask_of(0.0f, 5.0f, 2.0f, 2.0f)};
  OmxHrpLv2Set a, b;
  int same = 1;
  fill_lcg(g_in, 3u * hop, 29u);
  for (uint32_t k = 0; k < 3u; k++) {
    omx_hrp_lv2_run(&g_s, g_in + k * hop, g_out, hop);
    omx_hrp_lv2_run(&g_t, g_in + k * hop, g_out, hop);
    omx_hrp_lv2_answer(&g_s, &asks[k], &a);
    omx_hrp_lv2_install_ask(&g_t, &asks[k]);
    omx_hrp_lv2_analyse(&g_t, asks[k].bypass, &b);
    same &= memcmp(&a, &b, sizeof(a)) == 0 && memcmp(g_s.slot, g_t.slot, sizeof(g_s.slot)) == 0;
    if (k == 0) {
      check(a.count == 1u && a.cut_db[0] == 1.5f * (float)OMX_HRP_GAIN_STEP_DB, "answer: a bypassed pass walks the band out",
            rate);
    }
  }
  check(same, "answer = install_ask + analyse, bit for bit", rate);
  check(g_s.family == 5 && g_s.amount == 1.0f && g_s.max_bands == 2u &&
            atomic_load(&g_s.consumed) == atomic_load(&g_t.consumed),
        "answer installs the snapshot", rate);
}

/* ---- 10. clear and restart ---------------------------------------------------------------- */

static void arm_clear_restart(uint32_t rate) {
  const uint32_t pc = 48u;
  OmxHrpLv2Set set;
  memset(&set, 0, sizeof(set));
  set.count = 1u;
  set.freq_hz[0] = (float)rate / (float)pc;
  set.cut_db[0] = 6.0f;
  omx_hrp_peaking_coeffs(set.freq_hz[0], OMX_HRP_BAND_Q, -6.0f, rate, set.coeffs[0]);

  fresh(rate);
  const uint32_t hop = omx_hrp_lv2_hop(&g_s);
  g_s.slot[0] = (OmxHrpLv2Slot){.voice_id = 60, .harmonic = 2u, .freq_hz = set.freq_hz[0], .applied_db = 6.0f};
  omx_hrp_lv2_adopt(&g_s, &set);
  fill_lcg(g_in, rate / 4u, 31u);
  run_frames(g_in, g_out, rate / 4u);
  check(memcmp(g_in, g_out, (size_t)(rate / 4u) * sizeof(float)) != 0, "an adopted band changes the audio", rate);
  omx_hrp_lv2_clear(&g_s);
  check(omx_hrp_cascade_live(&g_s.live) == 0u, "cleared: the cascade is empty", rate);
  run_frames(g_in, g_out, rate / 4u);
  check(memcmp(g_in, g_out, (size_t)(rate / 4u) * sizeof(float)) == 0, "cleared: the output is the input, byte for byte",
        rate);
  check(g_s.slot[0].applied_db == 6.0f, "clear leaves the worker's slots", rate);

  /* A shell that has heard audio, learned, counted passes and holds a band. */
  fresh(rate);
  fill_lcg(g_in, 5u * hop + 17u, 37u);
  run_frames(g_in, NULL, 5u * hop + 17u);
  for (int k = 0; k < 3; k++) omx_hrp_lv2_analyse(&g_s, 0, &set);
  omx_hrp_lv2_mark_asked(&g_s);
  omx_hrp_lv2_mark_answered(&g_s);
  g_s.slot[1] = (OmxHrpLv2Slot){.voice_id = 61, .harmonic = 3u, .freq_hz = (float)rate / (float)pc, .applied_db = 4.0f};
  set.count = 1u;
  omx_hrp_peaking_coeffs((float)rate / (float)pc, OMX_HRP_BAND_Q, -4.0f, rate, set.coeffs[0]);
  omx_hrp_lv2_adopt(&g_s, &set);
  static OmxHrpBaseline model;
  memcpy(&model, &g_s.render.baseline, sizeof(model));
  const uint32_t passes = g_s.passes;

  omx_hrp_lv2_restart(&g_s);
  check(atomic_load(&g_s.written) == 0u && atomic_load(&g_s.consumed) == 0u, "restart: both cursors at zero", rate);
  OmxHrpLv2Slot none[OMX_HRP_LV2_MAX_BANDS];
  memset(none, 0, sizeof(none));
  check(memcmp(g_s.slot, none, sizeof(none)) == 0, "restart: every slot free", rate);
  check(omx_hrp_cascade_live(&g_s.live) == 0u, "restart: no correction in force", rate);
  check(memcmp(&model, &g_s.render.baseline, sizeof(model)) == 0 && g_s.passes == passes,
        "restart keeps the learned model and the pass count", rate);
  check(atomic_load(&g_s.asked) == 1u && atomic_load(&g_s.answered) == 1u, "restart keeps the pass counters", rate);
  check(g_s.ready == 1, "restart: still ready", rate);
  run_silence(hop - 1u);
  check(!omx_hrp_lv2_due(&g_s, 0), "restart: hop - 1 frames later, not due", rate);
  run_silence(1u);
  check(omx_hrp_lv2_due(&g_s, 0) == 1, "restart: one hop later, due", rate);
  omx_hrp_lv2_analyse(&g_s, 0, &set);
  check(atomic_load(&g_s.consumed) == (uint64_t)hop, "restart: the first pass takes the first hop", rate);
}

int main(void) {
  omx_analysis_require_rates();
  for (int k = 0; k < OMX_ANALYSIS_RATE_COUNT; k++) {
    const uint32_t rate = (uint32_t)OMX_ANALYSIS_RATES[k];
    arm_hop_and_due(rate);
    arm_idle_identity(rate);
    arm_cursor(rate);
    arm_band(rate);
    arm_walk_out(rate);
    arm_block(rate);
    arm_ask(rate);
    arm_counters(rate);
    arm_wants_answer(rate);
    arm_clear_restart(rate);
  }
  printf("hrp_lv2_rates: %d checks over %d rates, %d failed\n", g_checks, OMX_ANALYSIS_RATE_COUNT, g_fail);
  return g_fail ? 1 : 0;
}
