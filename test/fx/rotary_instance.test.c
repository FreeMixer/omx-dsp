// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * rotary_instance.test.c — the rotary speaker instance core (omx_rotary_instance.h), every arm at
 * every rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, a non-finite rate, a rate the console does not declare) is the
 *      identity; the published latency is zero;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_rotary_process on the atom the
 *      kernel's own omx_rotary_resolve builds each block from the same knobs, at every speed;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default): the atom is the kernel's resolve of the landed values; a speed
 *      outside the member set reads as `slow`; the output is finite;
 *   E  a bypass->engaged edge re-arms the state: the re-engaged instance is a fresh one;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone (no shared state).
 *   make test-fx
 */
#include <math.h>
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_rotary_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 12

/* Where a hostile word must land: every non-finite word at the default, else inside [lo, hi]. */
static float land(float x, float lo, float hi, float def) {
  if (x - x != 0.0f) return def;
  return x < lo ? lo : x > hi ? hi : x;
}

static int same_rotor(const struct omx_rotor *a, const struct omx_rotor *b) {
  return memcmp(a, b, sizeof *a) == 0;
}

static int same_atom(const struct omx_rotary *a, const struct omx_rotary *b) {
  return a->enabled == b->enabled && same_rotor(&a->drum, &b->drum) && same_rotor(&a->horn, &b->horn) &&
         memcmp(a->xo, b->xo, sizeof a->xo) == 0 && a->g_lo == b->g_lo && a->g_hi == b->g_hi &&
         a->mix == b->mix && a->dry == b->dry;
}

/* Heap-held: the state's inline rings make an instance a few kilobytes. */
static OmxRotaryInstance *inst(void) { return calloc(1, sizeof(OmxRotaryInstance)); }

