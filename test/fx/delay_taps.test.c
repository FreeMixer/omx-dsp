// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The FX delay's read taps (FreeMixer/omx-dsp#132, #147): up to OMX_FXDELAY_MAX_TAPS integer reads of
 * the one ring per leg, read by omx_fx_delay_process_taps from the taps snapshot beside the atom,
 * gain x balance-law pan per tap, feedback from the longest engaged tap. Every arm
 * runs at the nine oracle rates (OMX_FX_ORACLE_RATES), contracts compiled in, each rate's ledger drained
 * (a `rate-is-declared` PRE at 32, 64 or 128 kHz is the contract saying so; any other violation
 * counts, except arm D's one deliberate refusal, which it counts itself).
 *
 *   A  one tap memcmps the frozen one-tap kernel (test/fx/fixtures/delay_one_tap_reference.h):
 *      omx_fx_delay_process, the taps call with no taps, at ntaps 0 and at ntaps 1 at unity and
 *      centre; outputs, rings and damping state, over feedback {0, 0.5, 0.99} x tone {0, 0.3, 1} x
 *      pingpong, -0.0f through a 0 ms leg, random block splits;
 *   B  every factor x base tap lands on round(min(T*phi, top)*sr/1000), formed once in double,
 *      carrying gain x balance per leg, and nothing anywhere else;
 *   C  four taps at feedback 0.5 regenerate every D_longest: impulses at m*D_kf + D_k of
 *      fb^m * g_k, within 1e-6 relative;
 *   D  a tap asked past the ring is refused (PRE taps-inside-the-ring) and read at cap-1; a tap asked
 *      past the time travel is clamped IN MILLISECONDS by omx_fxdelay_tap_ms, says so, and lands on
 *      the travel's top at every rate, not where the ring would have held it;
 *   E  the output peak stays under ((1-mix) + mix*sum(g)/(1-fb)) * max|x|;
 *   F  a tap is rounded once from its own time, never phi times a rounded base;
 *   G  four taps are block-size invariant, centred taps on identical legs give L == R, and
 *      omx_fxdelay_tap_legs is gain x the balance law in closed form (unity at centre).
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
static void check(int cond, const char *arm, const char *what) {
  g_checks++;
  if (!cond) { g_fail++; printf("FAIL [%s] %s\n", arm, what); }
}

/* Drained after every block: at 32, 64 and 128 kHz each call records its `rate-is-declared` PRE, and
 * thousands of blocks would overflow the ledger, whose lost records count as unexplained. Arm D holds
 * the ledger (g_hold) to read its one refusal itself. */
static uint32_t g_unexplained = 0;
static int g_hold = 0;

static void drain(float sr) {
  const uint32_t unexplained = g_unexplained + omx_fx_drain_ledger(sr);
  g_unexplained = 0;
  char what[96];
  snprintf(what, sizeof what, "%.0f Hz: no contract violation the rate does not explain", (double)sr);
  check(unexplained == 0u, "ledger", what);
}

/** The tap-delay factor set, num/den, in its declared order (2026-09-26 tap-delay variants §3:
 * DELAY_TAP_FACTORS), as omx-contract renders it (OMX_DELAY_TAP_FACTOR_NUMS / _DENS). The kernel never
 * reads it, only this oracle's choice of taps. */
static const int TAP_NUM[] = OMX_DELAY_TAP_FACTOR_NUMS_INIT;
static const int TAP_DEN[] = OMX_DELAY_TAP_FACTOR_DENS_INIT;
_Static_assert(sizeof TAP_NUM / sizeof TAP_NUM[0] == OMX_DELAY_TAP_FACTOR_NUMS_COUNT, "the factor numerators as declared");
_Static_assert(sizeof TAP_DEN / sizeof TAP_DEN[0] == OMX_DELAY_TAP_FACTOR_DENS_COUNT, "the factor denominators as declared");
#define NFACTORS ((int)(sizeof TAP_NUM / sizeof TAP_NUM[0]))

static uint32_t g_rng = 0x2545F491u;
static float noise(void) {
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
  return (float)((double)g_rng / 4294967295.0 * 2.0 - 1.0);
}

static void ring_alloc(struct omx_fx_delay_state *s) {
  memset(s, 0, sizeof *s);
  s->cap = OMX_FXDELAY_CAP;
  s->ring_l = calloc(OMX_FXDELAY_CAP, sizeof(float));
  s->ring_r = calloc(OMX_FXDELAY_CAP, sizeof(float));
}
static void ring_free(struct omx_fx_delay_state *s) { free(s->ring_l); free(s->ring_r); }

/** The closed form of a tap's time (tap-delay §2): T*num/den in ms, clamped IN MILLISECONDS to the
 * declared time travel's top, so the longest tap does not depend on the clock. The oracle's own
 * statement of the mathematics; the kernel's word for it is omx_fxdelay_tap_ms. */
static double tap_ms_closed_form(double base_ms, int num, int den) {
  const double ms = base_ms * num / den;
  return ms > (double)OMX_FXDELAY_MAX_MS ? (double)OMX_FXDELAY_MAX_MS : ms;
}

/** Tap k as a resolver sets it, through the kernel's words: omx_fxdelay_tap_ms, ONE rounding by
 * omx_fxdelay_ms_to_samples, gain x balance-law pan by omx_fxdelay_tap_legs. */
static void set_tap(struct omx_fx_delay_taps *t, uint32_t k, double base_ms, int num, int den, float gain, float pan,
                    float sr) {
  omx_fxdelay_tap_legs(gain, pan, &t->gl[k], &t->gr[k]);
  if (k > 0) t->d[k] = omx_fxdelay_ms_to_samples(omx_fxdelay_tap_ms((float)base_ms, (uint32_t)num, (uint32_t)den, NULL), sr);
}

/** The closed form of tap `T*num/den` at `sr`: round once, in double. */
static uint32_t tap_closed_form(double base_ms, int num, int den, float sr) {
  return (uint32_t)floor(tap_ms_closed_form(base_ms, num, den) * 0.001 * (double)sr + 0.5);
}

/** How run_blocks calls the kernel. */
enum call { REFERENCE, PLAIN, TAPS };

/** Run `n` frames in blocks of random length 1..maxb (maxb 0: one block): through the frozen one-tap
 * reference, omx_fx_delay_process, or omx_fx_delay_process_taps with `t` (which may be NULL). */
static void run_blocks(float *l, float *r, int n, const struct omx_fx_delay *p, const struct omx_fx_delay_taps *t,
                       struct omx_fx_delay_state *s, float sr, int maxb, enum call call) {
  for (int o = 0; o < n;) {
    int q = maxb == 0 ? n - o : 1 + (int)(g_rng % (uint32_t)maxb);
    g_rng = g_rng * 1664525u + 1013904223u;
    if (q > n - o) q = n - o;
    if (call == REFERENCE) omx_fx_delay_process_one_tap_reference(l + o, r + o, (uint32_t)q, p, s, sr);
    else if (call == PLAIN) omx_fx_delay_process(l + o, r + o, (uint32_t)q, p, s, sr);
    else omx_fx_delay_process_taps(l + o, r + o, (uint32_t)q, p, t, s, sr);
    if (!g_hold) g_unexplained += omx_fx_drain_ledger(sr);
    o += q;
  }
}

static void arm_a_one_tap_is_the_plain_delay(float sr) {
  static const float FB[] = { 0.0f, 0.5f, 0.99f };
  static const float TONE[] = { 0.0f, 0.3f, 1.0f };
  const int n = (int)(sr * 1.5f);
  float *in = malloc((size_t)n * sizeof(float) * 2);
  /* -0.0f samples through a 0 ms leg: a wet sum that starts at +0 turns them into +0 */
  for (int i = 0; i < 2 * n; i++) in[i] = (i % 4801 == 0) ? 1.0f : (i % 97 == 0) ? -0.0f : 0.5f * noise();
  float *la = malloc((size_t)n * sizeof(float)), *ra = malloc((size_t)n * sizeof(float));
  float *lb = malloc((size_t)n * sizeof(float)), *rb = malloc((size_t)n * sizeof(float));
  int all = 1, runs = 0;
  for (int f = 0; f < 3; f++)
    for (int t = 0; t < 3; t++)
      for (int pp = 0; pp < 2; pp++)
        for (int form = 0; form < 4; form++) {
          /* form 0: omx_fx_delay_process; 1: the taps call with no taps; 2: ntaps 0; 3: ntaps 1 at
           * unity and centre */
          struct omx_fx_delay p;
          struct omx_fx_delay_taps tp;
          memset(&p, 0, sizeof p);
          memset(&tp, 0, sizeof tp);
          p.enabled = 1; p.feedback = FB[f]; p.tone = TONE[t]; p.pingpong = pp; p.mix = 0.3f;
          p.d_l = omx_fxdelay_ms_to_samples(300.0f, sr);
          p.d_r = omx_fxdelay_ms_to_samples(t == 1 ? 212.5f : 0.0f, sr);
          if (form == 3) { tp.ntaps = 1; set_tap(&tp, 0, 300.0, 1, 1, 1.0f, 0.0f, sr); }
          memcpy(la, in, (size_t)n * sizeof(float)); memcpy(ra, in + n, (size_t)n * sizeof(float));
          memcpy(lb, la, (size_t)n * sizeof(float)); memcpy(rb, ra, (size_t)n * sizeof(float));
          struct omx_fx_delay_state sa, sb;
          ring_alloc(&sa); ring_alloc(&sb);
          const uint32_t seed = g_rng;
          run_blocks(la, ra, n, &p, NULL, &sa, sr, 512, REFERENCE);
          g_rng = seed;
          run_blocks(lb, rb, n, &p, form == 1 ? NULL : &tp, &sb, sr, 512, form == 0 ? PLAIN : TAPS);
          if (memcmp(la, lb, (size_t)n * sizeof(float)) || memcmp(ra, rb, (size_t)n * sizeof(float)) ||
              memcmp(sa.ring_l, sb.ring_l, OMX_FXDELAY_CAP * sizeof(float)) ||
              memcmp(sa.ring_r, sb.ring_r, OMX_FXDELAY_CAP * sizeof(float)) ||
              memcmp(&sa.damp_l, &sb.damp_l, sizeof(float)) || memcmp(&sa.damp_r, &sb.damp_r, sizeof(float)) ||
              sa.wpos != sb.wpos)
            all = 0;
          runs++;
          ring_free(&sa); ring_free(&sb);
        }
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: one tap (the plain call, no taps, ntaps 0, ntaps 1 at unity/centre) "
           "memcmps the frozen one-tap kernel over %d runs: outputs, rings, damping, cursor", (double)sr, runs);
  check(all && runs == 72, "A", what);
  free(in); free(la); free(ra); free(lb); free(rb);
}

