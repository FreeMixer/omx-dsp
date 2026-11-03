// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * delay_instance.test.c — the FX delay instance face (omx_delay_instance.h), every arm at every
 * rate of the nine oracle rates (OMX_FX_ORACLE_RATES, 32 to 192 kHz; the six declared ones are among them):
 *   A  a rate outside the nine is refused and the face is the identity; the published
 *      latency is zero;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_fx_delay_process over caller rings
 *      on the atom the console builds from the same controls (omx_fxdelay_ms_to_samples), with
 *      ping-pong off and on, at the shortest and the longest time;
 *   D  every travel, at every hostile host value, lands inside its declared travel (a non-finite
 *      word at the declared default); a ping-pong outside its ids reads as the declared default;
 *      the output is finite;
 *   E  a bypass->engaged edge clears the rings: the re-engaged instance is a fresh one;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone (no shared state);
 *   H  init, resolve (re-engage included) and run allocate nothing: malloc, calloc, realloc and
 *      free are wrapped at link time and counted across the instance's whole life;
 *   I  the read taps: tap k at omx_fxdelay_ms_to_samples(time x num / den), bounded to the time travel,
 *      with gain_k times the balance law written out here, nothing between them; a factor outside its
 *      set reads as the tap's default.
 *   make test-fx
 */
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_delay_instance.h>

/* The colour's come-up values, the controls this oracle predates: free, a quarter, digital, no duck. */
#define COLOR_IDLE                                                                                    \
  (int)OMX_DELAY_SYNCS_DEFAULT, (int)OMX_DELAY_DIVISIONS_DEFAULT, (int)OMX_DELAY_MODES_DEFAULT,         \
      (float)OMX_DELAY_DUCK_DB_RANGE_DEFAULT, (float)OMX_DELAY_DUCK_THRESHOLD_DB_RANGE_DEFAULT,         \
      (float)OMX_DELAY_DUCK_RELEASE_MS_RANGE_DEFAULT

/* The taps' come-up values: one tap, at unity and centre; taps 1..3 at their declared defaults. */
#define TAPS_IDLE (float)OMX_FX_DELAY_TAPS_RANGE_DEFAULT, 1.0f, 0.0f, (int)OMX_DELAY_TAP1_FACTORS_DEFAULT, 0.7f, -0.5f, (int)OMX_DELAY_TAP2_FACTORS_DEFAULT, 0.5f, 0.5f, (int)OMX_DELAY_TAP3_FACTORS_DEFAULT, 0.35f, 0.0f

#include "instance_harness.h"

/* Outside the console's declaration each call below records its kernels' `rate-is-declared`; the ledger is taken after every call. */
#define omx_delay_instance_run(...) OMX_FX_CHECKPOINTED(omx_delay_instance_run, __VA_ARGS__)
#define omx_delay_instance_init(...) OMX_FX_CHECKPOINTED_INT(omx_delay_instance_init, __VA_ARGS__)
#define omx_delay_instance_resolve(...) OMX_FX_CHECKPOINTED(omx_delay_instance_resolve, __VA_ARGS__)
#define omx_fx_delay_process(...) OMX_FX_CHECKPOINTED(omx_fx_delay_process, __VA_ARGS__)

#define BLK 256u
#define NBLK 12

/* the allocation counter: fx_harness.h (linked with --wrap=malloc,calloc,realloc,free) */
#include "fx_harness.h"

/* The contract's controls, in their declared order. */
typedef struct {
  float time, feedback, tone, mix;
  int pingpong;
} Knobs;

static void resolve(OmxDelayInstance *s, int bypass, const Knobs *k) {
  omx_delay_instance_resolve(s, bypass, k->time, k->feedback, k->tone, k->mix, k->pingpong, TAPS_IDLE, COLOR_IDLE);
}

/* Heap-held: an instance carries its two 2 s rings. */
static OmxDelayInstance *inst(void) { return calloc(1, sizeof(OmxDelayInstance)); }

