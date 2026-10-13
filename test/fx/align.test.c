// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The input alignment stage's oracle (omx_align.h): the alignment arms of the console's
 * mix_align.test.c, moved with the stage. The rest of that file (the complex spectrum and the
 * analyser ring reads) tests the console's own instrument and stays there.
 *
 * Three claims, through the same omx_align_apply the RT path calls (an order-5 fractional line,
 * smart-alignment §7): it delays by the amount asked, zero is an exact passthrough, and moving it
 * does not click. The fractional half is align_fdelay.test.c's.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_align.h>

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

static void close_to(double got, double want, double tol, const char *what) {
  g_checks++;
  double d = fabs(got - want);
  if (!(d <= tol)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s (got %.9g, want %.9g, |d| %.3g > %.3g)\n", what, got, want, d, tol);
  }
}

static float g_al_l[OMX_ALIGN_CAP], g_al_r[OMX_ALIGN_CAP];

static void arm(struct omx_align_state *al) {
  memset(al, 0, sizeof *al);
  memset(g_al_l, 0, sizeof g_al_l);
  memset(g_al_r, 0, sizeof g_al_r);
  check(omx_align_arm(al, g_al_l, g_al_r), "the alignment line arms over two rings");
}

static void test_the_alignment_stage_delays_by_what_it_is_asked(void) {
  struct omx_align_state al;
  arm(&al);
  /* A move SLEWS toward the target (OMX_ALIGN_SLEW per sample), so settle with silent blocks
   * until it has landed — 7 / (1/32) = 224 samples — then measure on the next. */
  const uint32_t D = 7;
  float buf[64];
  for (int b = 0; b < 5; b++) {
    memset(buf, 0, sizeof buf);
    omx_align_apply(&al, buf, NULL, 64, (float)D);
  }
  check(al.cur == (float)D, "the slew has landed exactly on the target");
  memset(buf, 0, sizeof buf);
  buf[0] = 1.0f;
  omx_align_apply(&al, buf, NULL, 64, (float)D);
  check(buf[D] == 1.0f, "the impulse arrives exactly D samples late, a copy");
  int clean = 1;
  for (uint32_t i = 0; i < 64; i++) {
    if (i != D && buf[i] != 0.0f) clean = 0;
  }
  check(clean, "and nothing arrives anywhere else");
}

static void test_zero_delay_is_an_exact_passthrough(void) {
  struct omx_align_state al;
  arm(&al);
  float buf[32], want[32];
  for (uint32_t i = 0; i < 32; i++) buf[i] = want[i] = sinf((float)i * 0.3f);
  omx_align_apply(&al, buf, NULL, 32, 0.0f);
  int same = 1;
  for (uint32_t i = 0; i < 32; i++) {
    if (buf[i] != want[i]) same = 0;
  }
  check(same, "an unaligned strip is BIT-IDENTICAL, not merely close");
}

static void test_moving_the_alignment_does_not_click(void) {
  struct omx_align_state al;
  arm(&al);
  /* Steady DC through the line, then the delay MOVES mid-show. A step would show up as a
   * discontinuity in the output; the read point slews across the move instead. */
  float buf[128];
  for (uint32_t b = 0; b < 12; b++) {
    for (uint32_t i = 0; i < 128; i++) buf[i] = 0.5f;
    omx_align_apply(&al, buf, NULL, 128, b < 3 ? 10.0f : 40.0f);
  }
  float worst = 0.0f;
  for (uint32_t i = 1; i < 128; i++) {
    float d = fabsf(buf[i] - buf[i - 1]);
    if (d > worst) worst = d;
  }
  close_to((double)worst, 0.0, 1e-3, "a delay MOVE produces no sample-to-sample step");
}

int main(void) {
  test_the_alignment_stage_delays_by_what_it_is_asked();
  test_zero_delay_is_an_exact_passthrough();
  test_moving_the_alignment_does_not_click();

  printf("fx/align: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