static void arm_b_every_tap_lands_on_its_closed_form(float sr) {
  static const double BASE[] = { 1.0, 300.0, 100.01, 2000.0 };
  int wrong = 0, checked = 0;
  for (int bi = 0; bi < 4; bi++)
    for (int fi = 0; fi < NFACTORS; fi++) {
      if (TAP_NUM[fi] == TAP_DEN[fi]) continue; /* factor 1 is tap 0 itself */
      struct omx_fx_delay p;
      struct omx_fx_delay_taps tp;
      memset(&p, 0, sizeof p);
      memset(&tp, 0, sizeof tp);
      p.enabled = 1; p.mix = 1.0f; tp.ntaps = 2;
      p.d_l = p.d_r = omx_fxdelay_ms_to_samples((float)BASE[bi], sr);
      set_tap(&tp, 0, BASE[bi], 1, 1, 0.8f, -0.3f, sr);
      set_tap(&tp, 1, BASE[bi], TAP_NUM[fi], TAP_DEN[fi], 0.6f, 0.3f, sr);
      const uint32_t d0 = tap_closed_form(BASE[bi], 1, 1, sr);
      const uint32_t d1 = tap_closed_form(BASE[bi], TAP_NUM[fi], TAP_DEN[fi], sr);
      if (d0 == d1) continue;
      /* the per-leg gains are gain x the balance law, in closed form: pan -0.3 keeps L, takes R to 0.7 */
      const float g0l = 0.8f, g0r = 0.8f * (1.0f - 0.3f), g1l = 0.6f * (1.0f - 0.3f), g1r = 0.6f;
      const int n = (int)(d0 > d1 ? d0 : d1) + 16;
      float *l = calloc((size_t)n, sizeof(float)), *r = calloc((size_t)n, sizeof(float));
      l[0] = r[0] = 1.0f;
      struct omx_fx_delay_state s;
      ring_alloc(&s);
      run_blocks(l, r, n, &p, &tp, &s, sr, 0, TAPS);
      int ok = l[d0] == g0l && r[d0] == g0r && l[d1] == g1l && r[d1] == g1r;
      for (int i = 0; i < n; i++)
        if (i != (int)d0 && i != (int)d1 && (l[i] != 0.0f || r[i] != 0.0f)) ok = 0;
      if (!ok) {
        wrong++;
        printf("  B %.0f Hz base %g factor %d/%d: want taps at %u, %u\n", (double)sr, BASE[bi], TAP_NUM[fi],
               TAP_DEN[fi], d0, d1);
      }
      checked++;
      ring_free(&s); free(l); free(r);
    }
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: %d factor x base taps land on round(T*phi*sr/1000) with gain x balance-law "
           "per leg, nowhere else", (double)sr, checked);
  check(wrong == 0 && checked > 30, "B", what);
}

