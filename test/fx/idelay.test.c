// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The integer delay line's oracle (omx_idelay.h): the route and bus-output delay arms of the
 * console's mix_dsp.test.c and mix_delay_math.test.c, moved with the line. The arms there that
 * measure the FX delay and the input alignment stay with those stages.
 *
 * Four claims, at the nine RME rates (32 to 192 kHz): the delay dialled in ms is the delay the
 * audio takes, to the nearest sample; zero is an exact passthrough; a steady delay does not
 * depend on the block size; and a re-point cross-fades across one block and lands exactly on the
 * new delay. The ring
 * the arms use is the console's: 1000 ms at OMX_RT_HARD_TARGET_RATE, plus one. Contracts are
 * compiled in and the last arm requires an empty ledger.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_idelay.h>

#include "fx_rates.h"

/* The console's output delay ring: its ceiling (OMX_IDELAY_MAX_MS) at the highest graph rate, +1 so the
 * full ceiling is a tap strictly inside the ring. */
#define MAX_MS OMX_IDELAY_MAX_MS /* the contract's OUTPUT_DELAY_RANGE.max, through the header */
#define MAX_RATE OMX_RT_HARD_TARGET_RATE
#define CAP (((MAX_RATE / 1000) * MAX_MS) + 1)

#define NRATES ((int)OMX_FX_RME_RATE_COUNT)
#define RATE OMX_FX_RME_RATES

static int g_fail = 0, g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

static void close_to(float got, float want, const char *what) {
  g_checks++;
  if (!(fabsf(got - want) <= 1e-6f)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s (got %.9g, want %.9g)\n", what, (double)got, (double)want);
  }
}

/* A band, because a one-sided bound on a measured quantity catches only one of its two ways of
 * being wrong. */
static void in_band(double v, double lo, double hi, const char *what) {
  g_checks++;
  if (!(v >= lo && v <= hi)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s — measured %.6g, wanted [%.6g, %.6g]\n", what, v, lo, hi);
  }
}

/* After each rate: the ledger holds nothing the rate does not explain (fx_rates.h). At 32, 64 and
 * 128 kHz, outside the declaration, the conversion's `rate-is-declared` precondition is the
 * contract saying so, and is not counted. */
static void rate_done(float sr) {
  char what[120];
  snprintf(what, sizeof what, "%.0f Hz: no contract violation the rate does not explain", (double)sr);
  check(omx_fx_drain_ledger(sr) == 0u, what);
}

/** The index of the largest |sample|: where an impulse lands. */
static int peak_index(const float *b, int n) {
  int best = 0;
  for (int i = 1; i < n; i++)
    if (fabsf(b[i]) > fabsf(b[best])) best = i;
  return best;
}

static double rms(const float *b, int from, int to) {
  double a = 0.0;
  for (int i = from; i < to; i++) a += (double)b[i] * (double)b[i];
  return to > from ? sqrt(a / (double)(to - from)) : 0.0;
}

/* ---- 1. ms -> samples --------------------------------------------------------------------- */

static void test_samples_fixed_points(void) {
  check(omx_idelay_samples(0.0f, 48000.0f, CAP) == 0, "0 ms is 0 samples");
  check(omx_idelay_samples(-5.0f, 48000.0f, CAP) == 0, "a negative ms is floored to 0");
  check(omx_idelay_samples(1.0f, 48000.0f, CAP) == 48, "1 ms at 48 kHz is 48 samples");
  check(omx_idelay_samples(50.0f, 48000.0f, CAP) == 2400, "50 ms at 48 kHz is 2400 samples");
  check(omx_idelay_samples(50.0f, 96000.0f, CAP) == 4800, "50 ms at 96 kHz is 4800 samples");
  check(omx_idelay_samples((float)MAX_MS, (float)MAX_RATE, CAP) == (MAX_RATE / 1000) * MAX_MS,
        "the ceiling is reachable at the highest rate, unclamped");
  check(omx_idelay_samples(100000.0f, 48000.0f, CAP) == CAP - 1, "a huge ms saturates at cap - 1");
  check(omx_idelay_samples(20.408f, 96000.0f, CAP) == 1959, "20.408 ms at 96 kHz is 1959 samples");
  check(omx_idelay_samples(20.408f, 48000.0f, CAP) == 980, "20.408 ms at 48 kHz is 980 samples");
  check(omx_idelay_samples(86.0f, 96000.0f, CAP) == 8256, "86 ms at 96 kHz is 8256 samples");
  check(omx_idelay_samples((float)MAX_MS, 96000.0f, CAP) < CAP, "the ceiling fits the ring at 96 kHz");
  check(omx_idelay_samples(5.0f, 48000.0f, 0u) == 0, "a ring of 0 is no delay");
  check(omx_idelay_samples(5.0f, 0.0f, CAP) == 0, "a rate of 0 is no delay");
}

