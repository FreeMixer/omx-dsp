// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The OFFLINE pre-hit reverse render's oracle (fx/omx_reverse_render.h; formerly the console's
 * mix_reverse_render.test.c). `make test-fx` runs it.
 *
 * The claim under test is a
 * PLACEMENT, so every arm here is about WHERE energy is, never about how much of it there is —
 * and the two facts the design makes sample-exact (zero at and after the hit, zero more than one
 * window before it) are asserted as exact zeros, because "small" and "absent" are different
 * answers and only one of them is the ruling.
 *
 * The live `reverse` voice is the control arm on the SAME click: it must put its energy on the
 * other side of the hit. One kernel, two placements — that is the design.
 *
 * Every arm runs at the nine RME rates (OMX_FX_RME_RATES, 32 to 192 kHz) over a 2 s region with
 * the click at 1 s.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_reverse_render.h>

#include "fx_rates.h"

static int g_fail = 0, g_checks = 0;
static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

static float SR;     /* the rate the arms run at */
static uint32_t N;   /* 2 s */
static uint32_t HIT; /* the click, at t = 1.000 s */

static struct omx_reverse_render_scratch *make_scratch(uint32_t n, uint32_t w) {
  struct omx_reverse_render_scratch *sc = calloc(1, sizeof(*sc));
  sc->st = calloc(1, sizeof(struct omx_reverb_state));
  sc->pool = calloc(OMX_REVERB_POOL_FLOATS, sizeof(float));
  sc->pool_len = OMX_REVERB_POOL_FLOATS;
  sc->tmp_l = calloc(w, sizeof(float));
  sc->tmp_r = calloc(w, sizeof(float));
  sc->tmp_cap = w;
  sc->wet_l = calloc(n, sizeof(float));
  sc->wet_r = calloc(n, sizeof(float));
  sc->wet_cap = n;
  return sc;
}
static void free_scratch(struct omx_reverse_render_scratch *sc) {
  free(sc->st); free(sc->pool); free(sc->tmp_l); free(sc->tmp_r);
  free(sc->wet_l); free(sc->wet_r); free(sc);
}

/* The reverb row as the operator would hand it to the render: ROOM at the shipped defaults. */
static struct omx_reverb row(float mix, float predelay_ms) {
  struct omx_reverb p = { 0 };
  p.enabled = 1;
  p.algorithm = OMX_REVERB_REVERSE; /* the row the operator is rendering WITH */
  p.size = 0.7f;
  p.damping = 0.5f;
  p.predelay_ms = predelay_ms;
  p.width = 1.0f;
  p.mix = mix;
  p.lowcut = 0.0f;
  p.highcut = 20000.0f;
  p.reverse_ms = 300.0f;
  return p;
}

static float energy(const float *x, uint32_t a, uint32_t b) {
  float e = 0.0f;
  for (uint32_t i = a; i < b; i++) e += x[i] * x[i];
  return e;
}
static uint32_t nonzero_count(const float *x, uint32_t a, uint32_t b) {
  uint32_t k = 0;
  for (uint32_t i = a; i < b; i++) if (x[i] != 0.0f) k++;
  return k;
}
static void click(float *l, float *r, uint32_t n) {
  memset(l, 0, n * sizeof(float));
  memset(r, 0, n * sizeof(float));
  l[HIT] = 1.0f;
  r[HIT] = 1.0f;
}

/*
 * THE RULING, on one click: the swell is in the 300 ms BEFORE the hit, nowhere else, and the dry
 * click is untouched where the operator played it.
 */