static void arm_c_feedback_regenerates_from_the_longest_tap(float sr) {
  static const int NUM[4] = { 1, 1, 3, 3 }, DEN[4] = { 1, 2, 4, 2 };
  static const float G[4] = { 1.0f, 0.7f, 0.5f, 0.35f };
  struct omx_fx_delay p;
  struct omx_fx_delay_taps tp;
  memset(&p, 0, sizeof p);
  memset(&tp, 0, sizeof tp);
  p.enabled = 1; p.mix = 1.0f; p.feedback = 0.5f; p.tone = 0.0f; tp.ntaps = 4;
  p.d_l = p.d_r = omx_fxdelay_ms_to_samples(300.0f, sr);
  uint32_t d[4];
  for (int k = 0; k < 4; k++) {
    set_tap(&tp, (uint32_t)k, 300.0, NUM[k], DEN[k], G[k], 0.0f, sr);
    d[k] = tap_closed_form(300.0, NUM[k], DEN[k], sr);
  }
  const uint32_t dkf = d[3];
  const int n = (int)(4 * dkf) + 16;
  float *l = calloc((size_t)n, sizeof(float)), *r = calloc((size_t)n, sizeof(float));
  double *want = calloc((size_t)n, sizeof(double));
  l[0] = r[0] = 1.0f;
  for (int m = 0; m < 4; m++)
    for (int k = 0; k < 4; k++) {
      const uint32_t at = (uint32_t)m * dkf + d[k];
      if ((int)at < n) want[at] += pow(0.5, m) * (double)G[k];
    }
  struct omx_fx_delay_state s;
  ring_alloc(&s);
  run_blocks(l, r, n, &p, &tp, &s, sr, 0, TAPS);
  int ok = 1;
  for (int i = 0; i < n; i++) {
    const double err = fabs((double)l[i] - want[i]);
    if (err > 1e-6 * (want[i] > 1e-3 ? want[i] : 1e-3)) {
      if (ok) printf("  C %.0f Hz: sample %d got %.9g want %.9g\n", (double)sr, i, (double)l[i], want[i]);
      ok = 0;
    }
  }
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: four taps at fb 0.5 regenerate every %u samples (the longest tap), "
           "fb^m x gain_k within 1e-6 relative, L == R", (double)sr, dkf);
  check(ok && memcmp(l, r, (size_t)n * sizeof(float)) == 0, "C", what);
  ring_free(&s); free(l); free(r); free(want);
}

