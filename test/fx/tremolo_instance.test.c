// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * tremolo_instance.test.c — the tremolo instance core (omx_tremolo_instance.h), every arm at every
 * rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, a non-finite rate) is the identity;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_tremolo_process on the atom the
 *      console's per-block resolve builds from the same knobs, in both modes;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default), a mode outside the member set reads as the tremolo, and the output
 *      is finite and never louder than the input;
 *   E  a bypass->engaged edge restarts the oscillator: the re-engaged instance is a fresh one;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone (no shared state).
 *   make test-fx
 */
#include <math.h>
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_tremolo_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 12

/* The console's per-block resolve, from the same units, for arm C. */
static struct omx_tremolo console_atom(float sr, int mode, float rate_hz, float depth_pct,
                                       float mix_pct) {
  struct omx_tremolo o;
  memset(&o, 0, sizeof(o));
  o.enabled = 1;
  o.mode = mode;
  o.lfo_inc = omx_lfo_inc(rate_hz, sr);
  o.depth = 0.01f * depth_pct;
  o.mix = 0.01f * mix_pct;
  return o;
}

static void arm_refused(void) {
  OmxTremoloInstance s;
  ok(omx_tremolo_instance_init(&s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_tremolo_instance_init(&s, NAN) == 0, "A: NaN rate refused");
  ok(omx_tremolo_instance_init(&s, INFINITY) == 0, "A: infinite rate refused");
  ok(omx_tremolo_instance_init(&s, -g_sr) == 0, "A: negative rate refused");
  omx_tremolo_instance_resolve(&s, 0, OMX_TREMOLO_MODE_PAN, 5.0f, 100.0f, 100.0f);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_tremolo_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_tremolo_instance_init(&s, g_sr) == 1, "A: the declared rate is accepted");
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxTremoloInstance s;
  omx_tremolo_instance_init(&s, g_sr);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_tremolo_instance_resolve(&s, 1, OMX_TREMOLO_MODE_TREMOLO, 6.0f, 80.0f, 100.0f);
    omx_tremolo_instance_run(&s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_tremolo_instance_run(&s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const struct { int mode; float hz, depth, mix; } K[] = {
      {OMX_TREMOLO_MODE_TREMOLO, 4.0f, 50.0f, 100.0f},
      {OMX_TREMOLO_MODE_TREMOLO, 20.0f, 100.0f, 60.0f},
      {OMX_TREMOLO_MODE_PAN, 0.5f, 80.0f, 100.0f},
      {OMX_TREMOLO_MODE_PAN, 11.0f, 35.0f, 40.0f}};
  for (int k = 0; k < 4; k++) {
    OmxTremoloInstance s;
    omx_tremolo_instance_init(&s, g_sr);
    struct omx_tremolo_state ks;
    omx_tremolo_state_init(&ks);
    const struct omx_tremolo a = console_atom(g_sr, K[k].mode, K[k].hz, K[k].depth, K[k].mix);
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
      omx_tremolo_instance_resolve(&s, 0, K[k].mode, K[k].hz, K[k].depth, K[k].mix);
      omx_tremolo_instance_run(&s, l, r, ol, or_, BLK);
      omx_tremolo_process(l, r, BLK, &a, &ks);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK) || !same_bytes(or_, r0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_tremolo_process on the console's atom, bit for bit");
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  OmxTremoloInstance s;
  omx_tremolo_instance_init(&s, g_sr);
  float l[BLK], r[BLK], l0[BLK], r0[BLK];
  const float rate_max_inc = omx_lfo_inc((float)OMX_TREMOLO_RATE_RANGE_MAX, g_sr);
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    const int nan = x - x != 0.0f; /* every non-finite word reads as the default */
    const int hi = x > 1.0f;
    for (int knob = 0; knob < 3; knob++) {
      float in[3] = {6.0f, 80.0f, 100.0f};
      in[knob] = x;
      omx_tremolo_instance_resolve(&s, 0, OMX_TREMOLO_MODE_TREMOLO, in[0], in[1], in[2]);
      const struct omx_tremolo *o = &s.atom;
      char what[128];
      snprintf(what, sizeof what, "D: knob %d at %g lands inside its travel", knob, (double)x);
      int inside = o->depth >= 0.0f && o->depth <= 1.0f && o->mix >= 0.0f && o->mix <= 1.0f &&
                   o->lfo_inc > 0.0f && o->lfo_inc <= rate_max_inc;
      /* The exact landing: non-finite at the default, the large at the ceiling, the rest at the floor. */
      switch (knob) {
      case 0:
        inside &= o->lfo_inc == omx_lfo_inc(nan ? OMX_TREMOLO_INSTANCE_RATE_HZ_DEFAULT
                                            : hi ? (float)OMX_TREMOLO_RATE_RANGE_MAX
                                                 : (float)OMX_TREMOLO_RATE_RANGE_MIN, g_sr);
        break;
      case 1:
        inside &= o->depth == (nan ? 0.01f * OMX_TREMOLO_INSTANCE_DEPTH_PCT_DEFAULT : hi ? 1.0f : 0.0f);
        break;
      case 2: inside &= o->mix == (nan ? 0.01f * OMX_TREMOLO_INSTANCE_MIX_PCT_DEFAULT : hi ? 1.0f : 0.0f); break;
      }
      ok(inside, what);
      programme(l, r, BLK, (uint32_t)(h * 3 + knob) * BLK);
      memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
      omx_tremolo_instance_run(&s, l, r, l, r, BLK);
      int quieter = all_finite(l, BLK) && all_finite(r, BLK);
      for (uint32_t i = 0; i < BLK; i++) quieter &= fabsf(l[i]) <= fabsf(l0[i]) && fabsf(r[i]) <= fabsf(r0[i]);
      ok(quieter, "D: hostile knobs leave the output finite and never louder than the input");
    }
  }
  static const int MODES[] = {-1, 2, 7, 0x7fffffff};
  for (int m = 0; m < 4; m++) {
    omx_tremolo_instance_resolve(&s, 0, MODES[m], 6.0f, 80.0f, 100.0f);
    ok(s.atom.mode == OMX_TREMOLO_MODE_TREMOLO, "D: a mode outside the member set reads as the tremolo");
  }
  omx_tremolo_instance_resolve(&s, 0, OMX_TREMOLO_MODE_PAN, 6.0f, 80.0f, 100.0f);
  ok(s.atom.mode == OMX_TREMOLO_MODE_PAN, "D: the pan member is kept");
  drain_violations("D: no contract broken");
}

static void arm_reengage_restarts(void) {
  OmxTremoloInstance s, fresh;
  omx_tremolo_instance_init(&s, g_sr);
  omx_tremolo_instance_init(&fresh, g_sr);
  float l[BLK], r[BLK], fl[BLK], fr[BLK];
  for (int b = 0; b < 5; b++) { /* advance the oscillator off phase zero */
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_tremolo_instance_resolve(&s, 0, OMX_TREMOLO_MODE_PAN, 3.3f, 90.0f, 100.0f);
    omx_tremolo_instance_run(&s, l, r, l, r, BLK);
  }
  omx_tremolo_instance_resolve(&s, 1, OMX_TREMOLO_MODE_PAN, 3.3f, 90.0f, 100.0f);
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)(b + 40) * BLK);
    memcpy(fl, l, sizeof l); memcpy(fr, r, sizeof r);
    omx_tremolo_instance_resolve(&s, 0, OMX_TREMOLO_MODE_PAN, 3.3f, 90.0f, 100.0f);
    omx_tremolo_instance_resolve(&fresh, 0, OMX_TREMOLO_MODE_PAN, 3.3f, 90.0f, 100.0f);
    omx_tremolo_instance_run(&s, l, r, l, r, BLK);
    omx_tremolo_instance_run(&fresh, fl, fr, fl, fr, BLK);
    same &= same_bytes(l, fl, BLK) && same_bytes(r, fr, BLK);
  }
  ok(same, "E: a re-engaged tremolo is a fresh one, its oscillator back at phase zero");
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxTremoloInstance a, b, c, alone;
  omx_tremolo_instance_init(&a, g_sr);
  omx_tremolo_instance_init(&b, g_sr);
  omx_tremolo_instance_init(&c, g_sr);
  omx_tremolo_instance_init(&alone, g_sr);
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_tremolo_instance_resolve(&a, 0, OMX_TREMOLO_MODE_TREMOLO, 5.0f, 70.0f, 90.0f);
    omx_tremolo_instance_resolve(&b, 0, OMX_TREMOLO_MODE_TREMOLO, 5.0f, 70.0f, 90.0f);
    omx_tremolo_instance_resolve(&alone, 0, OMX_TREMOLO_MODE_TREMOLO, 5.0f, 70.0f, 90.0f);
    omx_tremolo_instance_resolve(&c, 0, OMX_TREMOLO_MODE_PAN, 13.0f, 100.0f, 100.0f);
    omx_tremolo_instance_run(&a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_tremolo_instance_run(&c, cl, cr, cl, cr, BLK); /* a different one between */
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_tremolo_instance_run(&b, bl, br, bl, br, BLK);
    omx_tremolo_instance_run(&alone, l, r, xl, xr, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
    indep &= same_bytes(al, xl, BLK) && same_bytes(ar, xr, BLK);
  }
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
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
    arm_reengage_restarts();
    arm_alias_and_independence();
  }
  return finish("tremolo_instance");
}
