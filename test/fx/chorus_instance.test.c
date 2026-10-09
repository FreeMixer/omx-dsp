// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * chorus_instance.test.c — the chorus instance core (omx_chorus_instance.h), every arm at every
 * rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, a rate the console does not declare) is the identity; every
 *      declared rate is accepted, its deepest sweep inside the instance's own rings;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_chorus_process on the atom the
 *      console's resolve_fx_chorus builds from the same knobs;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default) and the output is finite;
 *   E  a bypass->engaged edge clears the rings: silence in is silence out;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  two instances interleaved are each the instance alone (no shared state);
 *   H  init, resolve (re-engage included) and run allocate nothing: malloc, calloc, realloc and
 *      free are wrapped at link time and counted across the instance's whole life.
 *   make test-fx
 */
#include <math.h>
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_chorus_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 12

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

static float *ring(uint32_t cap) { return calloc(cap, sizeof(float)); }

/* The console's resolve_fx_chorus, from the same units, for arm C. */
static struct omx_chorus console_atom(float sr, int voices, float depth_ms, float rate_hz,
                                      float mix, float spread) {
  struct omx_chorus o;
  memset(&o, 0, sizeof(o));
  o.enabled = 1;
  o.voices = voices;
  o.base_samples = OMX_CHORUS_BASE_MS * 0.001f * sr;
  o.depth_samples = depth_ms * 0.001f * sr;
  o.lfo_inc = omx_lfo_inc(rate_hz, sr);
  o.mix = mix;
  o.spread = spread;
  return o;
}