static void arm_d_a_tap_past_the_ring_is_refused(float sr) {
  struct omx_fx_delay p;
  struct omx_fx_delay_taps tp;
  memset(&p, 0, sizeof p);
  memset(&tp, 0, sizeof tp);
  p.enabled = 1; p.mix = 1.0f; tp.ntaps = 2;
  p.d_l = p.d_r = omx_fxdelay_ms_to_samples(1000.0f, sr);
  set_tap(&tp, 0, 1000.0, 1, 1, 1.0f, 0.0f, sr);
  set_tap(&tp, 1, 1000.0, 1, 1, 0.5f, 0.0f, sr);
  tp.d[1] = (uint32_t)OMX_FXDELAY_CAP * 4u;
  const uint32_t d0 = tap_closed_form(1000.0, 1, 1, sr);
  const int n = OMX_FXDELAY_CAP + 8;
  float *l = calloc((size_t)n, sizeof(float)), *r = calloc((size_t)n, sizeof(float));
  l[0] = r[0] = 1.0f;
  struct omx_fx_delay_state s;
  ring_alloc(&s);
  /* everything before this arm is drained; the one block below must record exactly one refusal */
  g_hold = 1;
  run_blocks(l, r, n, &p, &tp, &s, sr, 0, TAPS);
  g_hold = 0;
  uint32_t refused = 0, other = 0;
  for (uint32_t i = 0; i < omx_contract_log.count && i < OMX_CONTRACT_MAX; i++) {
    const struct omx_contract_record *rec = &omx_contract_log.rec[i];
    if (strcmp(rec->stage, "fx-delay") == 0 && strcmp(rec->token, "taps-inside-the-ring") == 0) refused++;
    else if (!(strcmp(rec->token, "rate-is-declared") == 0 && !omx_rate_is_declared(sr))) other++;
  }
  omx_contract_reset();
  int ok = l[d0] == 1.0f && l[OMX_FXDELAY_CAP - 1] == 0.5f;
  for (int i = 0; i < n; i++)
    if (i != (int)d0 && i != OMX_FXDELAY_CAP - 1 && l[i] != 0.0f) ok = 0;
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: a tap asked at 4 x cap reads cap-1 = %d, tap 0 exact at %u",
           (double)sr, OMX_FXDELAY_CAP - 1, d0);
  check(ok, "D", what);
  snprintf(what, sizeof what, "%.0f Hz: the tap past the ring is refused once by the kernel's precondition "
           "(taps-inside-the-ring: %u), nothing else recorded (%u)", (double)sr, refused, other);
  check(refused == 1u && other == 0u, "D", what);
  ring_free(&s); free(l); free(r);
}