/* Round to nearest, never truncate: a truncating conversion makes every dialled delay up to a
 * sample early, 10 us at 96 kHz of a control whose whole job is arriving together. */
static void test_samples_round_to_nearest_at_every_rate(void) {
  static const float MS[] = {0.0f, 0.001f, 0.5f, 1.0f, 5.0f, 23.5f, 99.9f, 120.0f, 750.0f};
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    char what[160];
    for (size_t k = 0; k < sizeof MS / sizeof MS[0]; k++) {
      double want = floor((double)MS[k] * (double)sr * 0.001 + 0.5);
      snprintf(what, sizeof what, "%.0f Hz: %g ms is %.0f samples, to the nearest", (double)sr, (double)MS[k],
               want);
      check((double)omx_idelay_samples(MS[k], sr, CAP) == want, what);
    }
    snprintf(what, sizeof what, "%.0f Hz: a negative ms is zero samples, not a wrap", (double)sr);
    check(omx_idelay_samples(-5.0f, sr, CAP) == 0u, what);
    snprintf(what, sizeof what, "%.0f Hz: an absurd ms saturates at the ring, it does not wrap", (double)sr);
    check(omx_idelay_samples(1.0e9f, sr, CAP) == CAP - 1u, what);
    uint32_t d = omx_idelay_samples((float)MAX_MS, sr, CAP);
    snprintf(what, sizeof what, "%.0f Hz: the full %d ms is reached without clamping", (double)sr, MAX_MS);
    check((double)d == floor((double)MAX_MS * (double)sr * 0.001 + 0.5) && d < CAP, what);
    rate_done(sr);
  }
}

/* ---- 2. the line, small rings ------------------------------------------------------------- */

static void test_impulse(void) {
  enum { RING = 64, N = 16 };
  float ring[RING] = {0};
  uint32_t wpos = 0, cur = 5;
  float buf[N] = {0};
  buf[0] = 1.0f;
  omx_idelay_apply(buf, N, ring, RING, &wpos, &cur, 5);
  for (int i = 0; i < N; i++) close_to(buf[i], i == 5 ? 1.0f : 0.0f, "an impulse lands exactly 5 samples late");
  check(wpos == N, "the write cursor advanced by n");
  check(cur == 5, "a steady delay leaves cur alone");
}

static void test_zero_is_a_passthrough(void) {
  enum { RING = 64, N = 8 };
  float ring[RING] = {0};
  uint32_t wpos = 0, cur = 0;
  float buf[N] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
  float want[N];
  memcpy(want, buf, sizeof buf);
  omx_idelay_apply(buf, N, ring, RING, &wpos, &cur, 0);
  check(memcmp(buf, want, sizeof buf) == 0, "D = 0 is a bit-exact passthrough");
}