static void arm_refused(void) {
  OmxRotaryInstance *s = inst();
  ok(omx_rotary_instance_init(s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_rotary_instance_init(s, NAN) == 0, "A: NaN rate refused");
  ok(omx_rotary_instance_init(s, g_sr + 1.0f) == 0, "A: an undeclared rate refused");
  omx_rotary_instance_resolve(s, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 0.0f, 100.0f, OMX_ROTARY_FAST);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_rotary_instance_run(s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_rotary_instance_init(s, g_sr) == 1, "A: the declared rate is accepted");
  ok(omx_rotary_instance_latency(s) == 0u, "A: the published latency is zero frames");
  free(s);
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxRotaryInstance *s = inst();
  omx_rotary_instance_init(s, g_sr);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_rotary_instance_resolve(s, 1, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 20.0f, 100.0f, OMX_ROTARY_FAST);
    omx_rotary_instance_run(s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_rotary_instance_run(s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  free(s);
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const struct { float hs, hf, ds, df, acc, bal, mix; int speed; } K[] = {
      {0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 0.0f, 100.0f, OMX_ROTARY_FAST},
      {2.0f, 10.0f, 0.1f, 3.0f, 0.25f, -100.0f, 60.0f, OMX_ROTARY_SLOW},
      {0.1f, 3.0f, 2.0f, 10.0f, 4.0f, 100.0f, 30.0f, OMX_ROTARY_FAST},
      {0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 35.0f, 100.0f, OMX_ROTARY_STOP}};
  for (int k = 0; k < 4; k++) {
    OmxRotaryInstance *s = inst();
    omx_rotary_instance_init(s, g_sr);
    struct omx_rotary_state *ks = calloc(1, sizeof *ks);
    omx_rotary_init(ks);
    struct omx_rotary a;
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
      omx_rotary_instance_resolve(s, 0, K[k].hs, K[k].hf, K[k].ds, K[k].df, K[k].acc, K[k].bal, K[k].mix,
                                  K[k].speed);
      omx_rotary_instance_run(s, l, r, ol, or_, BLK);
      omx_rotary_resolve(&a, ks, 1, K[k].speed, K[k].hs, K[k].hf, K[k].ds, K[k].df, K[k].acc,
                         0.01f * K[k].bal, 0.01f * K[k].mix, g_sr);
      omx_rotary_process(l, r, BLK, &a, ks);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK) || !same_bytes(or_, r0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_rotary_process on the kernel's resolve, bit for bit");
    free(s); free(ks);
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  float l[BLK], r[BLK];
  static const float LO[7] = {(float)OMX_ROTARY_HORN_SLOW_RANGE_MIN, (float)OMX_ROTARY_HORN_FAST_RANGE_MIN,
                              (float)OMX_ROTARY_DRUM_SLOW_RANGE_MIN, (float)OMX_ROTARY_DRUM_FAST_RANGE_MIN,
                              (float)OMX_ROTARY_ACCEL_RANGE_MIN,     (float)OMX_ROTARY_BALANCE_RANGE_MIN,
                              (float)OMX_ROTARY_MIX_RANGE_MIN};
  static const float HI[7] = {(float)OMX_ROTARY_HORN_SLOW_RANGE_MAX, (float)OMX_ROTARY_HORN_FAST_RANGE_MAX,
                              (float)OMX_ROTARY_DRUM_SLOW_RANGE_MAX, (float)OMX_ROTARY_DRUM_FAST_RANGE_MAX,
                              (float)OMX_ROTARY_ACCEL_RANGE_MAX,     (float)OMX_ROTARY_BALANCE_RANGE_MAX,
                              (float)OMX_ROTARY_MIX_RANGE_MAX};
  static const float DEF[7] = {OMX_ROTARY_INSTANCE_HORN_SLOW_HZ_DEFAULT, OMX_ROTARY_INSTANCE_HORN_FAST_HZ_DEFAULT,
                               OMX_ROTARY_INSTANCE_DRUM_SLOW_HZ_DEFAULT, OMX_ROTARY_INSTANCE_DRUM_FAST_HZ_DEFAULT,
                               OMX_ROTARY_INSTANCE_ACCEL_DEFAULT,        OMX_ROTARY_INSTANCE_BALANCE_DEFAULT,
                               OMX_ROTARY_INSTANCE_MIX_DEFAULT};
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    for (int knob = 0; knob < 7; knob++) {
      /* A fresh instance per landing, so the expected atom resolves against the same state. */
      OmxRotaryInstance *s = inst();
      omx_rotary_instance_init(s, g_sr);
      float in[7] = {0.8f, 6.8f, 0.7f, 5.9f, 1.5f, 20.0f, 80.0f};
      in[knob] = x;
      float want[7];
      memcpy(want, in, sizeof want);
      want[knob] = land(x, LO[knob], HI[knob], DEF[knob]);
      struct omx_rotary_state *es = calloc(1, sizeof *es);
      omx_rotary_init(es);
      struct omx_rotary e;
      omx_rotary_resolve(&e, es, 1, OMX_ROTARY_FAST, want[0], want[1], want[2], want[3], want[4],
                         0.01f * want[5], 0.01f * want[6], g_sr);
      omx_rotary_instance_resolve(s, 0, in[0], in[1], in[2], in[3], in[4], in[5], in[6], OMX_ROTARY_FAST);
      char what[128];
      snprintf(what, sizeof what, "D: knob %d at %g lands at %g", knob, (double)x, (double)want[knob]);
      ok(same_atom(&s->atom, &e), what);
      programme(l, r, BLK, (uint32_t)(h * 7 + knob) * BLK);
      omx_rotary_instance_run(s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile knobs leave the output finite");
      free(s); free(es);
    }
  }
  static const int SPEEDS[] = {-1, 3, 7, 0x7fffffff};
  for (int m = 0; m < 4; m++) {
    OmxRotaryInstance *s = inst(), *slow = inst();
    omx_rotary_instance_init(s, g_sr);
    omx_rotary_instance_init(slow, g_sr);
    omx_rotary_instance_resolve(s, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 0.0f, 100.0f, SPEEDS[m]);
    omx_rotary_instance_resolve(slow, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 0.0f, 100.0f, OMX_ROTARY_SLOW);
    ok(same_atom(&s->atom, &slow->atom), "D: a speed outside the member set reads as slow");
    free(s); free(slow);
  }
  drain_violations("D: no contract broken");
}

static void arm_reengage_rearms(void) {
  OmxRotaryInstance *s = inst(), *fresh = inst();
  omx_rotary_instance_init(s, g_sr);
  omx_rotary_instance_init(fresh, g_sr);
  float l[BLK], r[BLK], fl[BLK], fr[BLK];
  for (int b = 0; b < 5; b++) { /* spin the rotors up and fill the rings */
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_rotary_instance_resolve(s, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 0.0f, 100.0f, OMX_ROTARY_FAST);
    omx_rotary_instance_run(s, l, r, l, r, BLK);
  }
  omx_rotary_instance_resolve(s, 1, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 0.0f, 100.0f, OMX_ROTARY_FAST);
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)(b + 40) * BLK);
    memcpy(fl, l, sizeof l); memcpy(fr, r, sizeof r);
    omx_rotary_instance_resolve(s, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 0.0f, 100.0f, OMX_ROTARY_FAST);
    omx_rotary_instance_resolve(fresh, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 0.0f, 100.0f, OMX_ROTARY_FAST);
    omx_rotary_instance_run(s, l, r, l, r, BLK);
    omx_rotary_instance_run(fresh, fl, fr, fl, fr, BLK);
    same &= same_bytes(l, fl, BLK) && same_bytes(r, fr, BLK);
  }
  ok(same, "E: a re-engaged rotary is a fresh one, its rings silent and its rotors from rest");
  free(s); free(fresh);
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxRotaryInstance *a = inst(), *b = inst(), *c = inst(), *alone = inst();
  omx_rotary_instance_init(a, g_sr);
  omx_rotary_instance_init(b, g_sr);
  omx_rotary_instance_init(c, g_sr);
  omx_rotary_instance_init(alone, g_sr);
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_rotary_instance_resolve(a, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 10.0f, 90.0f, OMX_ROTARY_FAST);
    omx_rotary_instance_resolve(b, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 10.0f, 90.0f, OMX_ROTARY_FAST);
    omx_rotary_instance_resolve(alone, 0, 0.8f, 6.8f, 0.7f, 5.9f, 1.0f, 10.0f, 90.0f, OMX_ROTARY_FAST);
    omx_rotary_instance_resolve(c, 0, 1.5f, 9.0f, 1.2f, 8.0f, 0.5f, -60.0f, 100.0f, OMX_ROTARY_SLOW);
    omx_rotary_instance_run(a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_rotary_instance_run(c, cl, cr, cl, cr, BLK); /* a different one between */
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_rotary_instance_run(b, bl, br, bl, br, BLK);
    omx_rotary_instance_run(alone, l, r, xl, xr, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
    indep &= same_bytes(al, xl, BLK) && same_bytes(ar, xr, BLK);
  }
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
  free(a); free(b); free(c); free(alone);
  drain_violations("F/G: no contract broken");
}

int main(void) {
  omx_fx_require_rate_floor();
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    arm_refused();
    arm_bypass();
    arm_is_the_kernel();
    arm_clamps();
    arm_reengage_rearms();
    arm_alias_and_independence();
  }
  return finish("rotary_instance");
}