static void arm_e_the_level_bound_holds(float sr) {
  static const float FB[] = { 0.0f, 0.5f, 0.99f };
  static const float G[4] = { 1.0f, 0.7f, 0.5f, 0.35f };
  static const int NUM[4] = { 1, 1, 3, 3 }, DEN[4] = { 1, 2, 4, 2 };
  const int n = (int)sr;
  int over = 0, runs = 0;
  float *l = malloc((size_t)n * sizeof(float)), *r = malloc((size_t)n * sizeof(float));
  for (uint32_t nt = 1; nt <= 4; nt++)
    for (int f = 0; f < 3; f++)
      for (int mi = 0; mi < 2; mi++) {
        struct omx_fx_delay p;
        struct omx_fx_delay_taps tp;
        memset(&p, 0, sizeof p);
        memset(&tp, 0, sizeof tp);
        p.enabled = 1; tp.ntaps = nt; p.feedback = FB[f]; p.mix = mi ? 1.0f : 0.3f;
        p.d_l = p.d_r = omx_fxdelay_ms_to_samples(30.0f, sr);
        double gsum = 0.0;
        for (uint32_t k = 0; k < nt; k++) { set_tap(&tp, k, 30.0, NUM[k], DEN[k], G[k], 0.0f, sr); gsum += G[k]; }
        double xmax = 0.0;
        for (int i = 0; i < n; i++) { l[i] = r[i] = noise(); if (fabs(l[i]) > xmax) xmax = fabs(l[i]); }
        const double bound = ((1.0 - p.mix) + p.mix * gsum / (1.0 - p.feedback)) * xmax;
        struct omx_fx_delay_state s;
        ring_alloc(&s);
        run_blocks(l, r, n, &p, &tp, &s, sr, 256, TAPS);
        for (int i = 0; i < n; i++) if (fabs(l[i]) > bound * (1.0 + 1e-6)) { over++; break; }
        runs++;
        ring_free(&s);
      }
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: %d tap/fb/mix settings stay inside the peak bound "
           "((1-mix) + mix*sum(g)/(1-fb)) * max|x|", (double)sr, runs);
  check(over == 0 && runs == 24, "E", what);
  free(l); free(r);
}

