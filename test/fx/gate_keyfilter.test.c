// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The gate's key filter (omx-contract 2.7.0: hpHz, lpHz), through omx_gate.h's omx_gate_key_*
// and the instance face fx/omx_gate_instance.h. At the nine oracle rates, contracts on, an empty
// ledger after each arm:
//   A  neutral: both edges at 0, and edges that are NaN, negative or infinite (out), are the gate
//      before the key filter: omx_gate_resolve over controls with no edges + omx_gate_run, bit for
//      bit, keyed, SELF and with no key;
//   B  closed form: a steady tone through the filter settles to the closed-form gain. The
//      high-pass is the cookbook's bilinear Butterworth, |H|^2 = W^4 / (1 + W^4) with
//      W = tan(w/2) / tan(w0/2), at two octaves either side of the corner; the low-pass is the
//      matched-Z Butterworth, -3.0103 dB at its corner and unity at DC wherever the corner sits,
//      and the analog Butterworth 1 / (1 + (f/f0)^4) up to an octave above a corner at most a
//      twentieth of the rate; both edges in is the product of the two; a corner above Nyquist is
//      held at 0.999 Nyquist, finite and -3.0103 dB there (the high-pass then cuts the band);
//   C  live: with the key band 500 Hz .. 1.5 kHz, a key tone inside the band opens the gate and one
//      below or above it does not, keyed and SELF; with the filter out the same out-of-band tones
//      open it;
//   D  history: an edge that comes in starts from silence, a corner that moves keeps its history,
//      a disabled gate leaves it as it was, and init clears it;
//   E  no allocation: init, resolve and run with the key filter in, allocators wrapped at link time.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_gate_instance.h>

#include "fx_rates.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
}

static void expect_clean(float sr) {
  const uint32_t unexplained = omx_fx_drain_ledger(sr);
  ok(unexplained == 0u, "no contract violation in this arm", (double)unexplained, 0.0);
}

static uint32_t g_seed;
static float rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

#define N 3000u /* not a multiple of OMX_GATE_CHUNK */
#define KNEE_HARD -40.0f

static OmxGateInstance g_inst;
static struct omx_gate g_ref;

static void fill(float *l, float *r, float *k) {
  g_seed = 103u;
  for (uint32_t i = 0; i < N; i++) {
    const float amp = ((i / 700u) % 2u == 0u) ? 0.5f : 0.002f;
    l[i] = amp * rnd();
    r[i] = amp * rnd();
    k[i] = ((i / 500u) % 2u == 0u) ? 0.3f * rnd() : 0.0f;
  }
}

/* ---- the measurement: a steady tone's amplitude by least squares on sin and cos --------------- */

/** The filter's gain, dB, at `f` Hz: a unit tone through `k` for `settle` frames, then the
 * amplitude of the next `win` frames fitted on sin and cos of the tone's own phase. */
static double tone_gain_db(struct omx_gate_key *k, double f, float sr, uint32_t settle, uint32_t win) {
  static float buf[OMX_GATE_CHUNK];
  double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;
  const double w = 2.0 * M_PI * f / (double)sr;
  for (uint32_t done = 0; done < settle + win;) {
    const uint32_t m = settle + win - done < OMX_GATE_CHUNK ? settle + win - done : OMX_GATE_CHUNK;
    for (uint32_t i = 0; i < m; i++) buf[i] = (float)sin(w * (double)(done + i));
    omx_gate_key_run(k, buf, m);
    for (uint32_t i = 0; i < m; i++) {
      if (done + i < settle) continue;
      const double s = sin(w * (double)(done + i)), c = cos(w * (double)(done + i));
      ss += s * s; cc += c * c; sc += s * c; ys += buf[i] * s; yc += buf[i] * c;
    }
    done += m;
  }
  const double det = ss * cc - sc * sc;
  const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
  return 10.0 * log10(a * a + b * b);
}

/** The cookbook high-pass at Q 1/sqrt 2, in closed form: the analog Butterworth through the
 * bilinear map warped at the corner. */
static double hp_closed_db(double f, double f0, double sr) {
  const double w = tan(M_PI * f / sr) / tan(M_PI * f0 / sr), w4 = w * w * w * w;
  return 10.0 * log10(w4 / (1.0 + w4));
}