static void test_carries_across_blocks(void) {
  enum { RING = 64, N = 4 };
  float ring[RING] = {0};
  uint32_t wpos = 0, cur = 6;
  float b1[N] = {1, 2, 3, 4};
  omx_idelay_apply(b1, N, ring, RING, &wpos, &cur, 6);
  for (int i = 0; i < N; i++) close_to(b1[i], 0.0f, "block 1 is still silent (the delay is longer than n)");
  float b2[N] = {5, 6, 7, 8};
  omx_idelay_apply(b2, N, ring, RING, &wpos, &cur, 6);
  close_to(b2[0], 0.0f, "b2[0] is still silent");
  close_to(b2[1], 0.0f, "b2[1] is still silent");
  close_to(b2[2], 1.0f, "b1[0] emerges 6 samples late");
  close_to(b2[3], 2.0f, "b1[1] emerges 6 samples late");
}

static void test_null_ring_is_a_passthrough(void) {
  float buf[3] = {1.0f, 2.0f, 3.0f};
  uint32_t wpos = 0, cur = 0;
  omx_idelay_apply(buf, 3, NULL, CAP, &wpos, &cur, 10);
  close_to(buf[0], 1.0f, "a NULL ring passes [0] through");
  close_to(buf[2], 3.0f, "a NULL ring passes [2] through");
  check(wpos == 0 && cur == 0, "a NULL ring moves no cursor");
}

static void test_cross_fade_on_dc_is_flat(void) {
  enum { RING = 64, N = 8 };
  float ring[RING] = {0};
  uint32_t wpos = 0, cur = 0;
  for (int blk = 0; blk < 4; blk++) {
    float b[N];
    for (int i = 0; i < N; i++) b[i] = 1.0f;
    omx_idelay_apply(b, N, ring, RING, &wpos, &cur, 0);
  }
  float b[N];
  for (int i = 0; i < N; i++) b[i] = 1.0f;
  omx_idelay_apply(b, N, ring, RING, &wpos, &cur, 4);
  for (int i = 0; i < N; i++) close_to(b[i], 1.0f, "a move on DC stays flat (no click)");
  check(cur == 4, "cur is the new delay after the cross-fade block");
}

/* ---- 3. the pair -------------------------------------------------------------------------- */

static void test_pair_zero_is_bit_identical(void) {
  enum { RING = 256, N = 16 };
  float ring_l[RING] = {0}, ring_r[RING] = {0};
  struct omx_idelay_pair d = {ring_l, ring_r, RING, 0, 0, 0, 0};
  float l[N], r[N], want_l[N], want_r[N];
  for (int i = 0; i < N; i++) {
    l[i] = want_l[i] = sinf((float)i * 0.37f);
    r[i] = want_r[i] = cosf((float)i * 0.11f) * 0.5f;
  }
  omx_idelay_pair_apply(l, r, N, &d, 0, 0);
  check(memcmp(l, want_l, sizeof l) == 0, "the pair at 0 is bit-identical on L");
  check(memcmp(r, want_r, sizeof r) == 0, "the pair at 0 is bit-identical on R");
  check(d.cur_l == 0 && d.cur_r == 0, "the pair at 0 engages no cross-fade");
}

static void test_pair_known_sample_count(void) {
  enum { RING = 512, N = 64 };
  float ring_l[RING] = {0}, ring_r[RING] = {0};
  struct omx_idelay_pair d = {ring_l, ring_r, RING, 0, 0, 21, 21};
  float l[N] = {0}, r[N] = {0};
  l[0] = 1.0f;
  r[0] = -1.0f;
  omx_idelay_pair_apply(l, r, N, &d, 21, 21);
  for (int i = 0; i < N; i++) {
    close_to(l[i], i == 21 ? 1.0f : 0.0f, "the L impulse lands at exactly 21 samples");
    close_to(r[i], i == 21 ? -1.0f : 0.0f, "the R impulse lands at exactly 21 samples");
  }
  check(d.wpos_l == N && d.wpos_r == N, "both write cursors advanced by n");
}

static void test_pair_mono(void) {
  enum { RING = 128, N = 8 };
  float ring_l[RING] = {0}, ring_r[RING] = {0};
  struct omx_idelay_pair d = {ring_l, ring_r, RING, 0, 0, 3, 0};
  float l[N] = {0};
  l[0] = 1.0f;
  omx_idelay_pair_apply(l, NULL, N, &d, 3, 3);
  close_to(l[3], 1.0f, "a mono pair delays its one leg");
  check(d.wpos_l == N, "a mono pair advanced the L cursor");
  check(d.wpos_r == 0, "a mono pair never touched the R cursor");
}