static void test_swell_lands_before_the_hit(void) {
  const uint32_t w = omx_reverse_render_window(300.0f, SR);
  check(w == (uint32_t)(0.3 * (double)SR) && w % 2u == 0u, "300 ms is 0.3 x the rate in samples, even");
  float *l = malloc(N * sizeof(float)), *r = malloc(N * sizeof(float));
  struct omx_reverse_render_scratch *sc = make_scratch(N, w);
  struct omx_reverb p = row(0.5f, 0.0f);
  click(l, r, N);
  check(omx_reverse_render(l, r, N, &p, SR, sc) == 0, "the render runs");

  /* 1. The dry click is where it was, to the bit: dry = 1 - mix over the sample it occupied. */
  check(l[HIT] == 0.5f, "the dry click is still at t = 1 s, at exactly (1 - mix)");
  check(r[HIT] == 0.5f, "the dry click is still at t = 1 s on the right leg too");

  /* 2. NOTHING after the hit — exactly zero, not merely small. The reversed-domain response is
   *    causal, so the mirror cannot place a sample before its cause. */
  check(nonzero_count(l, HIT + 1u, N) == 0u, "the wet is EXACTLY zero after the hit (left)");
  check(nonzero_count(r, HIT + 1u, N) == 0u, "the wet is EXACTLY zero after the hit (right)");

  /* 3. NOTHING earlier than one window before the hit — the span is `reverseMs`, enforced. */
  check(nonzero_count(l, 0u, HIT - w + 1u) == 0u, "the wet is EXACTLY zero before [hit - 300 ms]");

  /* 4. The energy that exists is in [0.700, 1.000) s, and it RISES into the hit. */
  const float swell = energy(l, HIT - w, HIT);
  const float first_half = energy(l, HIT - w, HIT - w / 2u);
  const float second_half = energy(l, HIT - w / 2u, HIT);
  check(swell > 1e-4f, "there IS a swell in [0.700, 1.000) s");
  check(second_half > 4.0f * first_half, "the swell RISES into the hit (second half > 4x first)");
  printf("  swell energy %.6g  first half %.6g  second half %.6g  ratio %.2f\n",
         (double)swell, (double)first_half, (double)second_half,
         (double)(second_half / (first_half > 0.0f ? first_half : 1e-30f)));

  free_scratch(sc); free(l); free(r);
}

/*
 * THE CONTROL ARM — the live `reverse` voice on the same click puts its energy on the other side.
 * Without this, a kernel that simply did nothing before the hit would pass every arm above.
 */
static void test_the_live_voice_swells_after_the_hit(void) {
  float *l = malloc(N * sizeof(float)), *r = malloc(N * sizeof(float));
  struct omx_reverb_state *s = calloc(1, sizeof(*s));
  float *pool = calloc(OMX_REVERB_POOL_FLOATS, sizeof(float));
  omx_reverb_state_layout(s, pool, OMX_REVERB_POOL_FLOATS, SR);
  struct omx_reverb p = row(1.0f, 0.0f);
  click(l, r, N);
  for (uint32_t off = 0; off < N; off += 256u) {
    const uint32_t q = N - off < 256u ? N - off : 256u; /* 2 s is not whole blocks at every rate */
    omx_reverb_process(l + off, r + off, q, &p, s, SR);
  }
  const uint32_t w = omx_reverse_render_window(300.0f, SR);
  const float before = energy(l, HIT - w, HIT);
  const float after = energy(l, HIT, HIT + 2u * w);
  check(before == 0.0f, "the LIVE reverse voice has exactly nothing before the hit");
  check(after > 1e-6f, "the LIVE reverse voice has its swell AFTER the hit");
  printf("  live reverse: before %.6g  after %.6g\n", (double)before, (double)after);
  free(pool); free(s); free(l); free(r);
}

/* Pre-delay runs in the REVERSED domain, so it opens a gap between the swell and the hit. */
static void test_predelay_opens_a_gap_before_the_hit(void) {
  const uint32_t w = omx_reverse_render_window(300.0f, SR);
  const uint32_t gap = (uint32_t)(0.030f * SR); /* 30 ms */
  float *l = malloc(N * sizeof(float)), *r = malloc(N * sizeof(float));
  struct omx_reverse_render_scratch *sc = make_scratch(N, w);
  struct omx_reverb p = row(1.0f, 30.0f);
  click(l, r, N);
  check(omx_reverse_render(l, r, N, &p, SR, sc) == 0, "the pre-delayed render runs");
  check(nonzero_count(l, HIT - gap + 1u, HIT + 1u) == 0u,
        "a 30 ms pre-delay leaves the 30 ms before the hit EXACTLY silent");
  check(energy(l, HIT - w, HIT - gap) > 1e-5f, "and the swell is still there, one gap earlier");
  free_scratch(sc); free(l); free(r);
}

