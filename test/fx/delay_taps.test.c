// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * delay_taps.test.c — the FX delay's read taps (omx_delay.h) at every rate in OMX_DECLARED_RATES.
 *
 * Spec: OpenMixer docs/design/specs/2026-09-26-tap-delay-variants.md §6.
 *
 *   A — ONE TAP IS THE ONE-TAP DELAY (L1). Noise and a click train through the frozen one-tap
 *       block (fixtures/delay_one_tap_reference.h), omx_fx_delay_process, and
 *       omx_fx_delay_process_taps with ntaps 0 and with one tap at gains (1, 1), over a grid of
 *       feedback, tone, ping-pong, mix and per-leg times, in uneven blocks: memcmp.
 *   B — TAP POSITIONS. An impulse, wet only, no feedback: each tap lands on
 *       omx_fxdelay_ms_to_samples(T * num / den) and carries its gain pair bit for bit, and every
 *       other sample is zero; every member of the spec's factor set at bases 1, 300 and 2000 ms.
 *   C — FEEDBACK FROM THE LONGEST TAP. Two taps, the long one silent in the wet: the regenerations
 *       fall at tap 0 plus multiples of the LONGEST tap's delay, at fb^m.
 *   D — CLAMP. A 2000 ms base at factor 3 is 2000 ms and says so; a tap asked past the ring reads
 *       cap - 1.
 *   F — ROUNDING ONCE. At a base that is not a whole number of samples (100.01 ms at 44.1 kHz)
 *       each tap is round(T * num / den * sr / 1000), never num / den times the rounded base, and
 *       the arm shows the two differ for some factor so it can tell them apart.
 *   G — BYPASS, BLOCKS, LEGS. Four taps at mix 0 are the dry block; one block equals random splits
 *       of it; pans at 0 with identical legs give L == R; the gain pair is the balance law.
 *
 * Contracts are compiled in and the last arm asserts the ledger came out empty.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_delay.h>

#include "fixtures/delay_one_tap_reference.h"
#include "fx_rates.h"

static int g_fail = 0, g_checks = 0;
static void check(int cond, const char *what, float sr) {
  g_checks++;
  if (!cond) { g_fail++; fprintf(stderr, "FAIL @%.0f Hz: %s\n", (double)sr, what); }
}

/** The spec's factor set (DELAY_TAP_FACTORS): straight, dotted and triplet relations of a base. */
static const uint32_t FACTORS[][2] = {
  {1, 4}, {1, 3}, {3, 8}, {1, 2}, {2, 3}, {3, 4}, {1, 1}, {4, 3}, {3, 2}, {2, 1}, {3, 1},
};
#define NFACTORS (sizeof FACTORS / sizeof FACTORS[0])

static void fresh(struct omx_fx_delay_state *s, uint32_t cap) {
  if (s->ring_l == NULL) {
    s->ring_l = malloc(OMX_FXDELAY_CAP * sizeof(float));
    s->ring_r = malloc(OMX_FXDELAY_CAP * sizeof(float));
  }
  memset(s->ring_l, 0, OMX_FXDELAY_CAP * sizeof(float));
  memset(s->ring_r, 0, OMX_FXDELAY_CAP * sizeof(float));
  s->cap = cap; s->wpos = 0u; s->damp_l = 0.0f; s->damp_r = 0.0f;
}
static void release(struct omx_fx_delay_state *s) { free(s->ring_l); free(s->ring_r); s->ring_l = s->ring_r = NULL; }

static uint32_t lcg_next(uint32_t *x) { *x = *x * 1664525u + 1013904223u; return *x; }

/** Noise at half scale with a full-scale click every 4801 frames, on both legs (R inverted, quieter). */
static void stimulus(float *l, float *r, uint32_t n, uint32_t seed) {
  uint32_t x = seed;
  for (uint32_t i = 0; i < n; i++) {
    float v = (float)(int32_t)(lcg_next(&x) >> 8) / 16777216.0f - 0.25f;
    l[i] = i % 4801u == 0u ? 1.0f : v;
    r[i] = i % 4801u == 0u ? -0.5f : -0.5f * v;
  }
}

enum path { REF, PLAIN, TAPS_ZERO, TAPS_ONE };