static void test_pair_unallocated_is_a_passthrough(void) {
  struct omx_idelay_pair d;
  memset(&d, 0, sizeof d);
  float l[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  float r[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
  omx_idelay_pair_apply(l, r, 4, &d, 999, 999);
  close_to(l[0], 1.0f, "an unallocated pair passes L through");
  close_to(l[3], 4.0f, "an unallocated pair passes L through (tail)");
  close_to(r[3], -4.0f, "an unallocated pair passes R through (tail)");
  check(d.cur_l == 0 && d.wpos_l == 0, "an unallocated pair moves no cursor");
}

static void test_pair_carries_across_blocks(void) {
  enum { RING = 256, N = 4 };
  float ring_l[RING] = {0}, ring_r[RING] = {0};
  struct omx_idelay_pair d = {ring_l, ring_r, RING, 0, 0, 6, 6};
  float b1l[N] = {1.0f, 2.0f, 3.0f, 4.0f}, b1r[N] = {1.0f, 2.0f, 3.0f, 4.0f};
  omx_idelay_pair_apply(b1l, b1r, N, &d, 6, 6);
  for (int i = 0; i < N; i++) close_to(b1l[i], 0.0f, "the pair's block 1 is still silent");
  float b2l[N] = {5.0f, 6.0f, 7.0f, 8.0f}, b2r[N] = {5.0f, 6.0f, 7.0f, 8.0f};
  omx_idelay_pair_apply(b2l, b2r, N, &d, 6, 6);
  close_to(b2l[2], 1.0f, "block 1's first sample emerges 6 late");
  close_to(b2r[3], 2.0f, "the R leg carries the same 6 samples");
}

/* ---- 4. every declared rate, the console's ring ------------------------------------------- */

static void test_an_impulse_lands_on_the_declared_sample(void) {
  static const float MS[] = {0.0f, 1.0f, 5.0f, 23.5f, 120.0f, 1000.0f};
  float *ring = calloc(CAP, sizeof(float));
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    for (size_t k = 0; k < sizeof MS / sizeof MS[0]; k++) {
      uint32_t d = omx_idelay_samples(MS[k], sr, CAP);
      int n = (int)d + 64;
      float *buf = calloc((size_t)n, sizeof(float));
      memset(ring, 0, CAP * sizeof(float));
      uint32_t wpos = 0, cur = d;
      buf[0] = 1.0f;
      for (int o = 0; o < n; o += 128) {
        int q = (n - o) < 128 ? (n - o) : 128;
        omx_idelay_apply(buf + o, (uint32_t)q, ring, CAP, &wpos, &cur, d);
      }
      int clean = 1;
      for (int i = 0; i < n; i++)
        if (i != (int)d && buf[i] != 0.0f) clean = 0;
      char what[200];
      snprintf(what, sizeof what, "%.0f Hz: a %g ms impulse lands on sample %u and nowhere else", (double)sr,
               (double)MS[k], d);
      check(peak_index(buf, n) == (int)d && buf[(int)d] == 1.0f && clean, what);
      free(buf);
    }
    rate_done(sr);
  }
  free(ring);
}

/* One target per side: two boxes at two distances off one pair arrive together only if the two
 * legs' delays are independent. */
static void test_the_pair_gives_each_leg_its_own_delay(void) {
  struct omx_idelay_pair d;
  memset(&d, 0, sizeof d);
  d.cap = CAP;
  d.ring_l = calloc(CAP, sizeof(float));
  d.ring_r = calloc(CAP, sizeof(float));
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    memset(d.ring_l, 0, CAP * sizeof(float));
    memset(d.ring_r, 0, CAP * sizeof(float));
    d.wpos_l = d.wpos_r = 0;
    uint32_t tl = omx_idelay_samples(7.0f, sr, d.cap);
    uint32_t tr = omx_idelay_samples(31.0f, sr, d.cap);
    d.cur_l = tl;
    d.cur_r = tr;
    int n = (int)tr + 64;
    float *l = calloc((size_t)n, sizeof(float));
    float *r = calloc((size_t)n, sizeof(float));
    l[0] = 1.0f;
    r[0] = 1.0f;
    for (int o = 0; o < n; o += 128) {
      int q = (n - o) < 128 ? (n - o) : 128;
      omx_idelay_pair_apply(l + o, r + o, (uint32_t)q, &d, tl, tr);
    }
    char what[200];
    snprintf(what, sizeof what, "%.0f Hz: 7 ms on L and 31 ms on R land on samples %u and %u", (double)sr, tl, tr);
    check(peak_index(l, n) == (int)tl && peak_index(r, n) == (int)tr, what);
    free(l);
    free(r);
    rate_done(sr);
  }
  free(d.ring_l);
  free(d.ring_r);
}