/** The analog Butterworth low-pass, second order. */
static double lp_analog_db(double f, double f0) {
  const double x = f / f0;
  return -10.0 * log10(1.0 + x * x * x * x);
}

static void key_at(struct omx_gate_key *k, float sr, float hp, float lp) {
  memset(k, 0, sizeof *k);
  omx_gate_key_design(k, sr, hp, lp);
}

/* ---- the arms ---------------------------------------------------------------------------------- */

static void arm_neutral(float sr) {
  g_arm = "A neutral";
  static float l[N], r[N], k[N], ol[N], or_[N], el[N], er[N];
  uint32_t unexplained = 0;
  const float out_edges[][2] = {{0.0f, 0.0f}, {NAN, NAN}, {-5.0f, -1.0f}, {INFINITY, -INFINITY}, {0.0f, NAN}};
  for (unsigned e = 0; e < sizeof out_edges / sizeof out_edges[0]; e++)
    for (int ks = 0; ks < 3; ks++) {
      const float *key = ks == 2 ? NULL : k;
      const int ke = ks == 0;
      fill(l, r, k);
      omx_gate_instance_init(&g_inst, sr);
      omx_gate_instance_resolve(&g_inst, 0, ke, -30.0f, -90.0f, KNEE_HARD, KNEE_HARD, 0.1f, 0.0f, 50.0f, 0.0f,
                                16.0f, out_edges[e][0], out_edges[e][1]);
      ok(!omx_gate_key_active(&g_inst.gate.key), "an edge at 0 or non-finite is out", g_inst.gate.key.hp_hz,
         g_inst.gate.key.lp_hz);
      omx_gate_instance_run(&g_inst, key, l, r, ol, or_, N);
      const float en = 1.0f, kx = (float)ke, t = -30.0f, ra = 16.0f, rg = -90.0f, a = 0.1f, rl = 50.0f;
      const struct omx_gate_controls c = {&en, &kx, &t, &ra, &rg, &a, &rl, NULL, NULL};
      omx_gate_init(&g_ref, sr);
      struct omx_dyn p;
      omx_gate_resolve(&g_ref, &c, &p);
      p.knee_shift_db = g_inst.atom.knee_shift_db;
      omx_gate_run(&g_ref, &p, key, &kx, l, r, el, er, N);
      uint32_t mis = 0;
      for (uint32_t j = 0; j < N; j++) mis += ol[j] != el[j] || or_[j] != er[j];
      ok(mis == 0u, "the key filter out is the gate before it, bit for bit", mis, e);
      /* drained per case: at a rate outside the declaration every chunk records its
       * rate-is-declared precondition, and fifteen cases would overflow the ledger */
      unexplained += omx_fx_drain_ledger(sr);
    }
  ok(unexplained == 0u, "no unexplained contract violation in any case", unexplained, 0);
}

