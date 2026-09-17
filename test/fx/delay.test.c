// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Standalone unit test for the native FX delay kernel (mix_delay.h). Compile + run:
 *   cc -Wall -Wextra -O2 -o build/mix_delay_test src/mix_delay.test.c -lm && ./build/mix_delay_test
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mix_delay.h"

static int g_fail = 0, g_checks = 0;
static void check(int cond, const char *what) {
  g_checks++; if (!cond) { g_fail++; fprintf(stderr, "FAIL: %s\n", what); }
}
static void close_to(float got, float want, const char *what) {
  g_checks++; float d = fabsf(got - want);
  if (d > 1e-4f) { g_fail++; fprintf(stderr, "FAIL: %s got %.6f want %.6f\n", what, got, want); }
}

static struct omx_fx_delay_state *make_state(uint32_t cap) {
  struct omx_fx_delay_state *s = calloc(1, sizeof(*s));
  s->ring_l = calloc(cap, sizeof(float));
  s->ring_r = calloc(cap, sizeof(float));
  s->cap = cap; s->wpos = 0; s->damp_l = s->damp_r = 0.0f;
  return s;
}
static void free_state(struct omx_fx_delay_state *s) { free(s->ring_l); free(s->ring_r); free(s); }

static void test_division_ms(void) {
  close_to(omx_bpm_division_ms(120.0f, 1.0f), 500.0f, "quarter @120 = 500ms");
  close_to(omx_bpm_division_ms(120.0f, 0.5f), 250.0f, "eighth @120 = 250ms");
  close_to(omx_bpm_division_ms(120.0f, 0.75f), 375.0f, "dotted-eighth @120 = 375ms");
  close_to(omx_bpm_division_ms(120.0f, 1.0f / 3.0f), 166.6667f, "eighth-triplet @120");
}

static void test_impulse_reappears_at_time(void) {
  struct omx_fx_delay_state *s = make_state(1024);
  struct omx_fx_delay p = { .enabled = 1, .d_l = 100, .d_r = 100, .feedback = 0.0f,
                            .mix = 1.0f, .tone = 0.0f, .pingpong = 0 };
  float l[256], r[256];
  memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r));
  l[0] = 1.0f; r[0] = 1.0f;
  omx_fx_delay_process(l, r, 256, &p, s, 48000.0f);
  // 100% wet, no feedback: the impulse reappears exactly 100 frames later, dry gone.
  close_to(l[0], 0.0f, "wet-only: dry sample suppressed at t=0");
  close_to(l[100], 1.0f, "impulse reappears at exactly d=100 frames (L)");
  close_to(r[100], 1.0f, "impulse reappears at exactly d=100 frames (R)");
  free_state(s);
}

static void test_feedback_decays_geometrically(void) {
  struct omx_fx_delay_state *s = make_state(64);
  struct omx_fx_delay p = { .enabled = 1, .d_l = 10, .d_r = 10, .feedback = 0.5f,
                            .mix = 1.0f, .tone = 0.0f, .pingpong = 0 };
  float l[64], r[64];
  memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r));
  l[0] = 1.0f; r[0] = 1.0f;
  omx_fx_delay_process(l, r, 64, &p, s, 48000.0f);
  close_to(l[10], 1.0f, "first echo");
  close_to(l[20], 0.5f, "second echo = fb^1");
  close_to(l[30], 0.25f, "third echo = fb^2");
  free_state(s);
}

static void test_mix_zero_is_bit_identical_dry(void) {
  struct omx_fx_delay_state *s = make_state(64);
  struct omx_fx_delay p = { .enabled = 1, .d_l = 5, .d_r = 5, .feedback = 0.7f,
                            .mix = 0.0f, .tone = 0.3f, .pingpong = 1 };
  float l[32], r[32], l0[32];
  for (int i = 0; i < 32; i++) { l[i] = sinf(i * 0.3f); r[i] = cosf(i * 0.2f); l0[i] = l[i]; }
  omx_fx_delay_process(l, r, 32, &p, s, 48000.0f);
  for (int i = 0; i < 32; i++) check(l[i] == l0[i], "mix=0 is bit-identical dry");
  free_state(s);
}

static void test_feedback_clamp_prevents_runaway(void) {
  struct omx_fx_delay_state *s = make_state(32);
  struct omx_fx_delay p = { .enabled = 1, .d_l = 4, .d_r = 4, .feedback = 5.0f, /* absurd */
                            .mix = 1.0f, .tone = 0.0f, .pingpong = 0 };
  float l[64], r[64];
  memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r));
  l[0] = 1.0f; r[0] = 1.0f;
  for (int blk = 0; blk < 4; blk++) omx_fx_delay_process(l + blk * 16, r + blk * 16, 16, &p, s, 48000.0f);
  for (int i = 0; i < 64; i++) check(fabsf(l[i]) <= 1.001f, "clamped feedback never grows > 1");
  free_state(s);
}