static void test_zero_is_an_exact_passthrough_at_every_rate(void) {
  enum { N = 4096 };
  static float src[N], rb[N];
  for (int i = 0; i < N; i++) src[i] = (float)sin(i * 0.037) * 0.7f + (float)((i % 17) - 8) * 0.01f;
  float *ring = calloc(CAP, sizeof(float));
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    char what[200];
    memcpy(rb, src, sizeof src);
    memset(ring, 0, CAP * sizeof(float));
    uint32_t wpos = 0, cur = 0;
    omx_idelay_apply(rb, N, ring, CAP, &wpos, &cur, omx_idelay_samples(0.0f, sr, CAP));
    snprintf(what, sizeof what, "%.0f Hz: the line at 0 is bit-identical", (double)sr);
    check(memcmp(rb, src, sizeof src) == 0, what);
    memcpy(rb, src, sizeof src);
    omx_idelay_apply(rb, N, NULL, 0u, &wpos, &cur, 0u);
    snprintf(what, sizeof what, "%.0f Hz: an unallocated ring is a passthrough, not a fault", (double)sr);
    check(memcmp(rb, src, sizeof src) == 0, what);
    rate_done(sr);
  }
  free(ring);
}

/* A steady delay is block-size invariant; a re-point is not, and that is the next arm's subject. */
static void test_a_steady_delay_is_block_size_invariant(void) {
  enum { N = 8192 };
  static const int BLK[4] = {1, 7, 64, 1024};
  static float src[N];
  for (int i = 0; i < N; i++) src[i] = (float)sin(i * 0.011) * (float)cos(i * 0.0007);
  float *ring = calloc(CAP, sizeof(float));
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    uint32_t d = omx_idelay_samples(12.0f, sr, CAP);
    float *ref = NULL;
    for (int b = 0; b < 4; b++) {
      float *buf = malloc(N * sizeof(float));
      memcpy(buf, src, N * sizeof(float));
      memset(ring, 0, CAP * sizeof(float));
      uint32_t wpos = 0, cur = d;
      for (int o = 0; o < N; o += BLK[b]) {
        int q = (N - o) < BLK[b] ? (N - o) : BLK[b];
        omx_idelay_apply(buf + o, (uint32_t)q, ring, CAP, &wpos, &cur, d);
      }
      if (b == 0) {
        ref = buf;
      } else {
        char what[200];
        snprintf(what, sizeof what, "%.0f Hz: blocks of %d are the same audio as one sample at a time", (double)sr,
                 BLK[b]);
        check(memcmp(buf, ref, N * sizeof(float)) == 0, what);
        free(buf);
      }
    }
    free(ref);
    rate_done(sr);
  }
  free(ring);
}

/*
 * The re-point is the one place the line is not a copy of its input: a linear blend of the same
 * signal at two delays, a comb. A probe at sr/8 makes a 4-sample move antiphase, so the blend
 * passes through a complete null at its midpoint; the transition block's RMS is 1/sqrt(3) of
 * steady (the integral of (1 - 2t)^2 over [0, 1]), and the dropout under -12 dB is a quarter of
 * the block, so its length in ms is the graph's quantum.
 */