/** Render `n` frames through one path, in blocks drawn from `seed` (1 .. 600 frames). */
static void render(enum path which, float *l, float *r, uint32_t n, const struct omx_fx_delay *p,
                   struct omx_fx_delay_state *s, float sr, uint32_t seed) {
  struct omx_fx_delay_taps zero, one;
  memset(&zero, 0, sizeof zero);
  memset(&one, 0, sizeof one);
  one.ntaps = 1u; one.gl[0] = 1.0f; one.gr[0] = 1.0f;
  uint32_t x = seed;
  for (uint32_t o = 0; o < n;) {
    uint32_t q = 1u + lcg_next(&x) % 600u;
    if (q > n - o) q = n - o;
    switch (which) {
    case REF: omx_fx_delay_one_tap_reference(l + o, r + o, q, p, s, sr); break;
    case PLAIN: omx_fx_delay_process(l + o, r + o, q, p, s, sr); break;
    case TAPS_ZERO: omx_fx_delay_process_taps(l + o, r + o, q, p, &zero, s, sr); break;
    case TAPS_ONE: omx_fx_delay_process_taps(l + o, r + o, q, p, &one, s, sr); break;
    }
    o += q;
  }
}

static void arm_a_one_tap_is_the_one_tap_delay(float sr, struct omx_fx_delay_state *s) {
  static const float FB[] = {0.0f, 0.5f, 0.99f}, TONE[] = {0.0f, 0.6f, 1.0f}, MIX[] = {0.0f, 0.5f, 1.0f};
  static const float MS[][2] = {{0.0f, 0.0f}, {23.5f, 31.0f}, {60.0f, 60.0f}};
  const uint32_t n = (uint32_t)(sr * 0.15f);
  float *rl = malloc(n * sizeof(float)), *rr = malloc(n * sizeof(float));
  float *l = malloc(n * sizeof(float)), *r = malloc(n * sizeof(float));
  int all = 1;
  uint32_t combo = 0;
  for (size_t a = 0; a < 3; a++)
    for (size_t b = 0; b < 3; b++)
      for (size_t c = 0; c < 3; c++)
        for (size_t m = 0; m < 3; m++)
          for (int pp = 0; pp < 2; pp++, combo++) {
            struct omx_fx_delay p = {1, omx_fxdelay_ms_to_samples(MS[m][0], sr),
                                     omx_fxdelay_ms_to_samples(MS[m][1], sr), FB[a], MIX[c], TONE[b], pp};
            fresh(s, OMX_FXDELAY_CAP);
            stimulus(rl, rr, n, 0x1234567u + combo);
            render(REF, rl, rr, n, &p, s, sr, 0xabcu + combo);
            for (int w = PLAIN; w <= TAPS_ONE; w++) {
              fresh(s, OMX_FXDELAY_CAP);
              stimulus(l, r, n, 0x1234567u + combo);
              render((enum path)w, l, r, n, &p, s, sr, 0xabcu + combo);
              if (memcmp(l, rl, n * sizeof(float)) != 0 || memcmp(r, rr, n * sizeof(float)) != 0) {
                all = 0;
                fprintf(stderr, "  A: path %d differs at fb %g tone %g mix %g times %g/%g pingpong %d\n", w,
                        (double)FB[a], (double)TONE[b], (double)MIX[c], (double)MS[m][0], (double)MS[m][1], pp);
              }
            }
          }
  check(all, "A: every one-tap path memcmp-equals the frozen one-tap block", sr);
  free(rl); free(rr); free(l); free(r);
}

/** Wet only, no feedback, an impulse on both legs: run `n` frames through `p` + `t`. */
static void impulse(float *l, float *r, uint32_t n, const struct omx_fx_delay *p,
                    const struct omx_fx_delay_taps *t, struct omx_fx_delay_state *s, float sr) {
  memset(l, 0, n * sizeof(float));
  memset(r, 0, n * sizeof(float));
  l[0] = 1.0f; r[0] = 1.0f;
  for (uint32_t o = 0; o < n; o += 1024u) {
    uint32_t q = n - o < 1024u ? n - o : 1024u;
    omx_fx_delay_process_taps(l + o, r + o, q, p, t, s, sr);
  }
}

