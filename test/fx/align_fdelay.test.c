// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * align_fdelay.test.c (formerly the console's mix_align_fdelay.test.c) — the STRIP ALIGNMENT stage
 * (omx_align.h) on the fractional line, against closed forms, at all six declared rates
 * (44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz).
 *
 * Law: the smart alignment spec §7 (#863), on the RULED
 * 2026-09-20-fractional-delay-line.md. The primitive's own oracle is test/kernels/fdelay.c; this file
 * holds what the STAGE adds on top of it — the ms → samples target, where it settles, the slew —
 * and measures each against a number derived here, never one recorded from a run.
 *
 *   A — THE TARGET. `omx_align_target` is `ms × rate / 1000`, unrounded at 88.2 kHz and up, the
 *       nearest whole sample below it and under the kernel's 2-sample reach; 0.5 ms at 96 kHz is
 *       EXACTLY 48 (an exact-copy read, not a kernel 4e-6 off whole); the 100 ms ceiling is
 *       reachable at every rate inside `OMX_ALIGN_CAP`, which is the primitive's own
 *       `omx_fdelay_cap_for`; nonsense reads as 0 or saturates, never wraps.
 *   B — THE DELAY THE AUDIO TAKES. A 1 kHz tone through the settled line, its phase delay measured
 *       by a one-bin DFT over exactly 100 cycles (no leakage at any of the six rates): the
 *       measured delay is the target within 0.001 sample, and the level within 0.01 dB.
 *   C — THE SLEW. (1) a move lands EXACTLY, after |Δ| / OMX_ALIGN_SLEW samples, and an impulse
 *       then arrives on the target sample; (2) NO HOLE: the delay review's D-2 probe — a tone at
 *       rate/8 moved by half its period, the condition where two taps are antiphase — keeps every
 *       one-period window within 2 % of steady, while the console's route delay (its
 *       one-block cross-fade, the kernel this stage left; route_fade below restates its loop) is
 *       measured on the same move as the POSITIVE CONTROL and must show the hole; (3) no sample-to-sample step beyond the
 *       signal's own steepest at the slewed pitch; (4) a move is BLOCK-SIZE INVARIANT — the same
 *       move in blocks of 1, 7, 64 and 1024 frames is bit-identical.
 *   D — ONE TARGET, TWO LEGS: each leg is bit-identical to a mono line given that leg alone.
 *
 * Pure C, -lm, no PipeWire. Built with -DOMX_CONTRACTS: every contract on the path is evaluated
 * and the ledger must come out empty, over a floor of evaluations.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_align.h>
#include <omxdsp/omx_lookahead.h>

/* The POSITIVE CONTROL: the console's route delay (omx_delay_apply in its mix_dsp.h), a
 * whole-sample ring that re-points by blending the old and the new tap linearly across one block.
 * The console keeps it; arm C needs only its loop, restated here over a ring of ROUTE_CAP. */
#define ROUTE_CAP 64u
static void route_fade(float *buf, uint32_t n, float *ring, uint32_t cap, uint32_t *wpos, uint32_t *cur,
                       uint32_t tgt) {
  uint32_t w = *wpos;
  const uint32_t d0 = *cur;
  if (d0 == tgt) {
    for (uint32_t i = 0; i < n; i++) {
      ring[w] = buf[i];
      buf[i] = ring[omx_lookahead_back(w, tgt, cap)];
      w = omx_lookahead_fwd(w, 1u, cap);
    }
  } else {
    for (uint32_t i = 0; i < n; i++) {
      ring[w] = buf[i];
      const uint32_t r0 = omx_lookahead_back(w, d0, cap);
      const uint32_t r1 = omx_lookahead_back(w, tgt, cap);
      const float t = (float)(i + 1) / (float)n; /* 0 -> 1 across the block */
      buf[i] = ring[r0] * (1.0f - t) + ring[r1] * t;
      w = omx_lookahead_fwd(w, 1u, cap);
    }
    *cur = tgt;
  }
  *wpos = w;
}