static void test_the_re_point_nulls_at_the_antiphase_frequency(void) {
  static const int BLK[3] = {64, 256, 1024};
  float *ring = calloc(CAP, sizeof(float));
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    for (int b = 0; b < 3; b++) {
      int n = BLK[b];
      uint32_t d0 = omx_idelay_samples(12.0f, sr, CAP);
      uint32_t d1 = d0 + 4u;
      int warm = (int)((d1 / (uint32_t)n) + 2u) * n;
      int w0 = warm / n;
      int total = warm + 3 * n;
      float *buf = malloc((size_t)total * sizeof(float));
      for (int i = 0; i < total; i++) buf[i] = (float)sin(2.0 * M_PI * (double)i / 8.0);
      memset(ring, 0, CAP * sizeof(float));
      uint32_t wpos = 0, cur = d0;
      for (int k = 0; k < w0 + 3; k++)
        omx_idelay_apply(buf + k * n, (uint32_t)n, ring, CAP, &wpos, &cur, (k <= w0) ? d0 : d1);
      double steady = rms(buf, w0 * n, (w0 + 1) * n);
      double during = rms(buf, (w0 + 1) * n, (w0 + 2) * n);
      double after = rms(buf, (w0 + 2) * n, (w0 + 3) * n);
      char what[220];
      snprintf(what, sizeof what, "%.0f Hz / %d frames: the re-point block is 1/sqrt(3) of steady RMS", (double)sr, n);
      in_band(during / steady, 0.550, 0.600, what);
      snprintf(what, sizeof what, "%.0f Hz / %d frames: the block after the re-point is back to full level",
               (double)sr, n);
      in_band(after / steady, 0.995, 1.005, what);
      double worst = 1.0e9;
      int deep = 0;
      for (int i = (w0 + 1) * n; i + 8 <= (w0 + 2) * n; i++) {
        double w = rms(buf, i, i + 8) / steady;
        if (w < worst) worst = w;
        if (w < 0.25) deep++;
      }
      snprintf(what, sizeof what, "%.0f Hz / %d frames: the re-point passes through a complete null", (double)sr, n);
      in_band(worst, 0.0, 0.120, what);
      snprintf(what, sizeof what, "%.0f Hz / %d frames: the dropout is a quarter of the block (%d samples)",
               (double)sr, n, deep);
      in_band((double)deep, 0.25 * n - 12.0, 0.25 * n + 4.0, what);
      free(buf);
    }
    rate_done(sr);
  }
  free(ring);
}

/* The re-point lands exactly, in one block: a transition, never a slew left between two delays. */
static void test_the_re_point_arrives_at_the_new_delay_exactly(void) {
  float *ring = calloc(CAP, sizeof(float));
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    uint32_t d0 = omx_idelay_samples(5.0f, sr, CAP);
    uint32_t d1 = omx_idelay_samples(37.0f, sr, CAP);
    int n = 256;
    int tail = (int)d1 + 512;
    float *warm = calloc((size_t)n, sizeof(float));
    float *buf = calloc((size_t)tail, sizeof(float));
    memset(ring, 0, CAP * sizeof(float));
    uint32_t wpos = 0, cur = d0;
    omx_idelay_apply(warm, (uint32_t)n, ring, CAP, &wpos, &cur, d1);
    char what[200];
    snprintf(what, sizeof what, "%.0f Hz: after one block the applied delay is the target (%u samples)", (double)sr,
             d1);
    check(cur == d1, what);
    buf[0] = 1.0f;
    for (int o = 0; o < tail; o += 128) {
      int q = (tail - o) < 128 ? (tail - o) : 128;
      omx_idelay_apply(buf + o, (uint32_t)q, ring, CAP, &wpos, &cur, d1);
    }
    snprintf(what, sizeof what, "%.0f Hz: an impulse after the re-point lands on the new delay", (double)sr);
    check(peak_index(buf, tail) == (int)d1 && buf[(int)d1] == 1.0f, what);
    free(warm);
    free(buf);
    rate_done(sr);
  }
  free(ring);
}