static void arm_g_blocks_and_legs(float sr) {
  static const int NUM[4] = { 1, 1, 2, 4 }, DEN[4] = { 1, 3, 3, 3 };
  const int n = (int)(sr * 0.5f);
  struct omx_fx_delay p;
  struct omx_fx_delay_taps tp;
  memset(&p, 0, sizeof p);
  memset(&tp, 0, sizeof tp);
  p.enabled = 1; tp.ntaps = 4; p.feedback = 0.4f; p.tone = 0.3f; p.mix = 0.5f;
  p.d_l = p.d_r = omx_fxdelay_ms_to_samples(40.0f, sr);
  for (uint32_t k = 0; k < 4; k++) set_tap(&tp, k, 40.0, NUM[k], DEN[k], 0.6f, 0.0f, sr);
  float *la = malloc((size_t)n * sizeof(float)), *ra = malloc((size_t)n * sizeof(float));
  float *lb = malloc((size_t)n * sizeof(float)), *rb = malloc((size_t)n * sizeof(float));
  for (int i = 0; i < n; i++) la[i] = ra[i] = lb[i] = rb[i] = noise();
  struct omx_fx_delay_state sa, sb;
  ring_alloc(&sa); ring_alloc(&sb);
  run_blocks(la, ra, n, &p, &tp, &sa, sr, 0, TAPS);
  run_blocks(lb, rb, n, &p, &tp, &sb, sr, 4096, TAPS);
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: four taps, one block memcmps random 1..4096 splits", (double)sr);
  check(memcmp(la, lb, (size_t)n * sizeof(float)) == 0, "G", what);
  snprintf(what, sizeof what, "%.0f Hz: centred taps on identical legs give L == R", (double)sr);
  check(memcmp(la, ra, (size_t)n * sizeof(float)) == 0, "G", what);
  ring_free(&sa); ring_free(&sb); free(la); free(ra); free(lb); free(rb);
}

/* Arm D, the millisecond ceiling: omx_fxdelay_tap_ms against its closed form, its flag and its one
 * refusal, then through the kernel, where a tap asked past the travel lands on the travel's top at
 * every rate. The ring holds 2 s at 192 kHz and more below it, so without the millisecond clamp a
 * 2.25 s tap would land at 2.25 s at every rate below 192 kHz. */