static int g_checks = 0, g_failed = 0;

static void check(int ok, const char *what) {
  g_checks++;
  if (!ok) {
    printf("FAIL %s\n", what);
    g_failed++;
  }
}

/* The declared rates (OMX_DECLARED_RATES, generated from core's STANDARD_SAMPLE_RATES — never a
 * local list). */
#define NRATES ((int)OMX_DECLARED_RATE_COUNT)
#define RATE(i) ((double)OMX_DECLARED_RATES[(i)])

static float g_rl[OMX_ALIGN_CAP], g_rr[OMX_ALIGN_CAP];

static void arm(struct omx_align_state *a) {
  memset(a, 0, sizeof *a);
  memset(g_rl, 0, sizeof g_rl);
  memset(g_rr, 0, sizeof g_rr);
  if (!omx_align_arm(a, g_rl, g_rr)) {
    printf("FAIL the alignment line did not arm\n");
    g_failed++;
  }
}

/* ---- A: the target --------------------------------------------------------------------- */
static void arm_a_the_target(void) {
  char what[200];
  check(OMX_ALIGN_CAP == omx_fdelay_cap_for(19200.0f, OMX_ALIGN_ORDER),
        "OMX_ALIGN_CAP is the primitive's own ring for 100 ms at 192 kHz");
  for (int ri = 0; ri < NRATES; ri++) {
    const float sr = (float)RATE(ri);
    const float ceil_t = omx_align_target((float)OMX_ALIGN_MAX_MS, sr);
    snprintf(what, sizeof what, "%.0f Hz: 100 ms is %g samples, unclamped, inside the ring",
             RATE(ri), (double)ceil_t);
    check((double)ceil_t == RATE(ri) / 10.0
              && ceil_t <= omx_fdelay_max_delay(OMX_ALIGN_CAP, OMX_ALIGN_ORDER), what);
    snprintf(what, sizeof what, "%.0f Hz: nonsense reads as 0 or saturates at the ceiling",
             RATE(ri));
    check(omx_align_target(-1.0f, sr) == 0.0f && omx_align_target(NAN, sr) == 0.0f
              && omx_align_target(INFINITY, sr) == 0.0f && omx_align_target(1e9f, sr) == ceil_t,
          what);
    /* 0.0703 ms: fractional at every rate; settles fractional at 88.2 kHz and up. */
    const double exact = 0.0703 * RATE(ri) / 1000.0;
    const float t = omx_align_target(0.0703f, sr);
    if (RATE(ri) >= (double)OMX_ALIGN_FRACTION_MIN_RATE) {
      snprintf(what, sizeof what, "%.0f Hz: 0.0703 ms settles at %.5f samples, unrounded",
               RATE(ri), (double)t);
      check(fabs((double)t - exact) < 1e-4 && t != floorf(t), what);
    } else {
      snprintf(what, sizeof what, "%.0f Hz: 0.0703 ms settles at the nearest whole sample, %g",
               RATE(ri), (double)t);
      check((double)t == floor(exact + 0.5), what);
    }
    /* Under the kernel's reach: 0.0146 ms is 0.64 / 0.70 / 1.29 / 1.40 / 2.58 / 2.80 samples. */
    const double under = 0.0146 * RATE(ri) / 1000.0;
    const float tu = omx_align_target(0.0146f, sr);
    snprintf(what, sizeof what, "%.0f Hz: 0.0146 ms (%.3f samples) settles at %g", RATE(ri),
             under, (double)tu);
    if (under < 2.0 || RATE(ri) < (double)OMX_ALIGN_FRACTION_MIN_RATE)
      check((double)tu == floor(under + 0.5), what);
    else
      check(fabs((double)tu - under) < 1e-4, what);
  }
  check(omx_align_target(0.5f, 96000.0f) == 48.0f,
        "96000 Hz: 0.5 ms is EXACTLY 48 samples — the exact-copy branch, not a kernel");
  if (omx_contract_log.count != 0u) {
    printf("FAIL arm A: %u contract violations\n", omx_contract_log.count);
    g_failed++;
    omx_contract_log.count = 0u;
  }
}