/* `mix: 0` is bit-identical dry — the insert's law, kept by the render. A disabled row refuses
 * rather than handing back a copy of the source dressed as a render. */
static void test_mix_zero_is_bit_identical_and_off_refuses(void) {
  const uint32_t w = omx_reverse_render_window(300.0f, SR);
  float *l = malloc(N * sizeof(float)), *r = malloc(N * sizeof(float));
  float *ref = malloc(N * sizeof(float));
  struct omx_reverse_render_scratch *sc = make_scratch(N, w);
  struct omx_reverb p = row(0.0f, 0.0f);
  click(l, r, N);
  memcpy(ref, l, N * sizeof(float));
  check(omx_reverse_render(l, r, N, &p, SR, sc) == 0, "mix 0 renders");
  check(memcmp(ref, l, N * sizeof(float)) == 0, "mix 0 leaves the region BIT-IDENTICAL");

  struct omx_reverb off = row(0.5f, 0.0f);
  off.enabled = 0;
  check(omx_reverse_render(l, r, N, &off, SR, sc) == -1, "a reverb that is OFF refuses the render");

  /* A scratch too small for the window is a refusal, never a short render. */
  sc->tmp_cap = w - 1u;
  check(omx_reverse_render(l, r, N, &p, SR, sc) == -1, "a scratch shorter than the window refuses");
  sc->tmp_cap = w;
  free_scratch(sc); free(l); free(r); free(ref);
}

/*
 * The Bartlett grid covers the REGION'S LAST SAMPLE at full weight — which is what the block
 * starting at `-H` is for. In the reversed domain that sample is the FIRST one, and a loop that
 * started at zero would render it under a triangle still ramping up from nothing.
 *
 * Measured as a RATIO between two identical clicks, one at the region's last sample and one in
 * its middle, because an absolute energy here would be a number nobody could judge. Sabotage-
 * verified: with the leading block removed the end click's swell falls from 0.0611 to 0.0149
 * (a quarter) while the middle click's is unchanged at 0.0468 — this arm reds, alone.
 */
static void test_the_regions_last_sample_is_covered(void) {
  const uint32_t w = omx_reverse_render_window(300.0f, SR);
  float *l = malloc(N * sizeof(float)), *r = malloc(N * sizeof(float));
  struct omx_reverse_render_scratch *sc = make_scratch(N, w);
  struct omx_reverb p = row(1.0f, 0.0f); /* wet only, so what is measured IS the wet */
  float swell[2];
  const uint32_t hits[2] = { HIT, N - 1u };
  for (int k = 0; k < 2; k++) {
    memset(l, 0, N * sizeof(float));
    memset(r, 0, N * sizeof(float));
    l[hits[k]] = 1.0f; r[hits[k]] = 1.0f;
    check(omx_reverse_render(l, r, N, &p, SR, sc) == 0, "the click renders");
    swell[k] = energy(l, hits[k] - w, hits[k]);
  }
  check(swell[1] > 0.5f * swell[0],
        "a click at the region's LAST sample swells like one in the middle");
  printf("  swell mid %.6g  swell at the region's end %.6g  ratio %.2f\n",
         (double)swell[0], (double)swell[1], (double)(swell[1] / swell[0]));
  free_scratch(sc); free(l); free(r);
}

int main(void) {
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    SR = OMX_FX_RME_RATES[ri];
    N = 2u * (uint32_t)SR;
    HIT = (uint32_t)SR;
    printf(" %.0f Hz\n", (double)SR);
    test_swell_lands_before_the_hit();
    test_the_live_voice_swells_after_the_hit();
    test_predelay_opens_a_gap_before_the_hit();
    test_mix_zero_is_bit_identical_and_off_refuses();
    test_the_regions_last_sample_is_covered();
  }
  printf("fx/reverse_render: %d checks, %d failed, %u rates\n", g_checks, g_fail, (unsigned)OMX_FX_RME_RATE_COUNT);
  return g_fail ? 1 : 0;
}