static void arm_closed_form(float sr) {
  g_arm = "B closed form";
  struct omx_gate_key k;
  const double nyq = 0.5 * (double)sr;
  const uint32_t settle = (uint32_t)(0.5f * sr), win = (uint32_t)(0.25f * sr);
  /* the high-pass, two octaves either side of the corner */
  static const float hps[] = {20.0f, 250.0f, 2000.0f, 9000.0f};
  static const double oct[] = {0.25, 0.5, 1.0, 2.0, 4.0};
  for (unsigned c = 0; c < sizeof hps / sizeof hps[0]; c++)
    for (unsigned o = 0; o < sizeof oct / sizeof oct[0]; o++) {
      const double f = hps[c] * oct[o];
      if (f > 0.45 * (double)sr) continue;
      key_at(&k, sr, hps[c], 0.0f);
      const double got = tone_gain_db(&k, f, sr, settle, win), want = hp_closed_db(f, hps[c], sr);
      ok(fabs(got - want) < 0.01, "the high-pass is the bilinear Butterworth", got, want);
    }
  /* the low-pass: -3.0103 dB at the corner wherever it sits, and unity at DC */
  const float lps[] = {100.0f, 1000.0f, 0.05f * sr, 9000.0f, 0.4f * sr < OMX_GATE_KEY_LP_HZ_MAX ? 0.4f * sr : OMX_GATE_KEY_LP_HZ_MAX};
  for (unsigned c = 0; c < sizeof lps / sizeof lps[0]; c++) {
    key_at(&k, sr, 0.0f, lps[c]);
    const double got = tone_gain_db(&k, lps[c], sr, settle, win);
    ok(fabs(got - 10.0 * log10(0.5)) < 0.01, "the low-pass is -3.0103 dB at its corner", got, 10.0 * log10(0.5));
    key_at(&k, sr, 0.0f, lps[c]);
    static float dc[OMX_GATE_CHUNK];
    float last = 0.0f;
    for (uint32_t done = 0; done < settle; done += OMX_GATE_CHUNK) {
      for (uint32_t i = 0; i < OMX_GATE_CHUNK; i++) dc[i] = 1.0f;
      omx_gate_key_run(&k, dc, OMX_GATE_CHUNK);
      last = dc[OMX_GATE_CHUNK - 1];
    }
    ok(fabs((double)last - 1.0) < 1e-4, "the low-pass is unity at DC", last, 1.0);
  }
  /* the low-pass against the analog Butterworth, a corner at most a twentieth of the rate */
  const float lpa[] = {100.0f, 1000.0f, 0.05f * sr};
  static const double below[] = {0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
  for (unsigned c = 0; c < sizeof lpa / sizeof lpa[0]; c++)
    for (unsigned o = 0; o < sizeof below / sizeof below[0]; o++) {
      const double f = lpa[c] * below[o];
      key_at(&k, sr, 0.0f, lpa[c]);
      const double got = tone_gain_db(&k, f, sr, settle, win), want = lp_analog_db(f, lpa[c]);
      ok(fabs(got - want) < 0.01, "the low-pass is the analog Butterworth below a low corner", got, want);
    }
  /* both edges: the product, the high-pass's closed form times the low-pass's at its corner */
  {
    const float hp = 200.0f, lp = 0.05f * sr;
    key_at(&k, sr, hp, lp);
    const double got = tone_gain_db(&k, lp, sr, settle, win), want = hp_closed_db(lp, hp, sr) + 10.0 * log10(0.5);
    ok(fabs(got - want) < 0.01, "both edges in is the product of the two", got, want);
    key_at(&k, sr, hp, lp);
    const double got_h = tone_gain_db(&k, hp, sr, settle, win),
                 want_h = hp_closed_db(hp, hp, sr) + lp_analog_db(hp, lp);
    ok(fabs(got_h - want_h) < 0.01, "both edges in: -3.0103 dB at the high-pass corner, the low-pass's beside it",
       got_h, want_h);
  }
  /* a corner at or above Nyquist is held at 0.999 Nyquist (omx_eq_design): the filter is finite
   * and -3.0103 dB there; the high-pass cuts the whole band below it */
  if ((double)OMX_GATE_KEY_LP_HZ_MAX >= nyq) {
    const double held = 0.999 * nyq;
    key_at(&k, sr, 0.0f, OMX_GATE_KEY_LP_HZ_MAX);
    const double got = tone_gain_db(&k, held, sr, settle, win);
    ok(isfinite(got) && fabs(got - 10.0 * log10(0.5)) < 0.01, "a low-pass corner past Nyquist is held at 0.999 Nyquist",
       got, 10.0 * log10(0.5));
    key_at(&k, sr, OMX_GATE_KEY_HP_HZ_MAX, 0.0f);
    const double cut = tone_gain_db(&k, 1000.0, sr, settle, win);
    ok(isfinite(cut) && cut < -40.0, "a high-pass corner past Nyquist cuts the band below it", cut, -40.0);
  }
}

/** The gate's settled gain, dB, with audio `a_hz` at `a_amp` on both legs and the key tone
 * `k_hz` at `k_amp` (keyed), or the audio itself (SELF); the key band `hp`..`lp`. */
static double gate_gain_db(float sr, int keyed, double a_hz, float a_amp, double k_hz, float k_amp, float hp, float lp) {
  static float l[N], r[N], k[N];
  const uint32_t settle = (uint32_t)(0.4f * sr), win = (uint32_t)(0.1f * sr);
  omx_gate_instance_init(&g_inst, sr);
  omx_gate_instance_resolve(&g_inst, 0, keyed, -40.0f, -80.0f, KNEE_HARD, KNEE_HARD, 1.0f, 0.0f, 20.0f, 0.0f, 16.0f,
                            hp, lp);
  double ein = 0.0, eout = 0.0;
  for (uint32_t done = 0; done < settle + win;) {
    const uint32_t m = settle + win - done < N ? settle + win - done : N;
    for (uint32_t i = 0; i < m; i++) {
      const double t = (double)(done + i) / (double)sr;
      l[i] = r[i] = a_amp * (float)sin(2.0 * M_PI * a_hz * t);
      k[i] = k_amp * (float)sin(2.0 * M_PI * k_hz * t);
    }
    for (uint32_t i = 0; i < m; i++)
      if (done + i >= settle) ein += (double)l[i] * l[i];
    omx_gate_instance_run(&g_inst, keyed ? k : NULL, l, r, l, r, m);
    for (uint32_t i = 0; i < m; i++)
      if (done + i >= settle) eout += (double)l[i] * l[i];
    done += m;
  }
  return 10.0 * log10(eout / ein);
}

static void arm_live(float sr) {
  g_arm = "C live";
  const float hp = 500.0f, lp = 1500.0f, amp = 0.1f; /* -20 dBFS, 20 dB above the -40 dB threshold */
  const double in_band = 866.0, low = 50.0, high = 12000.0; /* three octaves and more outside */
  /* keyed: the audio a steady 300 Hz, the key the tone */
  double g = gate_gain_db(sr, 1, 300.0, 0.5f, in_band, amp, hp, lp);
  ok(g > -0.5, "keyed: a key tone inside the band opens the gate", g, -0.5);
  g = gate_gain_db(sr, 1, 300.0, 0.5f, low, amp, hp, lp);
  ok(g < -40.0, "keyed: a key tone below the band leaves it shut", g, -40.0);
  g = gate_gain_db(sr, 1, 300.0, 0.5f, high, amp, hp, lp);
  ok(g < -40.0, "keyed: a key tone above the band leaves it shut", g, -40.0);
  g = gate_gain_db(sr, 1, 300.0, 0.5f, low, amp, 0.0f, 0.0f);
  ok(g > -0.5, "keyed, the filter out: the low tone opens it", g, -0.5);
  g = gate_gain_db(sr, 1, 300.0, 0.5f, high, amp, 0.0f, 0.0f);
  ok(g > -0.5, "keyed, the filter out: the high tone opens it", g, -0.5);
  /* SELF: the audio is the key, (l + r) / 2 through the filter */
  g = gate_gain_db(sr, 0, in_band, amp, 0.0, 0.0f, hp, lp);
  ok(g > -0.5, "SELF: audio inside the band opens the gate", g, -0.5);
  g = gate_gain_db(sr, 0, low, amp, 0.0, 0.0f, hp, lp);
  ok(g < -40.0, "SELF: audio below the band leaves it shut", g, -40.0);
  g = gate_gain_db(sr, 0, high, amp, 0.0, 0.0f, hp, lp);
  ok(g < -40.0, "SELF: audio above the band leaves it shut", g, -40.0);
  g = gate_gain_db(sr, 0, low, amp, 0.0, 0.0f, 0.0f, 0.0f);
  ok(g > -0.5, "SELF, the filter out: the low audio opens it", g, -0.5);
}

static int zero4(const double s[4]) { return s[0] == 0.0 && s[1] == 0.0 && s[2] == 0.0 && s[3] == 0.0; }

static void arm_history(float sr) {
  g_arm = "D history";
  static float l[N], r[N], k[N];
  fill(l, r, k);
  omx_gate_instance_init(&g_inst, sr);
  ok(!omx_gate_key_active(&g_inst.gate.key) && zero4(g_inst.gate.key.hp_s) && zero4(g_inst.gate.key.lp_s),
     "init: the key filter out, its history clear", 0, 0);
  omx_gate_instance_resolve(&g_inst, 0, 1, -30.0f, -90.0f, KNEE_HARD, KNEE_HARD, 1.0f, 0.0f, 50.0f, 0.0f, 16.0f, 300.0f, 5000.0f);
  omx_gate_instance_run(&g_inst, k, l, r, l, r, N);
  ok(!zero4(g_inst.gate.key.hp_s) && !zero4(g_inst.gate.key.lp_s), "a run advances both edges' history", 0, 0);
  double hs[4], ls[4];
  memcpy(hs, g_inst.gate.key.hp_s, sizeof hs);
  memcpy(ls, g_inst.gate.key.lp_s, sizeof ls);
  omx_gate_instance_resolve(&g_inst, 0, 1, -30.0f, -90.0f, KNEE_HARD, KNEE_HARD, 1.0f, 0.0f, 50.0f, 0.0f, 16.0f, 350.0f, 4000.0f);
  ok(!memcmp(hs, g_inst.gate.key.hp_s, sizeof hs) && !memcmp(ls, g_inst.gate.key.lp_s, sizeof ls),
     "a corner that moves keeps its history", 0, 0);
  omx_gate_instance_resolve(&g_inst, 1, 1, -30.0f, -90.0f, KNEE_HARD, KNEE_HARD, 1.0f, 0.0f, 50.0f, 0.0f, 16.0f, 350.0f, 4000.0f);
  omx_gate_instance_run(&g_inst, k, l, r, l, r, N);
  ok(!memcmp(hs, g_inst.gate.key.hp_s, sizeof hs) && !memcmp(ls, g_inst.gate.key.lp_s, sizeof ls),
     "a disabled gate leaves the key filter's history as it was", 0, 0);
  omx_gate_instance_resolve(&g_inst, 0, 1, -30.0f, -90.0f, KNEE_HARD, KNEE_HARD, 1.0f, 0.0f, 50.0f, 0.0f, 16.0f, 0.0f, 4000.0f);
  omx_gate_instance_resolve(&g_inst, 0, 1, -30.0f, -90.0f, KNEE_HARD, KNEE_HARD, 1.0f, 0.0f, 50.0f, 0.0f, 16.0f, 350.0f, 4000.0f);
  ok(zero4(g_inst.gate.key.hp_s) && !memcmp(ls, g_inst.gate.key.lp_s, sizeof ls),
     "an edge that goes out and comes back starts from silence, the other keeps its own", 0, 0);
  omx_gate_instance_init(&g_inst, sr);
  ok(!omx_gate_key_active(&g_inst.gate.key) && zero4(g_inst.gate.key.hp_s) && zero4(g_inst.gate.key.lp_s),
     "init again clears it", 0, 0);
}

static void arm_no_alloc(float sr) {
  g_arm = "E no allocation";
  static float l[N], r[N], k[N];
  fill(l, r, k);
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_gate_instance_init(&g_inst, sr);
  omx_gate_instance_resolve(&g_inst, 0, 1, -35.0f, -90.0f, KNEE_HARD, KNEE_HARD, 0.1f, 25.0f, 50.0f, 6.0f, 16.0f, 120.0f, 6000.0f);
  omx_gate_instance_run(&g_inst, k, l, r, l, r, 512u);
  omx_gate_instance_resolve(&g_inst, 0, 0, -35.0f, -90.0f, KNEE_HARD, KNEE_HARD, 2.0f, 0.0f, 50.0f, 0.0f, 16.0f, 800.0f, 0.0f);
  omx_gate_instance_run(&g_inst, NULL, l, r, l, r, 512u);
  g_counting = 0;
  ok(ready && g_allocs == 0, "init, resolve and run with the key filter in allocate nothing", (double)g_allocs, 0);
  void *(*volatile alloc)(size_t) = malloc;
  void (*volatile release)(void *) = free;
  g_allocs = 0;
  g_counting = 1;
  release(alloc(16));
  g_counting = 0;
  ok(g_allocs == 2, "the allocation counter counts (a malloc and a free inside the window)", (double)g_allocs, 2);
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    const float sr = OMX_FX_ORACLE_RATES[ri];
    arm_neutral(sr);
    expect_clean(sr);
    arm_closed_form(sr);
    expect_clean(sr);
    arm_live(sr);
    expect_clean(sr);
    arm_history(sr);
    expect_clean(sr);
    arm_no_alloc(sr);
    expect_clean(sr);
  }
  printf("fx/gate_keyfilter: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_FX_ORACLE_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