/* ---- B: the delay the audio takes ------------------------------------------------------ */
static void arm_b_the_delay_the_audio_takes(void) {
  static const float MS[3] = {0.0703f, 0.1234f, 0.0333f};
  char what[240];
  for (int ri = 0; ri < NRATES; ri++) {
    const double sr = RATE(ri);
    const double w = 2.0 * M_PI * 1000.0 / sr;
    const uint32_t L = (uint32_t)(sr / 10.0); /* 100 cycles of 1 kHz, exactly */
    float *x = malloc(sizeof(float) * 2u * L), *y = malloc(sizeof(float) * 2u * L);
    for (int k = 0; k < 3; k++) {
      struct omx_align_state a;
      arm(&a);
      const float tgt = omx_align_target(MS[k], (float)sr);
      a.cur = tgt; /* settled: the slew is arm C */
      for (uint32_t i = 0; i < 2u * L; i++) x[i] = y[i] = (float)(0.5 * sin(w * (double)i));
      for (uint32_t o = 0; o < 2u * L; o += 256u) {
        const uint32_t q = (2u * L - o) < 256u ? (2u * L - o) : 256u;
        omx_align_apply(&a, y + o, NULL, q, tgt);
      }
      double complex X = 0.0, Y = 0.0;
      for (uint32_t i = L; i < 2u * L; i++) {
        const double complex e = cexp(-I * w * (double)i);
        X += (double)x[i] * e;
        Y += (double)y[i] * e;
      }
      const double tau = -carg(Y / X) / w;
      const double db = 20.0 * log10(cabs(Y) / cabs(X));
      snprintf(what, sizeof what,
               "%.0f Hz: %.4f ms settles at %.5f samples; the tone is delayed %.5f, level %+.4f dB",
               sr, (double)MS[k], (double)tgt, tau, db);
      check(fabs(tau - (double)tgt) <= 1e-3 && fabs(db) <= 0.01, what);
      printf("  B %6.0f Hz  %.4f ms  target %9.5f  measured %9.5f  %+.5f dB\n", sr,
             (double)MS[k], (double)tgt, tau, db);
    }
    free(x);
    free(y);
  }
  if (omx_contract_log.count != 0u) {
    printf("FAIL arm B: %u contract violations\n", omx_contract_log.count);
    g_failed++;
    omx_contract_log.count = 0u;
  }
}

/* ---- C: the slew ----------------------------------------------------------------------- */
static void arm_c_lands_exactly(void) {
  char what[200];
  struct omx_align_state a;
  arm(&a);
  float buf[64];
  /* Land on 10, then move to 40: exactly 30 / (1/32) = 960 samples. */
  for (int b = 0; b < 6; b++) {
    memset(buf, 0, sizeof buf);
    omx_align_apply(&a, buf, NULL, 64, 10.0f);
  }
  uint32_t moved = 0;
  while (a.cur != 40.0f && moved < 4096u) {
    memset(buf, 0, sizeof buf);
    omx_align_apply(&a, buf, NULL, 1, 40.0f);
    moved++;
  }
  snprintf(what, sizeof what, "a 10 -> 40 move lands exactly after %u samples (960 declared)",
           moved);
  check(a.cur == 40.0f && moved == (uint32_t)(30.0f / OMX_ALIGN_SLEW), what);
  memset(buf, 0, sizeof buf);
  buf[0] = 1.0f;
  omx_align_apply(&a, buf, NULL, 64, 40.0f);
  int clean = 1;
  for (int i = 0; i < 64; i++)
    if (i != 40 && buf[i] != 0.0f) clean = 0;
  check(buf[40] == 1.0f && clean, "after landing, an impulse arrives on sample 40 and nowhere else");
}