static void arm_refused(void) {
  OmxDelayInstance *s = inst();
  ok(omx_delay_instance_init(s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_delay_instance_init(s, NAN) == 0, "A: NaN rate refused");
  ok(omx_delay_instance_init(s, g_sr + 1.0f) == 0, "A: a rate outside the nine refused");
  ok(omx_delay_instance_init(s, 22050.0f) == 0, "A: 22.05 kHz, below the nine, refused");
  ok(omx_delay_instance_init(s, 384000.0f) == 0, "A: 384 kHz, above the nine, refused");
  const Knobs k = {120.0f, 0.5f, 0.3f, 0.5f, 1};
  resolve(s, 0, &k);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_delay_instance_run(s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_delay_instance_init(s, g_sr) == 1, "A: the rate is accepted");
  ok(omx_delay_instance_latency(s) == 0u && OMX_DELAY_INSTANCE_LATENCY_FRAMES == 0.0f,
     "A: the published latency is zero frames");
  free(s);
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxDelayInstance *s = inst();
  omx_delay_instance_init(s, g_sr);
  const Knobs k = {5.0f, 0.9f, 0.1f, 1.0f, 1};
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    resolve(s, 1, &k);
    omx_delay_instance_run(s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_delay_instance_run(s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  free(s);
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const Knobs K[] = {{3.0f, 0.6f, 0.3f, 0.5f, 0},  {3.0f, 0.6f, 0.3f, 0.5f, 1},
                            {1.0f, 0.99f, 0.0f, 1.0f, 1}, {7.5f, 0.0f, 1.0f, 0.25f, 0},
                            {0.0f, 0.4f, 0.7f, 0.8f, 1},  {2000.0f, 0.5f, 0.5f, 0.5f, 1}};
  float *rl = calloc(OMX_FXDELAY_CAP, sizeof(float)), *rr = calloc(OMX_FXDELAY_CAP, sizeof(float));
  for (int k = 0; k < (int)(sizeof K / sizeof K[0]); k++) {
    OmxDelayInstance *s = inst();
    omx_delay_instance_init(s, g_sr);
    memset(rl, 0, OMX_FXDELAY_CAP * sizeof(float));
    memset(rr, 0, OMX_FXDELAY_CAP * sizeof(float));
    struct omx_fx_delay_state ks = {rl, rr, (uint32_t)OMX_FXDELAY_CAP, 0u, 0.0f, 0.0f};
    struct omx_fx_delay a = {1, omx_fxdelay_ms_to_samples(K[k].time, g_sr), omx_fxdelay_ms_to_samples(K[k].time, g_sr),
                             K[k].feedback, K[k].mix, K[k].tone, K[k].pingpong};
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l);
      resolve(s, 0, &K[k]);
      omx_delay_instance_run(s, l, r, ol, or_, BLK);
      omx_fx_delay_process(l, r, BLK, &a, &ks, g_sr);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK);
    }
    ok(wet || K[k].time > 30.0f, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_fx_delay_process on the console's atom, bit for bit");
    free(s);
  }
  free(rl); free(rr);
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  static const float LO[4] = {(float)OMX_FX_DELAY_TIME_RANGE_MIN, (float)OMX_FX_DELAY_FEEDBACK_RANGE_MIN,
                              (float)OMX_DELAY_TONE_RANGE_MIN, (float)OMX_DELAY_MIX_RANGE_MIN};
  static const float HI[4] = {(float)OMX_FX_DELAY_TIME_RANGE_MAX, (float)OMX_FX_DELAY_FEEDBACK_RANGE_MAX,
                              (float)OMX_DELAY_TONE_RANGE_MAX, (float)OMX_DELAY_MIX_RANGE_MAX};
  static const float DEF[4] = {OMX_DELAY_INSTANCE_TIME_DEFAULT, OMX_DELAY_INSTANCE_FEEDBACK_DEFAULT,
                               OMX_DELAY_INSTANCE_TONE_DEFAULT, OMX_DELAY_INSTANCE_MIX_DEFAULT};
  float l[BLK], r[BLK];
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    for (int knob = 0; knob < 4; knob++) {
      OmxDelayInstance *s = inst(), *e = inst();
      omx_delay_instance_init(s, g_sr);
      omx_delay_instance_init(e, g_sr);
      float in[4] = {4.0f, 0.5f, 0.4f, 0.6f}, want[4];
      memcpy(want, in, sizeof want);
      in[knob] = x;
      want[knob] = land(x, LO[knob], HI[knob], DEF[knob]);
      omx_delay_instance_resolve(s, 0, in[0], in[1], in[2], in[3], 1, TAPS_IDLE, COLOR_IDLE);
      omx_delay_instance_resolve(e, 0, want[0], want[1], want[2], want[3], 1, TAPS_IDLE, COLOR_IDLE);
      char what[128];
      snprintf(what, sizeof what, "D: control %d at %g lands at %g", knob, (double)x, (double)want[knob]);
      ok(memcmp(&s->atom, &e->atom, sizeof s->atom) == 0, what);
      programme(l, r, BLK, (uint32_t)(h * 4 + knob) * BLK);
      omx_delay_instance_run(s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile controls leave the output finite");
      free(s); free(e);
    }
  }
  static const int BAD[] = {-1, 2, 7, 0x7fffffff};
  for (int m = 0; m < 4; m++) {
    OmxDelayInstance *s = inst(), *e = inst();
    omx_delay_instance_init(s, g_sr);
    omx_delay_instance_init(e, g_sr);
    omx_delay_instance_resolve(s, 0, 4.0f, 0.5f, 0.4f, 0.6f, BAD[m], TAPS_IDLE, COLOR_IDLE);
    omx_delay_instance_resolve(e, 0, 4.0f, 0.5f, 0.4f, 0.6f, OMX_DELAY_INSTANCE_PINGPONG_DEFAULT, TAPS_IDLE, COLOR_IDLE);
    ok(memcmp(&s->atom, &e->atom, sizeof s->atom) == 0, "D: a ping-pong outside its ids reads as the declared default");
    free(s); free(e);
  }
  drain_violations("D: no contract broken");
}

static void arm_reengage_clears(void) {
  OmxDelayInstance *s = inst(), *fresh = inst();
  omx_delay_instance_init(s, g_sr);
  omx_delay_instance_init(fresh, g_sr);
  const Knobs k = {4.0f, 0.8f, 0.2f, 0.7f, 1};
  float l[BLK], r[BLK], fl[BLK], fr[BLK];
  for (int b = 0; b < 5; b++) { /* fill the rings */
    programme(l, r, BLK, (uint32_t)b * BLK);
    resolve(s, 0, &k);
    omx_delay_instance_run(s, l, r, l, r, BLK);
  }
  resolve(s, 1, &k);
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)(b + 40) * BLK);
    memcpy(fl, l, sizeof l); memcpy(fr, r, sizeof r);
    resolve(s, 0, &k);
    resolve(fresh, 0, &k);
    omx_delay_instance_run(s, l, r, l, r, BLK);
    omx_delay_instance_run(fresh, fl, fr, fl, fr, BLK);
    same &= same_bytes(l, fl, BLK) && same_bytes(r, fr, BLK);
  }
  ok(same, "E: a re-engaged delay is a fresh one, its rings silent");
  free(s); free(fresh);
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxDelayInstance *a = inst(), *b = inst(), *c = inst(), *alone = inst();
  omx_delay_instance_init(a, g_sr);
  omx_delay_instance_init(b, g_sr);
  omx_delay_instance_init(c, g_sr);
  omx_delay_instance_init(alone, g_sr);
  const Knobs p = {3.0f, 0.6f, 0.3f, 0.5f, 1}, q = {1.5f, 0.9f, 0.8f, 1.0f, 0};
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    resolve(a, 0, &p);
    resolve(b, 0, &p);
    resolve(alone, 0, &p);
    resolve(c, 0, &q);
    omx_delay_instance_run(a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_delay_instance_run(c, cl, cr, cl, cr, BLK); /* a different one between */
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_delay_instance_run(b, bl, br, bl, br, BLK);
    omx_delay_instance_run(alone, l, r, xl, xr, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
    indep &= same_bytes(al, xl, BLK) && same_bytes(ar, xr, BLK);
  }
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
  free(a); free(b); free(c); free(alone);
  drain_violations("F/G: no contract broken");
}