static void arm_b_tap_positions(float sr, struct omx_fx_delay_state *s) {
  static const float BASES[] = {1.0f, 300.0f, 2000.0f};
  const uint32_t n = (uint32_t)(sr * 2.0f) + 64u;
  float *l = malloc(n * sizeof(float)), *r = malloc(n * sizeof(float));
  int land = 1, quiet = 1;
  for (size_t bi = 0; bi < 3; bi++)
    for (size_t f = 0; f < NFACTORS; f++) {
      float T = BASES[bi];
      uint32_t d0 = omx_fxdelay_ms_to_samples(T, sr);
      struct omx_fx_delay p = {1, d0, d0, 0.0f, 1.0f, 0.0f, 0};
      /* Four taps: tap 0 at the base, tap 1 at the factor under test, taps 2 and 3 at the factors
       * either side of it in the set, each with its own gain pair. */
      struct omx_fx_delay_taps t;
      memset(&t, 0, sizeof t);
      t.ntaps = 4u;
      const size_t fk[4] = {6, f, (f + 1) % NFACTORS, (f + NFACTORS - 1) % NFACTORS};
      const float pan[4] = {0.0f, -0.5f, 0.75f, 0.25f}, gain[4] = {0.5f, 0.75f, 0.375f, 1.0f};
      uint32_t at[4];
      at[0] = d0;
      for (int k = 0; k < 4; k++) omx_fxdelay_tap_legs(gain[k], pan[k], &t.gl[k], &t.gr[k]);
      for (int k = 1; k < 4; k++) {
        t.d[k] = omx_fxdelay_ms_to_samples(omx_fxdelay_tap_ms(T, FACTORS[fk[k]][0], FACTORS[fk[k]][1], NULL), sr);
        at[k] = t.d[k];
      }
      uint32_t span = 0u;
      for (int k = 0; k < 4; k++) span = at[k] > span ? at[k] : span;
      span += 64u;
      fresh(s, OMX_FXDELAY_CAP);
      impulse(l, r, span, &p, &t, s, sr);
      for (uint32_t i = 0; i < span; i++) {
        /* The expected sample: the gains of every tap landing here, summed in tap order. */
        int hit = 0;
        float wl = 0.0f, wr = 0.0f;
        for (int k = 0; k < 4; k++)
          if (at[k] == i) {
            if (!hit) { wl = t.gl[k]; wr = t.gr[k]; } else { wl += t.gl[k]; wr += t.gr[k]; }
            hit = 1;
          }
        if (hit) {
          if (l[i] != wl || r[i] != wr) {
            land = 0;
            fprintf(stderr, "  B: base %g factor %u/%u: sample %u is %g/%g, want %g/%g\n", (double)T,
                    FACTORS[f][0], FACTORS[f][1], i, (double)l[i], (double)r[i], (double)wl, (double)wr);
          }
        } else if (l[i] != 0.0f || r[i] != 0.0f) {
          quiet = 0;
        }
      }
    }
  check(land, "B: each tap lands on its declared sample with its gain pair, bit for bit", sr);
  check(quiet, "B: every sample no tap lands on is zero", sr);
  free(l); free(r);
}

static void arm_c_feedback_from_the_longest_tap(float sr, struct omx_fx_delay_state *s) {
  const uint32_t D0 = omx_fxdelay_ms_to_samples(10.0f, sr), D1 = omx_fxdelay_ms_to_samples(25.0f, sr);
  const uint32_t n = D0 + 9u * D1 + 8u;
  float *l = malloc(n * sizeof(float)), *r = malloc(n * sizeof(float));
  /* The long tap engaged but silent in the wet: what the output shows is tap 0 reading a ring
   * the LONG tap regenerates. */
  struct omx_fx_delay p = {1, D0, D0, 0.5f, 1.0f, 0.0f, 0};
  struct omx_fx_delay_taps t;
  memset(&t, 0, sizeof t);
  t.ntaps = 2u; t.gl[0] = 1.0f; t.gr[0] = 1.0f; t.d[1] = D1;
  fresh(s, OMX_FXDELAY_CAP);
  impulse(l, r, n, &p, &t, s, sr);
  int ok = 1;
  float want = 1.0f;
  for (uint32_t m = 0; m < 8; m++, want *= 0.5f) {
    uint32_t at = D0 + m * D1;
    if (l[at] != want || r[at] != want) {
      ok = 0;
      fprintf(stderr, "  C: regeneration %u at %u is %g, want %g\n", m, at, (double)l[at], (double)want);
    }
  }
  int quiet = 1;
  for (uint32_t i = 0; i < D0 + 7u * D1; i++)
    if ((i < D0 || (i - D0) % D1 != 0u) && (l[i] != 0.0f || r[i] != 0.0f)) quiet = 0;
  check(ok, "C: the regenerations fall at tap 0 + m * the longest tap, at fb^m", sr);
  check(quiet, "C: nothing regenerates at tap 0's own period", sr);
  /* The long tap is tap 0: the second tap is shorter, and the period is tap 0's. */
  t.d[1] = D0 / 2u;
  t.gl[1] = 0.0f; t.gr[1] = 0.0f;
  fresh(s, OMX_FXDELAY_CAP);
  impulse(l, r, n, &p, &t, s, sr);
  check(l[2u * D0] == 0.5f && l[3u * D0] == 0.25f, "C: a shorter second tap leaves the period at tap 0's", sr);
  check(omx_fxdelay_longest_tap(7u, (const uint32_t[]){0u, 7u, 3u}, 3u) == 0u,
        "C: a tie is the first tap, so one tap is tap 0", sr);
  free(l); free(r);
}