/* The worst one-period RMS of `y[from, to)` (period `p`), as a fraction of `steady`. */
static double worst_window(const float *y, int from, int to, int p, double steady) {
  double worst = 1e9;
  for (int s = from; s + p <= to; s++) {
    double e = 0.0;
    for (int i = s; i < s + p; i++) e += (double)y[i] * (double)y[i];
    const double r = sqrt(e / p) / steady;
    if (r < worst) worst = r;
  }
  return worst;
}

static void arm_c_no_hole_and_no_click(void) {
  char what[240];
  for (int ri = 0; ri < NRATES; ri++) {
    const double sr = RATE(ri);
    /* D-2's probe: rate/8, period 8 samples; a move of 4 samples puts the taps antiphase. Every
     * one of the eight phases the probe can land on, worst taken: a step's size depends on where
     * in the waveform the move falls (the delay review's FX-delay arm, same sweep). */
    const int P = 8, BLK = 64, NB = 40, LEN = BLK * NB;
    const double w = 2.0 * M_PI / (double)P;
    const double steady = 0.5 / sqrt(2.0);
    const int from = 10 * BLK - P, to = LEN; /* the move, and well past its 128-sample landing */
    double line = 1e9, fade = 1e9, worst = 0.0;
    float *y = malloc(sizeof(float) * LEN), *z = malloc(sizeof(float) * LEN);
    float *ring = calloc(ROUTE_CAP, sizeof(float));
    for (int ph = 0; ph < 8; ph++) {
      for (int i = 0; i < LEN; i++)
        y[i] = z[i] = (float)(0.5 * sin(w * (double)i + 0.3 + (double)ph * M_PI / 4.0));
      struct omx_align_state a;
      arm(&a);
      memset(ring, 0, sizeof(float) * ROUTE_CAP);
      uint32_t wpos = 0, cur = 20u;
      a.cur = 20.0f;
      /* d0 = 20 (whole at every rate), d1 = 24: 10 blocks steady, then the move. */
      for (int b = 0; b < NB; b++) {
        const float t = b < 10 ? 20.0f : 24.0f;
        omx_align_apply(&a, y + b * BLK, NULL, (uint32_t)BLK, t);
        route_fade(z + b * BLK, (uint32_t)BLK, ring, ROUTE_CAP, &wpos, &cur, (uint32_t)t);
      }
      const double l = worst_window(y, from, to, P, steady);
      const double f = worst_window(z, from, to, P, steady);
      if (l < line) line = l;
      if (f < fade) fade = f;
      for (int i = from + 1; i < to; i++) {
        const double d = fabs((double)y[i] - (double)y[i - 1]);
        if (d > worst) worst = d;
      }
    }
    snprintf(what, sizeof what,
             "%.0f Hz: an antiphase re-point keeps every period at %.4f of steady (>= 0.98); "
             "the one-block cross-fade drops to %.4f",
             sr, line, fade);
    check(line >= 0.98, what);
    snprintf(what, sizeof what,
             "%.0f Hz: POSITIVE CONTROL — the measure sees the cross-fade's hole (%.4f < 0.2)", sr,
             fade);
    check(fade < 0.2, what);
    /* No click: no step beyond the steepest a tone at the slewed pitch takes, plus the kernel's
     * own error. A tone of peak A at w(1 + SLEW) steps at most 2A sin(w(1 + SLEW)/2). */
    const double bound = 2.0 * 0.5 * sin(w * (1.0 + (double)OMX_ALIGN_SLEW) / 2.0) + 2e-3;
    snprintf(what, sizeof what,
             "%.0f Hz: the move's worst step over 8 phases, %.4f, is inside the slewed tone's %.4f",
             sr, worst, bound);
    check(worst <= bound, what);
    printf("  C %6.0f Hz  worst period %.4f of steady (cross-fade %.4f), worst step %.4f <= %.4f\n",
           sr, line, fade, worst, bound);
    free(y);
    free(z);
    free(ring);
  }
}