static void arm_d_a_tap_past_the_travel_is_clamped_in_ms(float sr) {
  static const struct { float base; uint32_t num, den; double want; int clamped; } CASE[] = {
    { 2000.0f, 3u, 1u, 2000.0, 1 },  /* the spec's case: a 2 s base at factor 3 */
    { 1500.0f, 3u, 2u, 2000.0, 1 },
    { 2000.0f, 1u, 1u, 2000.0, 0 },  /* the top itself is inside the travel */
    { 300.0f, 3u, 2u, 450.0, 0 },
    { 300.0f, 1u, 3u, 100.0, 0 },
    { 100.01f, 3u, 8u, 100.01f * 3.0 / 8.0, 0 },
    { -5.0f, 1u, 2u, 0.0, 0 },
    { 0.0f, 3u, 1u, 0.0, 0 },
  };
  int wrong = 0;
  for (size_t i = 0; i < sizeof CASE / sizeof CASE[0]; i++) {
    int clamped = -1;
    const float ms = omx_fxdelay_tap_ms(CASE[i].base, CASE[i].num, CASE[i].den, &clamped);
    if (ms != (float)CASE[i].want || clamped != CASE[i].clamped) {
      wrong++;
      printf("  D %.0f Hz: tap_ms(%g, %u/%u) = %.9g clamped %d, want %.9g clamped %d\n", (double)sr,
             (double)CASE[i].base, CASE[i].num, CASE[i].den, (double)ms, clamped, CASE[i].want, CASE[i].clamped);
    }
  }
  /* a non-finite base is no time; a NULL flag is allowed */
  if (omx_fxdelay_tap_ms(NAN, 1u, 2u, NULL) != 0.0f || omx_fxdelay_tap_ms(INFINITY, 1u, 2u, NULL) != 0.0f) wrong++;
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: omx_fxdelay_tap_ms is min(T*num/den, %d ms) with its clamped flag, "
           "a non-finite or negative base 0", (double)sr, OMX_FXDELAY_MAX_MS);
  check(wrong == 0, "D", what);

  /* the one refusal: a zero denominator */
  g_hold = 1;
  const float zero = omx_fxdelay_tap_ms(300.0f, 1u, 0u, NULL);
  g_hold = 0;
  uint32_t refused = 0, other = 0;
  for (uint32_t i = 0; i < omx_contract_log.count && i < OMX_CONTRACT_MAX; i++) {
    const struct omx_contract_record *rec = &omx_contract_log.rec[i];
    if (strcmp(rec->stage, "fx-delay/tap-ms") == 0 && strcmp(rec->token, "factor-denominator-is-not-zero") == 0)
      refused++;
    else other++;
  }
  omx_contract_reset();
  snprintf(what, sizeof what, "%.0f Hz: a zero denominator is refused once (%u) and is a 0 ms tap, nothing "
           "else recorded (%u)", (double)sr, refused, other);
  check(refused == 1u && other == 0u && zero == 0.0f, "D", what);

  /* through the kernel: 1500 ms x 3/2 asks 2250 ms and lands on 2000 ms, round(2 * sr) */
  struct omx_fx_delay p;
  struct omx_fx_delay_taps tp;
  memset(&p, 0, sizeof p);
  memset(&tp, 0, sizeof tp);
  p.enabled = 1; p.mix = 1.0f; tp.ntaps = 2;
  p.d_l = p.d_r = omx_fxdelay_ms_to_samples(1500.0f, sr);
  set_tap(&tp, 0, 1500.0, 1, 1, 1.0f, 0.0f, sr);
  set_tap(&tp, 1, 1500.0, 3, 2, 0.5f, 0.0f, sr);
  const uint32_t d0 = tap_closed_form(1500.0, 1, 1, sr);
  const uint32_t d1 = (uint32_t)floor((double)OMX_FXDELAY_MAX_MS * 0.001 * (double)sr + 0.5);
  const int n = (int)d1 + 16;
  float *l = calloc((size_t)n, sizeof(float)), *r = calloc((size_t)n, sizeof(float));
  l[0] = r[0] = 1.0f;
  struct omx_fx_delay_state s;
  ring_alloc(&s);
  run_blocks(l, r, n, &p, &tp, &s, sr, 0, TAPS);
  int ok = l[d0] == 1.0f && l[d1] == 0.5f;
  for (int i = 0; i < n; i++)
    if (i != (int)d0 && i != (int)d1 && l[i] != 0.0f) ok = 0;
  snprintf(what, sizeof what, "%.0f Hz: a 2250 ms tap lands on the travel's top, sample %u, and nowhere else",
           (double)sr, d1);
  check(ok, "D", what);
  ring_free(&s); free(l); free(r);
}

/* Arm F, one rounding: every factor at bases that are no whole number of samples, resolved through
 * omx_fxdelay_tap_ms and omx_fxdelay_ms_to_samples, equals round(T*phi*sr/1000) in double. The arm
 * proves it can tell: at every rate some taps differ from round(phi * round(T*sr/1000)), the tap a
 * resolver that rounded the base first would set. */
static void arm_f_a_tap_is_rounded_once(float sr) {
  static const double BASE[] = { 100.01, 33.33, 7.77, 251.3 };
  int wrong = 0, apart = 0, checked = 0;
  for (int bi = 0; bi < 4; bi++)
    for (int fi = 0; fi < NFACTORS; fi++) {
      const uint32_t got = omx_fxdelay_ms_to_samples(
          omx_fxdelay_tap_ms((float)BASE[bi], (uint32_t)TAP_NUM[fi], (uint32_t)TAP_DEN[fi], NULL), sr);
      const uint32_t want = tap_closed_form(BASE[bi], TAP_NUM[fi], TAP_DEN[fi], sr);
      const double base_samples = floor(BASE[bi] * 0.001 * (double)sr + 0.5);
      const uint32_t twice = (uint32_t)floor(base_samples * TAP_NUM[fi] / TAP_DEN[fi] + 0.5);
      if (got != want) {
        wrong++;
        printf("  F %.0f Hz base %g factor %d/%d: got %u want %u\n", (double)sr, BASE[bi], TAP_NUM[fi],
               TAP_DEN[fi], got, want);
      }
      if (twice != want) apart++;
      checked++;
    }
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: %d taps are rounded once from T*phi (%d of them differ from phi x a "
           "rounded base)", (double)sr, checked, apart);
  check(wrong == 0 && apart > 0, "F", what);
}