static void arm_d_clamp(float sr, struct omx_fx_delay_state *s) {
  int c = 0;
  float ms = omx_fxdelay_tap_ms(2000.0f, 3u, 1u, &c);
  check(ms == (float)OMX_FXDELAY_MAX_MS && c == 1, "D: 2000 ms at factor 3 is the travel's top, clamped", sr);
  ms = omx_fxdelay_tap_ms(500.0f, 3u, 1u, &c);
  check(ms == 1500.0f && c == 0, "D: 500 ms at factor 3 is 1500 ms, not clamped", sr);
  check(omx_fxdelay_tap_ms(NAN, 1u, 2u, &c) == 0.0f && c == 0, "D: a non-finite base is a 0 ms tap", sr);
  check(omx_fxdelay_tap_ms(-5.0f, 1u, 2u, NULL) == 0.0f, "D: a negative base is a 0 ms tap", sr);
  /* A tap the caller did not bound reads cap - 1, every other tap exact. */
  const uint32_t cap = 64u, n = 128u;
  float l[128], r[128];
  struct omx_fx_delay p = {1, 5u, 5u, 0.0f, 1.0f, 0.0f, 0};
  struct omx_fx_delay_taps t;
  memset(&t, 0, sizeof t);
  t.ntaps = 3u;
  t.gl[0] = t.gr[0] = 1.0f; t.gl[1] = t.gr[1] = 0.5f; t.gl[2] = t.gr[2] = 0.25f;
  t.d[1] = 1000u; t.d[2] = 9u;
  fresh(s, cap);
  impulse(l, r, n, &p, &t, s, sr);
  check(l[5] == 1.0f && l[9] == 0.25f && l[cap - 1u] == 0.5f,
        "D: a tap past the ring reads cap - 1, the others exact", sr);
}

static void arm_f_rounding_once(float sr) {
  const float T = 100.01f;
  const uint32_t d0 = omx_fxdelay_ms_to_samples(T, sr);
  int exact = 1, differs = 0;
  for (size_t f = 0; f < NFACTORS; f++) {
    uint32_t num = FACTORS[f][0], den = FACTORS[f][1];
    uint32_t got = omx_fxdelay_ms_to_samples(omx_fxdelay_tap_ms(T, num, den, NULL), sr);
    uint32_t want = (uint32_t)floor((double)T * num / den * (double)sr / 1000.0 + 0.5);
    uint32_t naive = (uint32_t)floor((double)d0 * num / den + 0.5);
    if (got != want) {
      exact = 0;
      fprintf(stderr, "  F: factor %u/%u is %u samples, want %u\n", num, den, got, want);
    }
    if (naive != want) differs = 1;
  }
  check(exact, "F: each tap is round(T * num / den * sr / 1000)", sr);
  if (sr == 44100.0f) check(differs, "F: rounding the base first moves some tap (the arm can see it)", sr);
}