/* The same antiphase move a jumping tap turns into a 1 + 1/sqrt2 step never steps past the
 * signal's own worst step through the cross-fade, at any of the eight phases of the probe. */
static void test_a_move_never_steps_past_the_signal(void) {
  float *ring = calloc(CAP, sizeof(float));
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = 256;
    double worst_route = 0.0, worst_steady = 0.0;
    for (int ph = 0; ph < 8; ph++) {
      uint32_t d0 = omx_idelay_samples(12.0f, sr, CAP);
      uint32_t d1 = d0 + 4u;
      int warm = (int)((d1 / (uint32_t)n) + 2u) * n;
      int w0 = warm / n;
      int total = warm + 2 * n;
      float *buf = malloc((size_t)total * sizeof(float));
      for (int i = 0; i < total; i++) buf[i] = (float)sin(2.0 * M_PI * (double)(i + ph) / 8.0);
      memset(ring, 0, CAP * sizeof(float));
      uint32_t wpos = 0, cur = d0;
      for (int k = 0; k < w0 + 2; k++)
        omx_idelay_apply(buf + k * n, (uint32_t)n, ring, CAP, &wpos, &cur, (k <= w0) ? d0 : d1);
      for (int i = w0 * n + 1; i < (w0 + 1) * n; i++) {
        double v = fabs((double)buf[i] - (double)buf[i - 1]);
        if (v > worst_steady) worst_steady = v;
      }
      for (int i = (w0 + 1) * n; i < (w0 + 2) * n; i++) {
        double v = fabs((double)buf[i] - (double)buf[i - 1]);
        if (v > worst_route) worst_route = v;
      }
      free(buf);
    }
    char what[200];
    snprintf(what, sizeof what, "%.0f Hz: a steady sr/8 probe steps at most sin(pi/4) between samples", (double)sr);
    in_band(worst_steady, 0.700, 0.715, what);
    snprintf(what, sizeof what, "%.0f Hz: the move through the line never steps past the signal's own worst step",
             (double)sr);
    in_band(worst_route, 0.0, 0.760, what);
    rate_done(sr);
  }
  free(ring);
}

/* ---- 5. the ledger ------------------------------------------------------------------------ */

static void test_the_contract_ledger_came_out_empty(void) {
  check(omx_contract_log.checks > 0u, "the contracts were reached");
  for (uint32_t i = 0; i < omx_contract_log.count && i < OMX_CONTRACT_MAX; i++)
    fprintf(stderr, "  contract: %s/%s (%s)\n", omx_contract_log.rec[i].stage, omx_contract_log.rec[i].token,
            omx_contract_log.rec[i].kind);
  check(omx_contract_log.count == 0u, "no contract on the line was violated");
}

int main(void) {
  omx_fx_require_rate_floor();
  test_samples_fixed_points();
  test_samples_round_to_nearest_at_every_rate();
  test_impulse();
  test_zero_is_a_passthrough();
  test_carries_across_blocks();
  test_null_ring_is_a_passthrough();
  test_cross_fade_on_dc_is_flat();
  test_pair_zero_is_bit_identical();
  test_pair_known_sample_count();
  test_pair_mono();
  test_pair_unallocated_is_a_passthrough();
  test_pair_carries_across_blocks();
  test_an_impulse_lands_on_the_declared_sample();
  test_the_pair_gives_each_leg_its_own_delay();
  test_zero_is_an_exact_passthrough_at_every_rate();
  test_a_steady_delay_is_block_size_invariant();
  test_the_re_point_nulls_at_the_antiphase_frequency();
  test_the_re_point_arrives_at_the_new_delay_exactly();
  test_a_move_never_steps_past_the_signal();
  test_the_contract_ledger_came_out_empty();
  printf("fx/idelay: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