static void arm_no_allocation(void) {
  static OmxDelayInstance s;
  float l[BLK], r[BLK];
  Knobs k = {3.0f, 0.6f, 0.3f, 0.5f, 1};
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_delay_instance_init(&s, g_sr);
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    k.time = b < 6 ? 3.0f : 1200.0f;
    k.pingpong = b & 1;
    resolve(&s, b == 5, &k);
    omx_delay_instance_run(&s, l, r, l, r, BLK);
  }
  g_counting = 0;
  ok(ready, "H: the instance came up");
  ok(g_allocs == 0, "H: init, resolve, a re-engage and run allocate nothing");
  /* The counter is live: an allocation inside the window is counted. Called through volatile
   * pointers, so the compiler cannot fold the pair away. */
  void *(*volatile alloc)(size_t) = malloc;
  void (*volatile release)(void *) = free;
  g_counting = 1;
  release(alloc(16));
  g_counting = 0;
  ok(g_allocs == 2, "H: the allocation counter counts (a malloc and a free inside the window)");
  drain_violations("H: no contract broken");
}

/* I: the read taps. An impulse on both legs, one tap per factor, mix 1 and no feedback: tap k arrives at
 * omx_fxdelay_ms_to_samples(time x num / den), the time bounded to the travel's top, at gain_k times the
 * balance law written out here: bL = 1 - p right of centre, 1 left of it; bR mirrored. */