static void test_pingpong_alternates_legs(void) {
  struct omx_fx_delay_state *s = make_state(64);
  struct omx_fx_delay p = { .enabled = 1, .d_l = 8, .d_r = 8, .feedback = 0.6f,
                            .mix = 1.0f, .tone = 0.0f, .pingpong = 1 };
  float l[64], r[64];
  memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r));
  l[0] = 1.0f; /* feed L only */
  omx_fx_delay_process(l, r, 64, &p, s, 48000.0f);
  // ping-pong cross-feeds: the L input's repeats bounce onto the R leg on the next tap.
  check(fabsf(r[16]) > 0.1f, "ping-pong: energy crosses to the R leg on the 2nd tap");
  free_state(s);
}

/* A bypassed-then-re-enabled delay must start clean. The mixer's re-enable path zeroes the rings and
 * resets wpos + the tone-damp state; this validates that reset primitive: after echoes are buffered,
 * that clear makes a silence-fed delay emit nothing (vs. a non-cleared state that bursts the buffered
 * echoes). */
static void test_reenable_clears_stale_tail(void) {
  const uint32_t CAP = 1024;
  struct omx_fx_delay p = { .enabled = 1, .d_l = 100, .d_r = 100, .feedback = 0.6f,
                            .mix = 1.0f, .tone = 0.0f, .pingpong = 0 };
  float l[256], r[256];
  /* dirty reference: build echoes, feed silence WITHOUT clearing -> buffered echoes still emerge. */
  struct omx_fx_delay_state *dirty = make_state(CAP);
  memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); l[0] = 1.0f; r[0] = 1.0f;
  omx_fx_delay_process(l, r, 256, &p, dirty, 48000.0f);
  memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); /* silence in */
  omx_fx_delay_process(l, r, 256, &p, dirty, 48000.0f);
  float dirtyE = 0.0f; for (int i = 0; i < 256; i++) dirtyE += l[i] * l[i] + r[i] * r[i];
  check(dirtyE > 1e-6f, "without clearing, a re-fed delay still emits buffered echoes");
  free_state(dirty);
  /* clean: build echoes, then apply the mixer's clear (memset rings + reset wpos/damp). */
  struct omx_fx_delay_state *s = make_state(CAP);
  memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); l[0] = 1.0f; r[0] = 1.0f;
  omx_fx_delay_process(l, r, 256, &p, s, 48000.0f);
  memset(s->ring_l, 0, CAP * sizeof(float)); memset(s->ring_r, 0, CAP * sizeof(float));
  s->wpos = 0; s->damp_l = s->damp_r = 0.0f;
  memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); /* silence in */
  omx_fx_delay_process(l, r, 256, &p, s, 48000.0f);
  float cleanE = 0.0f; for (int i = 0; i < 256; i++) cleanE += l[i] * l[i] + r[i] * r[i];
  check(cleanE == 0.0f, "clearing rings + resetting wpos/damp makes a re-enabled delay start silent");
  free_state(s);
}

/* An ENABLED delay at d==0 must be a clean passthrough (output == input), NOT a full-ring echo of
 * the value sitting `cap` samples ago in the write slot. Regression: reading ring[w] before writing
 * it echoed ~2 s of stale audio. Uses full wet mix + feedback + tone to prove the passthrough holds
 * regardless of the wet path (the d==0 tap is the input itself, so dry+wet reconstruct it). */
static void test_zero_delay_is_passthrough(void) {
  struct omx_fx_delay_state *s = make_state(1024);
  struct omx_fx_delay p = { .enabled = 1, .d_l = 0, .d_r = 0, .feedback = 0.7f,
                            .mix = 1.0f, .tone = 0.4f, .pingpong = 1 };
  float l[64], r[64], l0[64], r0[64];
  for (int i = 0; i < 64; i++) { l[i] = sinf(i * 0.31f); r[i] = cosf(i * 0.19f); l0[i] = l[i]; r0[i] = r[i]; }
  /* prime the ring with a prior block so ring[w] holds non-zero stale audio (the echo source). */
  omx_fx_delay_process(l, r, 64, &p, s, 48000.0f);
  for (int i = 0; i < 64; i++) { l[i] = l0[i]; r[i] = r0[i]; }
  omx_fx_delay_process(l, r, 64, &p, s, 48000.0f);
  for (int i = 0; i < 64; i++) {
    close_to(l[i], l0[i], "d==0 is exact passthrough (L), no stale-ring echo");
    close_to(r[i], r0[i], "d==0 is exact passthrough (R), no stale-ring echo");
  }
  free_state(s);
}

int main(void) {
  test_division_ms();
  test_impulse_reappears_at_time();
  test_feedback_decays_geometrically();
  test_mix_zero_is_bit_identical_dry();
  test_feedback_clamp_prevents_runaway();
  test_pingpong_alternates_legs();
  test_reenable_clears_stale_tail();
  test_zero_delay_is_passthrough();
  printf("mix_delay: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