/* Arm G, the legs: omx_fxdelay_tap_legs is the gain (clamped, a non-finite gain 0) times the balance
 * law in closed form, bL = 1 - max(pan, 0), bR = 1 + min(pan, 0): unity on both legs at centre, the
 * far leg attenuated, never a boost; a non-finite pan is centre. */
static void arm_g_tap_legs_are_gain_times_the_balance_law(float sr) {
  static const float GAIN[] = { 0.0f, 0.35f, 0.7f, 1.0f };
  static const float PAN[] = { -1.0f, -0.5f, -0.3f, 0.0f, 0.25f, 0.5f, 1.0f };
  int wrong = 0;
  for (size_t gi = 0; gi < sizeof GAIN / sizeof GAIN[0]; gi++)
    for (size_t pi = 0; pi < sizeof PAN / sizeof PAN[0]; pi++) {
      float gl = -1.0f, gr = -1.0f;
      omx_fxdelay_tap_legs(GAIN[gi], PAN[pi], &gl, &gr);
      const float bl = PAN[pi] > 0.0f ? 1.0f - PAN[pi] : 1.0f;
      const float br = PAN[pi] < 0.0f ? 1.0f + PAN[pi] : 1.0f;
      if (gl != GAIN[gi] * bl || gr != GAIN[gi] * br) {
        wrong++;
        printf("  G tap_legs(%g, %g) = (%.9g, %.9g), want (%.9g, %.9g)\n", (double)GAIN[gi], (double)PAN[pi],
               (double)gl, (double)gr, (double)(GAIN[gi] * bl), (double)(GAIN[gi] * br));
      }
    }
  float gl, gr;
  omx_fxdelay_tap_legs(NAN, 0.0f, &gl, &gr);
  if (gl != 0.0f || gr != 0.0f) wrong++;
  omx_fxdelay_tap_legs(1.5f, 0.0f, &gl, &gr);
  if (gl != 1.0f || gr != 1.0f) wrong++;
  omx_fxdelay_tap_legs(0.6f, NAN, &gl, &gr);
  if (gl != 0.6f || gr != 0.6f) wrong++;
  omx_fxdelay_tap_legs(0.6f, -3.0f, &gl, &gr);
  if (gl != 0.6f || gr != 0.0f) wrong++;
  char what[200];
  snprintf(what, sizeof what, "%.0f Hz: omx_fxdelay_tap_legs is gain x the balance law (unity at centre), a "
           "non-finite gain silent, a non-finite pan centre", (double)sr);
  check(wrong == 0, "G", what);
}

int main(void) {
  omx_fx_require_rate_floor();
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    const float sr = OMX_FX_ORACLE_RATES[ri];
    arm_a_one_tap_is_the_plain_delay(sr);
    arm_b_every_tap_lands_on_its_closed_form(sr);
    arm_c_feedback_regenerates_from_the_longest_tap(sr);
    drain(sr);
    arm_d_a_tap_past_the_ring_is_refused(sr);
    drain(sr);
    arm_d_a_tap_past_the_travel_is_clamped_in_ms(sr);
    arm_e_the_level_bound_holds(sr);
    arm_f_a_tap_is_rounded_once(sr);
    arm_g_blocks_and_legs(sr);
    arm_g_tap_legs_are_gain_times_the_balance_law(sr);
    drain(sr);
  }
  check(omx_contract_log.checks > 0u, "ledger", "the contracts were REACHED");
  printf("fx/delay_taps: %d checks, %d failures (%d rates), %u contracts evaluated\n", g_checks, g_fail,
         (int)OMX_FX_ORACLE_RATE_COUNT, omx_contract_log.checks);
  return g_fail == 0 ? 0 : 1;
}
