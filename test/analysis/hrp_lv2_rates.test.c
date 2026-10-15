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
 *      input again, byte for byte.
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

int main(void) {
  omx_analysis_require_rates();
  for (int k = 0; k < OMX_ANALYSIS_RATE_COUNT; k++) {
    const uint32_t rate = (uint32_t)OMX_ANALYSIS_RATES[k];
    arm_hop_and_due(rate);
    arm_idle_identity(rate);
    arm_cursor(rate);
    arm_band(rate);
    arm_walk_out(rate);
  }
  printf("hrp_lv2_rates: %d checks over %d rates, %d failed\n", g_checks, OMX_ANALYSIS_RATE_COUNT, g_fail);
  return g_fail ? 1 : 0;
}
