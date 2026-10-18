// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * pitch_instance.test.c — the pitch shifter instance core (omx_pitch_instance.h), every arm at
 * each of the nine RME rates (OMX_FX_RME_RATES, 32 to 192 kHz; the declared rates are six of them):
 *   A  a refused init (no rate, a rate outside the nine) is the identity; each of the nine rates
 *      is accepted, its window inside the instance's own rings; a refused, a fresh and a bypassed
 *      instance publish no latency;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_pitch_process on the atom the
 *      kernel's own omx_pitch_resolve builds from the same knobs;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default): the atom is the kernel's resolve of the landed values; the output
 *      is finite;
 *   E  a bypass->engaged edge clears the rings: silence in is silence out;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone (no shared state);
 *   H  init, resolve (re-engage included) and run allocate nothing: malloc, calloc, realloc and
 *      free are wrapped at link time and counted across the instance's whole life;
 *   I  at 100 % mix the output is the pre-filtered input delayed by the published latency,
 *      d_min + W/2 frames, measured on an impulse at the defaults and at a shift, and equal to
 *      the closed form (order - 1)/2 + WINDOW_MS·sr/2000 worked in whole numbers, never
 *      through the kernel's words (1921 frames at 96 kHz); below 100 % the
 *      published latency is zero and the dry path is undelayed; the mix crossing 100 % either way
 *      moves the published latency at the resolve that crosses it.
 *   make test-fx
 */
#include <math.h>
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_pitch_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 12

static float *ring(uint32_t cap) { return calloc(cap, sizeof(float)); }

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

/* Where a hostile word must land: every non-finite word at the default, else inside [lo, hi]. */
static float land(float x, float lo, float hi, float def) {
  if (x - x != 0.0f) return def;
  return x < lo ? lo : x > hi ? hi : x;
}

static int same_atom(const struct omx_pitch *a, const struct omx_pitch *b) {
  return a->enabled == b->enabled && a->dir == b->dir && a->window == b->window && a->step == b->step &&
         a->mix == b->mix && memcmp(a->lp, b->lp, sizeof a->lp) == 0;
}