static void arm_c_block_size_invariant(void) {
  static const uint32_t BS[4] = {1u, 7u, 64u, 1024u};
  /* The target switches at 7168 = lcm(7, 64, 1024): the same SAMPLE for every block size. */
  const uint32_t LEN = 16384u, SWITCH = 7168u;
  float *ref = malloc(sizeof(float) * LEN), *got = malloc(sizeof(float) * LEN);
  for (int ri = 0; ri < NRATES; ri++) {
    const float sr = (float)RATE(ri);
    for (int b = 0; b < 4; b++) {
      struct omx_align_state a;
      arm(&a);
      float *y = b == 0 ? ref : got;
      for (uint32_t i = 0; i < LEN; i++) y[i] = sinf(0.05f * (float)i) + 0.25f * sinf(0.71f * (float)i);
      /* A fractional move mid-run: 0.0703 ms, then 0.2381 ms from frame SWITCH. */
      for (uint32_t o = 0; o < LEN; o += BS[b]) {
        const uint32_t q = (LEN - o) < BS[b] ? (LEN - o) : BS[b];
        omx_align_apply(&a, y + o, NULL, q, omx_align_target(o < SWITCH ? 0.0703f : 0.2381f, sr));
      }
      if (b > 0) {
        char what[200];
        snprintf(what, sizeof what,
                 "%.0f Hz: a fractional move in blocks of %u is bit-identical to blocks of 1",
                 RATE(ri), BS[b]);
        check(memcmp(ref, got, sizeof(float) * LEN) == 0, what);
      }
    }
  }
  free(ref);
  free(got);
}

/* ---- D: one target, two legs ----------------------------------------------------------- */
static void arm_d_one_target_two_legs(void) {
  enum { LEN = 4096 };
  static float l[LEN], r[LEN], m[LEN];
  static float ml[OMX_ALIGN_CAP], mr[OMX_ALIGN_CAP];
  for (int i = 0; i < LEN; i++) {
    l[i] = sinf(0.031f * (float)i);
    r[i] = m[i] = 0.7f * sinf(0.113f * (float)i + 1.0f);
  }
  struct omx_align_state a, mono;
  arm(&a);
  memset(&mono, 0, sizeof mono);
  memset(ml, 0, sizeof ml);
  memset(mr, 0, sizeof mr);
  omx_align_arm(&mono, ml, mr);
  for (int o = 0; o < LEN; o += 128) {
    const float t = omx_align_target(o < 1024 ? 0.0911f : 0.0402f, 96000.0f);
    omx_align_apply(&a, l + o, r + o, 128u, t);
    omx_align_apply(&mono, m + o, NULL, 128u, t);
  }
  check(memcmp(r, m, sizeof r) == 0,
        "the right leg is bit-identical to a mono line given that leg alone, through a move");
}

int main(void) {
  printf("fx/align_fdelay: the strip alignment stage on the order-5 line, six rates\n");
  arm_a_the_target();
  arm_b_the_delay_the_audio_takes();
  arm_c_lands_exactly();
  arm_c_no_hole_and_no_click();
  arm_c_block_size_invariant();
  arm_d_one_target_two_legs();
  printf("fx/align_fdelay: %d checks, %d failed; %u contracts evaluated, %u violations\n",
         g_checks, g_failed, omx_contract_log.checks, omx_contract_log.count);
  if (omx_contract_log.count != 0u) g_failed++;
  if (omx_contract_log.checks < 10000u) {
    printf("FAIL the battery evaluated %u contracts — floor is 10000\n", omx_contract_log.checks);
    g_failed++;
  }
  printf("INTEGRATION-CHECKS %d\n", g_checks);
  return g_failed == 0 ? 0 : 1;
}