static double bal_l(double p) { return p <= 0.0 ? 1.0 : 1.0 - p; }
static double bal_r(double p) { return p >= 0.0 ? 1.0 : 1.0 + p; }
#define TAPS_N 400000u
static float g_tl[TAPS_N], g_tr[TAPS_N], g_tol[TAPS_N], g_tor[TAPS_N];
static void arm_taps(void) {
  static const uint32_t NUM[11] = {1, 1, 3, 1, 2, 3, 1, 4, 3, 2, 3}, DEN[11] = {4, 3, 8, 2, 3, 4, 1, 3, 2, 1, 1};
  static const int SETS[3][3] = {{3, 5, 8}, {0, 2, 10}, {1, 7, 9}};
  static const float G[4] = {0.9f, 0.6f, 0.45f, 0.3f}, P[4] = {0.0f, -0.6f, 0.6f, 0.25f};
  static OmxDelayInstance inst_taps;
  OmxDelayInstance *s = &inst_taps;
  for (int set = 0; set < 3; set++) {
    const float time = set == 2 ? 1800.0f : 250.0f;
    omx_delay_instance_init(s, g_sr);
    omx_delay_instance_resolve(s, 0, time, 0.0f, 0.0f, 1.0f, 0, 4.0f, G[0], P[0], SETS[set][0], G[1], P[1], SETS[set][1], G[2], P[2],
                               SETS[set][2], G[3], P[3], COLOR_IDLE);
    uint32_t want_d[4];
    double top = 0.0;
    want_d[0] = omx_fxdelay_ms_to_samples(time, g_sr);
    for (int k = 1; k < 4; k++) {
      double ms = (double)time * NUM[SETS[set][k - 1]] / DEN[SETS[set][k - 1]];
      const int over = ms > 2000.0;
      ms = over ? 2000.0 : ms;
      want_d[k] = omx_fxdelay_ms_to_samples((float)ms, g_sr);
      int cl = -1;
      const float got_ms = omx_delay_instance_tap_ms(s, (uint32_t)k, &cl);
      ok(fabs((double)got_ms - ms) <= 1e-3 * ms && cl == over, "I: each tap's delivered time and clamp are the closed form");
    }
    for (int k = 0; k < 4; k++) top = want_d[k] > top ? want_d[k] : top;
    const uint32_t n = (uint32_t)top + 8u;
    memset(g_tl, 0, n * sizeof(float));
    memset(g_tr, 0, n * sizeof(float));
    g_tl[0] = g_tr[0] = 1.0f;
    for (uint32_t o = 0; o < n; o += 4096u)
      omx_delay_instance_run(s, g_tl + o, g_tr + o, g_tol + o, g_tor + o, n - o < 4096u ? n - o : 4096u);
    for (int k = 0; k < 4; k++) {
      double wl = 0.0, wr = 0.0;
      for (int j = 0; j < 4; j++)
        if (want_d[j] == want_d[k]) {
          wl += (double)G[j] * bal_l(P[j]);
          wr += (double)G[j] * bal_r(P[j]);
        }
      ok(fabs((double)g_tol[want_d[k]] - wl) < 1e-6 && fabs((double)g_tor[want_d[k]] - wr) < 1e-6,
         "I: tap k arrives at its time with its gain and pan");
    }
    double rest = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      int at = 0;
      for (int k = 0; k < 4; k++) at |= i == want_d[k];
      if (!at) rest += fabs((double)g_tol[i]) + fabs((double)g_tor[i]);
    }
    ok(rest == 0.0, "I: nothing arrives between the taps");
  }
  /* a factor outside the set reads as the tap's declared default */
  omx_delay_instance_init(s, g_sr);
  omx_delay_instance_resolve(s, 0, 400.0f, 0.0f, 0.0f, 1.0f, 0, 2.0f, 1.0f, 0.0f, 99, 1.0f, 0.0f, -1, 1.0f, 0.0f, 11, 1.0f, 0.0f, COLOR_IDLE);
  ok(fabsf(omx_delay_instance_tap_ms(s, 1u, NULL) - 200.0f) < 1e-3f, "I: a factor outside the set is the declared 1/2");
  ok(fabsf(omx_delay_instance_tap_ms(s, 3u, NULL) - 600.0f) < 1e-3f, "I: and tap 3's is 3/2");
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (int ri = 0; ri < (int)OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_refused();
    arm_bypass();
    arm_is_the_kernel();
    arm_clamps();
    arm_reengage_clears();
    arm_alias_and_independence();
    arm_no_allocation();
    arm_taps();
  }
  return finish("delay_instance");
}