static void arm_refused(void) {
  OmxPitchInstance s;
  ok(omx_pitch_instance_init(&s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_pitch_instance_init(&s, NAN) == 0, "A: NaN rate refused");
  ok(omx_pitch_instance_init(&s, g_sr + 1.0f) == 0, "A: a rate outside the nine refused");
  ok(omx_pitch_instance_init(&s, 22050.0f) == 0, "A: 22.05 kHz, below the nine, refused");
  ok(omx_pitch_instance_init(&s, 384000.0f) == 0, "A: 384 kHz, above the rings, refused");
  omx_pitch_instance_resolve(&s, 0, 7.0f, 0.0f, 100.0f);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_pitch_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_pitch_instance_init(&s, g_sr) == 1, "A: the rate is accepted");
  ok(s.cap == omx_pitch_instance_cap_for(g_sr) && s.cap <= OMX_PITCH_INSTANCE_RING_FLOATS,
     "A: the kernel's ring is the window's, inside the instance's own");
  ok(omx_pitch_instance_latency(&s) == 0u, "A: a fresh instance publishes no latency");
  omx_pitch_instance_resolve(&s, 1, 0.0f, 0.0f, 100.0f);
  ok(omx_pitch_instance_latency(&s) == 0u, "A: a bypassed instance publishes no latency");
  ok(omx_pitch_instance_latency(NULL) == 0u, "A: no instance publishes no latency");
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxPitchInstance s;
  omx_pitch_instance_init(&s, g_sr);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_pitch_instance_resolve(&s, 1, -5.0f, 20.0f, 80.0f);
    omx_pitch_instance_run(&s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_pitch_instance_run(&s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const struct { float st, ct, mix; } K[] = {
      {7.0f, 0.0f, 100.0f}, {-12.0f, -50.0f, 60.0f}, {12.0f, 50.0f, 100.0f}, {0.0f, 25.0f, 40.0f}};
  for (int k = 0; k < 4; k++) {
    const uint32_t cap = omx_pitch_instance_cap_for(g_sr);
    float *kl = ring(cap), *kr = ring(cap);
    OmxPitchInstance s;
    omx_pitch_instance_init(&s, g_sr);
    struct omx_pitch_state ks;
    omx_pitch_state_init(&ks, kl, kr, cap);
    struct omx_pitch a;
    omx_pitch_resolve(&a, 1, K[k].st, K[k].ct, K[k].mix, g_sr);
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
      omx_pitch_instance_resolve(&s, 0, K[k].st, K[k].ct, K[k].mix);
      omx_pitch_instance_run(&s, l, r, ol, or_, BLK);
      omx_pitch_process(l, r, BLK, &a, &ks);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK) || !same_bytes(or_, r0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_pitch_process on the kernel's resolve, bit for bit");
    free(kl); free(kr);
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  OmxPitchInstance s;
  omx_pitch_instance_init(&s, g_sr);
  float l[BLK], r[BLK];
  static const float LO[3] = {OMX_PITCH_SEMITONES_MIN, OMX_PITCH_CENTS_MIN, OMX_PITCH_MIX_MIN};
  static const float HI[3] = {OMX_PITCH_SEMITONES_MAX, OMX_PITCH_CENTS_MAX, OMX_PITCH_MIX_MAX};
  static const float DEF[3] = {OMX_PITCH_SEMITONES_DEFAULT, OMX_PITCH_CENTS_DEFAULT, OMX_PITCH_MIX_DEFAULT};
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    for (int knob = 0; knob < 3; knob++) {
      float in[3] = {5.0f, 10.0f, 70.0f};
      in[knob] = x;
      omx_pitch_instance_resolve(&s, 0, in[0], in[1], in[2]);
      float want[3] = {in[0], in[1], in[2]};
      want[knob] = land(x, LO[knob], HI[knob], DEF[knob]);
      struct omx_pitch e;
      omx_pitch_resolve(&e, 1, want[0], want[1], want[2], g_sr);
      char what[128];
      snprintf(what, sizeof what, "D: knob %d at %g lands at %g", knob, (double)x, (double)want[knob]);
      ok(same_atom(&s.atom, &e), what);
      programme(l, r, BLK, (uint32_t)(h * 3 + knob) * BLK);
      omx_pitch_instance_run(&s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile knobs leave the output finite");
    }
  }
  drain_violations("D: no contract broken");
}

static void arm_reengage_clears(void) {
  OmxPitchInstance s;
  omx_pitch_instance_init(&s, g_sr);
  float l[BLK], r[BLK];
  for (int b = 0; b < 4; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_pitch_instance_resolve(&s, 0, 7.0f, 0.0f, 100.0f);
    omx_pitch_instance_run(&s, l, r, l, r, BLK);
  }
  omx_pitch_instance_resolve(&s, 1, 7.0f, 0.0f, 100.0f);
  omx_pitch_instance_resolve(&s, 0, 7.0f, 0.0f, 100.0f);
  memset(l, 0, sizeof l); memset(r, 0, sizeof r);
  omx_pitch_instance_run(&s, l, r, l, r, BLK);
  ok(all_zero(l, BLK) && all_zero(r, BLK), "E: a re-engaged shifter starts silent");
  ok(s.state.ramp == (uint32_t)BLK * s.atom.step, "E: the ramp restarted at phase zero");
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxPitchInstance a, b, c, alone;
  omx_pitch_instance_init(&a, g_sr);
  omx_pitch_instance_init(&b, g_sr);
  omx_pitch_instance_init(&c, g_sr);
  omx_pitch_instance_init(&alone, g_sr);
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_pitch_instance_resolve(&a, 0, 3.0f, -15.0f, 90.0f);
    omx_pitch_instance_resolve(&b, 0, 3.0f, -15.0f, 90.0f);
    omx_pitch_instance_resolve(&alone, 0, 3.0f, -15.0f, 90.0f);
    omx_pitch_instance_resolve(&c, 0, -9.0f, 40.0f, 100.0f);
    omx_pitch_instance_run(&a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_pitch_instance_run(&c, cl, cr, cl, cr, BLK); /* a different one between */
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_pitch_instance_run(&b, bl, br, bl, br, BLK);
    omx_pitch_instance_run(&alone, l, r, xl, xr, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
    indep &= same_bytes(al, xl, BLK) && same_bytes(ar, xr, BLK);
  }
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
  drain_violations("F/G: no contract broken");
}

static void arm_no_allocation(void) {
  static OmxPitchInstance s;
  float l[BLK], r[BLK];
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_pitch_instance_init(&s, g_sr);
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_pitch_instance_resolve(&s, b == 5, b < 6 ? 7.0f : -4.0f, b < 3 ? 0.0f : 30.0f, 90.0f);
    omx_pitch_instance_run(&s, l, r, l, r, BLK);
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

/* d_min + W/2 at g_sr, from the kernel's own words: the centre of the window's taps. */
static uint32_t window_centre(void) {
  return (uint32_t)lroundf(omx_fdelay_min_delay(OMX_PITCH_ORDER) + 0.5f * OMX_PITCH_WINDOW_MS * 1e-3f * g_sr);
}

/* The delay, frames, at which `y` best matches `h`: the argmax of their cross-correlation. */
static uint32_t best_delay(const float *y, const float *h, uint32_t n) {
  uint32_t best = 0u;
  double top = -1.0;
  for (uint32_t d = 0; d < n; d++) {
    double c = 0.0;
    for (uint32_t i = d; i < n; i++) c += (double)y[i] * (double)h[i - d];
    if (c > top) top = c, best = d;
  }
  return best;
}

/* An impulse on L through the face engaged at its defaults: the measured delay against the face's
 * own pre-filter (its group delay is the filter's, not the window's) and the largest residual. */
static uint32_t measure_defaults(uint32_t n, double *resid) {
  static OmxPitchInstance s;
  omx_pitch_instance_init(&s, g_sr);
  omx_pitch_instance_resolve(&s, 0, OMX_PITCH_SEMITONES_DEFAULT, OMX_PITCH_CENTS_DEFAULT, OMX_PITCH_MIX_DEFAULT);
  float *l = calloc(n, sizeof(float)), *r = calloc(n, sizeof(float)), *h = calloc(n, sizeof(float));
  float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  l[0] = 1.0f;
  for (uint32_t i = 0; i < n; i++) h[i] = omx_biquad(i == 0u ? 1.0f : 0.0f, s.atom.lp, z);
  for (uint32_t o = 0; o < n; o += BLK) omx_pitch_instance_run(&s, l + o, r + o, l + o, r + o, BLK);
  const uint32_t d = best_delay(l, h, n / 2u);
  *resid = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    const double e = fabs((double)l[i] - (i >= d ? (double)h[i - d] : 0.0));
    if (e > *resid) *resid = e;
  }
  free(l); free(r); free(h);
  return d;
}

static void arm_latency(void) {
  const uint32_t want = window_centre();
  const uint32_t n = 2u * ((want + BLK) / BLK) * BLK;
  static OmxPitchInstance s;
  omx_pitch_instance_init(&s, g_sr);
  omx_pitch_instance_resolve(&s, 0, 0.0f, 0.0f, 100.0f);
  const uint32_t published = omx_pitch_instance_latency(&s);
  char what[160];
  snprintf(what, sizeof what, "I: at 100 %% the published latency is d_min + W/2 = %u frames (got %u)",
           (unsigned)want, (unsigned)published);
  ok(published == want, what);
  /* The closed form in whole numbers: d_min = (order - 1)/2 frames, W/2 = WINDOW_MS·sr/2000
   * frames, whole at each of the nine rates (1 + sr/50 at order 3 and 40 ms; 1921 at 96 kHz). */
  const uint64_t half_w = (uint64_t)OMX_PITCH_WINDOW_MS * (uint64_t)g_sr;
  const uint32_t closed = (uint32_t)((OMX_PITCH_ORDER - 1) / 2) + (uint32_t)(half_w / 2000u);
  snprintf(what, sizeof what, "I: at 100 %% the published latency is (order-1)/2 + W/2 = %u frames (got %u)",
           (unsigned)closed, (unsigned)published);
  ok(half_w % 2000u == 0u && (OMX_PITCH_ORDER - 1) % 2 == 0 && published == closed, what);

  double resid = 0.0;
  const uint32_t d = measure_defaults(n, &resid);
  snprintf(what, sizeof what, "I: at the defaults the impulse comes out %u frames late, the published %u",
           (unsigned)d, (unsigned)published);
  ok(d == published, what);
  ok(resid < 1e-4, "I: at the defaults the output is the pre-filtered input, delayed and nothing else");

  /* A shift sweeps the taps across the window; the published latency is the window's centre. */
  omx_pitch_instance_resolve(&s, 0, -5.0f, 12.0f, 100.0f);
  ok(omx_pitch_instance_latency(&s) == want, "I: a shift at 100 % publishes the same latency");

  /* Below 100 %: nothing published, and the dry path is frame-aligned with the input. */
  static OmxPitchInstance h;
  omx_pitch_instance_init(&h, g_sr);
  omx_pitch_instance_resolve(&h, 0, 0.0f, 0.0f, 50.0f);
  ok(omx_pitch_instance_latency(&h) == 0u, "I: at 50 % the published latency is zero");
  float *l = calloc(n, sizeof(float)), *r = calloc(n, sizeof(float));
  float *l0 = calloc(n, sizeof(float)), *r0 = calloc(n, sizeof(float));
  for (uint32_t o = 0; o < n; o += BLK) programme(l + o, r + o, BLK, o);
  memcpy(l0, l, n * sizeof(float)); memcpy(r0, r, n * sizeof(float));
  for (uint32_t o = 0; o < n; o += BLK) omx_pitch_instance_run(&h, l + o, r + o, l + o, r + o, BLK);
  /* Until the wet path's first tap reaches the input (its Lagrange reach starts two frames before
   * the centre), the output is the dry half of the input, frame for frame. */
  int aligned = 1, live = 0;
  for (uint32_t i = 0; i + 2u < want; i++) {
    aligned &= l[i] == 0.5f * l0[i] && r[i] == 0.5f * r0[i];
    live |= l0[i] != 0.0f;
  }
  ok(live && aligned, "I: at 50 % the dry path is undelayed (the output is half the input, frame for frame)");
  free(l); free(r); free(l0); free(r0);

  /* The mix crossing 100 % is a latency change, at the resolve that crosses it, both ways. */
  omx_pitch_instance_resolve(&h, 0, 0.0f, 0.0f, 100.0f);
  ok(omx_pitch_instance_latency(&h) == want, "I: 50 % -> 100 % publishes the window's centre");
  omx_pitch_instance_resolve(&h, 0, 0.0f, 0.0f, 99.0f);
  ok(omx_pitch_instance_latency(&h) == 0u, "I: 100 % -> 99 % publishes zero");
  omx_pitch_instance_resolve(&h, 1, 0.0f, 0.0f, 100.0f);
  ok(omx_pitch_instance_latency(&h) == 0u, "I: bypassed at 100 % publishes zero");
  omx_pitch_instance_resolve(&h, 0, 0.0f, 0.0f, 100.0f);
  ok(omx_pitch_instance_latency(&h) == want, "I: re-engaged at 100 % publishes the window's centre");
  drain_violations("I: no contract broken");
}

int main(void) {
  omx_fx_require_rate_floor();
  /* The nine cover every declared rate, so this loop tests no less than the declaration. */
  for (int k = 0; k < (int)OMX_DECLARED_RATE_COUNT; k++) {
    int found = 0;
    for (int ri = 0; ri < (int)OMX_FX_RME_RATE_COUNT; ri++) found |= OMX_FX_RME_RATES[ri] == OMX_DECLARED_RATES[k];
    if (!found) {
      fprintf(stderr, "OMX_FX_RME_RATES lacks the declared %.0f Hz\n", (double)OMX_DECLARED_RATES[k]);
      return 2;
    }
  }
  for (int ri = 0; ri < (int)OMX_FX_RME_RATE_COUNT; ri++) {
    g_sr = OMX_FX_RME_RATES[ri];
    arm_refused();
    arm_bypass();
    arm_is_the_kernel();
    arm_clamps();
    arm_reengage_clears();
    arm_alias_and_independence();
    arm_no_allocation();
    arm_latency();
  }
  printf("fx/pitch_instance: %d RME rates\n", (int)OMX_FX_RME_RATE_COUNT);
  return finish("pitch_instance");
}