static void arm_g_bypass_blocks_legs(float sr, struct omx_fx_delay_state *s) {
  const uint32_t n = (uint32_t)(sr * 0.5f);
  float *l = malloc(n * sizeof(float)), *r = malloc(n * sizeof(float));
  float *l2 = malloc(n * sizeof(float)), *r2 = malloc(n * sizeof(float));
  const uint32_t d0 = omx_fxdelay_ms_to_samples(120.0f, sr);
  struct omx_fx_delay p = {1, d0, d0, 0.7f, 0.0f, 0.4f, 0};
  struct omx_fx_delay_taps t;
  memset(&t, 0, sizeof t);
  t.ntaps = 4u;
  for (int k = 0; k < 4; k++) omx_fxdelay_tap_legs(1.0f - 0.2f * (float)k, 0.0f, &t.gl[k], &t.gr[k]);
  for (int k = 1; k < 4; k++)
    t.d[k] = omx_fxdelay_ms_to_samples(omx_fxdelay_tap_ms(120.0f, FACTORS[k * 3][0], FACTORS[k * 3][1], NULL), sr);

  /* mix 0: the dry block, bit for bit. */
  stimulus(l, r, n, 7u);
  memcpy(l2, l, n * sizeof(float)); memcpy(r2, r, n * sizeof(float));
  fresh(s, OMX_FXDELAY_CAP);
  omx_fx_delay_process_taps(l, r, n, &p, &t, s, sr);
  check(memcmp(l, l2, n * sizeof(float)) == 0 && memcmp(r, r2, n * sizeof(float)) == 0,
        "G: four taps at mix 0 are the dry block", sr);
  /* disabled: the block untouched. */
  p.enabled = 0; p.mix = 1.0f;
  fresh(s, OMX_FXDELAY_CAP);
  omx_fx_delay_process_taps(l, r, n, &p, &t, s, sr);
  check(memcmp(l, l2, n * sizeof(float)) == 0, "G: a disabled delay with taps is a passthrough", sr);
  p.enabled = 1; p.mix = 0.6f;

  /* one block equals random splits. */
  stimulus(l, r, n, 9u);
  memcpy(l2, l, n * sizeof(float)); memcpy(r2, r, n * sizeof(float));
  fresh(s, OMX_FXDELAY_CAP);
  omx_fx_delay_process_taps(l, r, n, &p, &t, s, sr);
  fresh(s, OMX_FXDELAY_CAP);
  uint32_t x = 99u;
  for (uint32_t o = 0; o < n;) {
    uint32_t q = 1u + lcg_next(&x) % 4096u;
    if (q > n - o) q = n - o;
    omx_fx_delay_process_taps(l2 + o, r2 + o, q, &p, &t, s, sr);
    o += q;
  }
  check(memcmp(l, l2, n * sizeof(float)) == 0 && memcmp(r, r2, n * sizeof(float)) == 0,
        "G: one block memcmp-equals random splits of 1 .. 4096", sr);

  /* identical legs, pans at 0: L == R. */
  stimulus(l, r, n, 11u);
  memcpy(r, l, n * sizeof(float));
  fresh(s, OMX_FXDELAY_CAP);
  omx_fx_delay_process_taps(l, r, n, &p, &t, s, sr);
  check(memcmp(l, r, n * sizeof(float)) == 0, "G: centred taps on identical legs give L == R", sr);

  float gl, gr;
  omx_fxdelay_tap_legs(0.8f, 0.0f, &gl, &gr);
  check(gl == 0.8f && gr == 0.8f, "G: a centred tap is its gain on both legs (no -3 dB)", sr);
  omx_fxdelay_tap_legs(0.8f, 1.0f, &gl, &gr);
  check(gl == 0.0f && gr == 0.8f, "G: a tap panned right is silent on the left", sr);
  omx_fxdelay_tap_legs(1.0f, -0.25f, &gl, &gr);
  check(gl == 1.0f && gr == 0.75f, "G: the far leg attenuates linearly, the near leg never boosts", sr);
  omx_fxdelay_tap_legs(NAN, NAN, &gl, &gr);
  check(gl == 0.0f && gr == 0.0f, "G: a non-finite gain is a silent tap", sr);
  omx_fxdelay_tap_legs(2.0f, 0.0f, &gl, &gr);
  check(gl == 1.0f && gr == 1.0f, "G: a gain above unity is unity", sr);
  free(l); free(r); free(l2); free(r2);
}

int main(void) {
  omx_fx_require_rate_floor();
  struct omx_fx_delay_state s;
  memset(&s, 0, sizeof s);
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    float sr = OMX_DECLARED_RATES[ri];
    arm_a_one_tap_is_the_one_tap_delay(sr, &s);
    arm_b_tap_positions(sr, &s);
    arm_c_feedback_from_the_longest_tap(sr, &s);
    arm_d_clamp(sr, &s);
    arm_f_rounding_once(sr);
    arm_g_bypass_blocks_legs(sr, &s);
  }
  release(&s);
  g_checks++;
  if (omx_contract_log.checks == 0u || omx_contract_log.count != 0u) {
    g_fail++;
    for (uint32_t i = 0; i < omx_contract_log.count && i < OMX_CONTRACT_MAX; i++)
      fprintf(stderr, "  contract: %s/%s (%s)\n", omx_contract_log.rec[i].stage, omx_contract_log.rec[i].token,
              omx_contract_log.rec[i].kind);
    fprintf(stderr, "FAIL: the contracts were reached and none was violated\n");
  }
  printf("fx/delay_taps: %d checks, %d failures (%u rates), %u contracts evaluated\n", g_checks, g_fail,
         (unsigned)OMX_DECLARED_RATE_COUNT, omx_contract_log.checks);
  return g_fail ? 1 : 0;
}