static void arm_refused(void) {
  static OmxChorusInstance s;
  ok(omx_chorus_instance_init(&s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_chorus_instance_init(&s, NAN) == 0, "A: NaN rate refused");
  ok(omx_chorus_instance_init(&s, g_sr + 1.0f) == 0, "A: an undeclared rate refused");
  omx_chorus_instance_resolve(&s, 0, 0.5f, 8.0f, 12.0f, 4.0f, 100.0f);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_chorus_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_chorus_instance_init(&s, g_sr) == 1, "A: the declared rate is accepted");
  ok(s.cap == omx_chorus_instance_cap_for(g_sr) && s.cap <= OMX_CHORUS_CAP,
     "A: the kernel's ring is the deepest sweep's, inside the instance's own");
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  static OmxChorusInstance s;
  omx_chorus_instance_init(&s, g_sr);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_chorus_instance_resolve(&s, 1, 0.0f, 0.6f, 4.0f, 3.0f, 35.0f);
    omx_chorus_instance_run(&s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_chorus_instance_run(&s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const struct { float v, d, hz, pct, sp; } K[] = {
      {3.0f, 4.0f, 0.6f, 35.0f, 0.0f}, {1.0f, 12.0f, 8.0f, 100.0f, 0.5f}, {4.0f, 0.5f, 0.05f, 60.0f, 0.25f}};
  for (int k = 0; k < 3; k++) {
    const uint32_t cap = omx_chorus_instance_cap_for(g_sr);
    float *kl = ring(cap), *kr = ring(cap);
    static OmxChorusInstance s;
    omx_chorus_instance_init(&s, g_sr);
    struct omx_chorus_state ks;
    omx_chorus_state_init(&ks, kl, kr, cap);
    const struct omx_chorus a =
        console_atom(g_sr, (int)K[k].v, K[k].d, K[k].hz, 0.01f * K[k].pct, K[k].sp);
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l);
      omx_chorus_instance_resolve(&s, 0, K[k].sp, K[k].hz, K[k].d, K[k].v, K[k].pct);
      omx_chorus_instance_run(&s, l, r, ol, or_, BLK);
      omx_chorus_process(l, r, BLK, &a, &ks);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_chorus_process on the console's atom, bit for bit");
    free(kl); free(kr);
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  static OmxChorusInstance s;
  omx_chorus_instance_init(&s, g_sr);
  float l[BLK], r[BLK];
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    const int nan = x - x != 0.0f; /* every non-finite word reads as the default */
    for (int knob = 0; knob < 5; knob++) {
      float in[5] = {3.0f, 4.0f, 0.6f, 35.0f, 0.0f};
      in[knob] = x;
      omx_chorus_instance_resolve(&s, 0, in[4], in[2], in[1], in[0], in[3]);
      const struct omx_chorus *o = &s.atom;
      char what[128];
      snprintf(what, sizeof what, "D: knob %d at %g lands inside its travel", knob, (double)x);
      int inside = o->voices >= OMX_CHORUS_VOICES_RANGE_MIN && o->voices <= OMX_CHORUS_VOICES_RANGE_MAX &&
                   o->depth_samples >= 0.0f && o->depth_samples <= (float)OMX_CHORUS_DEPTH_RANGE_MAX * 0.001f * g_sr &&
                   o->mix >= 0.0f && o->mix <= 1.0f && o->spread >= 0.0f &&
                   o->spread <= OMX_CHORUS_SPREAD_MAX && o->lfo_inc > 0.0f &&
                   o->lfo_inc <= omx_lfo_inc((float)OMX_CHORUS_RATE_RANGE_MAX, g_sr);
      /* The exact landing: non-finite at the default, the large at the ceiling, the negative at the floor. */
      const int hi = x > 1.0f;
      switch (knob) {
      case 0: inside &= o->voices == (nan ? OMX_CHORUS_VOICES_RANGE_DEFAULT : hi ? 4 : 1); break;
      case 1: inside &= o->depth_samples == (nan ? OMX_CHORUS_INSTANCE_DEPTH_MS_DEFAULT * 0.001f * g_sr
                                          : hi ? (float)OMX_CHORUS_DEPTH_RANGE_MAX * 0.001f * g_sr : 0.0f); break;
      case 2:
        inside &= o->lfo_inc == omx_lfo_inc(nan ? OMX_CHORUS_RATE_RANGE_DEFAULT
                                            : hi ? (float)OMX_CHORUS_RATE_RANGE_MAX
                                                 : OMX_CHORUS_RATE_RANGE_MIN, g_sr);
        break;
      case 3: inside &= o->mix == (nan ? 0.01f * OMX_CHORUS_INSTANCE_MIX_PCT_DEFAULT : hi ? 1.0f : 0.0f); break;
      case 4: inside &= o->spread == (nan ? 0.0f : hi ? OMX_CHORUS_SPREAD_MAX : 0.0f); break;
      }
      ok(inside, what);
      programme(l, r, BLK, (uint32_t)(h * 5 + knob) * BLK);
      omx_chorus_instance_run(&s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile knobs leave the output finite");
    }
  }
  drain_violations("D: no contract broken");
}

static void arm_reengage_clears(void) {
  static OmxChorusInstance s;
  omx_chorus_instance_init(&s, g_sr);
  float l[BLK], r[BLK];
  for (int b = 0; b < 4; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_chorus_instance_resolve(&s, 0, 0.0f, 0.6f, 12.0f, 3.0f, 100.0f);
    omx_chorus_instance_run(&s, l, r, l, r, BLK);
  }
  omx_chorus_instance_resolve(&s, 1, 0.0f, 0.6f, 12.0f, 3.0f, 100.0f);
  omx_chorus_instance_resolve(&s, 0, 0.0f, 0.6f, 12.0f, 3.0f, 100.0f);
  memset(l, 0, sizeof l); memset(r, 0, sizeof r);
  omx_chorus_instance_run(&s, l, r, l, r, BLK);
  ok(all_zero(l, BLK) && all_zero(r, BLK), "E: a re-engaged chorus starts silent");
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  static OmxChorusInstance a, b, c;
  omx_chorus_instance_init(&a, g_sr);
  omx_chorus_instance_init(&b, g_sr);
  omx_chorus_instance_init(&c, g_sr);
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_chorus_instance_resolve(&a, 0, 0.3f, 1.5f, 6.0f, 2.0f, 50.0f);
    omx_chorus_instance_resolve(&b, 0, 0.3f, 1.5f, 6.0f, 2.0f, 50.0f);
    omx_chorus_instance_resolve(&c, 0, 0.1f, 7.0f, 11.0f, 4.0f, 90.0f); /* a different one between */
    omx_chorus_instance_run(&a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_chorus_instance_run(&c, cl, cr, cl, cr, BLK);
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_chorus_instance_run(&b, bl, br, bl, br, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
  }
  indep = alias;
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
  drain_violations("F/G: no contract broken");
}

static void arm_no_allocation(void) {
  static OmxChorusInstance s;
  float l[BLK], r[BLK];
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_chorus_instance_init(&s, g_sr);
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_chorus_instance_resolve(&s, b == 5, b < 3 ? 0.0f : 0.25f, 0.6f, b < 6 ? 4.0f : 9.0f, 3.0f, 50.0f);
    omx_chorus_instance_run(&s, l, r, l, r, BLK);
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

int main(void) {
  omx_fx_require_rate_floor();
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    arm_refused();
    arm_bypass();
    arm_is_the_kernel();
    arm_clamps();
    arm_reengage_clears();
    arm_alias_and_independence();
    arm_no_allocation();
  }
  return finish("chorus_instance");
}
