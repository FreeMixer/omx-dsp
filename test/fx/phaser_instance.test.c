// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * phaser_instance.test.c — the phaser instance core (omx_phaser_instance.h), every arm at every
 * rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, a non-finite rate) is the identity; the latency is zero;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_phaser_process on the atom built
 *      from the same knobs in the kernel's units;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default), the section count is an even member, and the output is finite;
 *   E  a bypass->engaged edge clears the state: the re-engaged instance is a fresh one;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone (no shared state).
 *   make test-fx
 */
#include <math.h>
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_phaser_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 12

/* The atom in the kernel's units, from the same knobs, for arm C. */
static struct omx_phaser kernel_atom(float sr, float rate_hz, float base_hz, float depth_oct, int stages,
                                     float feedback, float mix_pct) {
  struct omx_phaser o;
  memset(&o, 0, sizeof(o));
  o.enabled = 1;
  o.stages = stages;
  o.base_hz = base_hz;
  o.depth_oct = depth_oct;
  o.lfo_inc = omx_lfo_inc(rate_hz, sr);
  o.feedback = feedback;
  o.mix = 0.01f * mix_pct;
  return o;
}

/* Where a hostile word must land: every non-finite word at the default, else inside [lo, hi]. */
static float land(float x, float lo, float hi, float def) {
  if (x - x != 0.0f) return def;
  return x < lo ? lo : x > hi ? hi : x;
}

static void arm_refused(void) {
  OmxPhaserInstance s;
  ok(omx_phaser_instance_init(&s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_phaser_instance_init(&s, NAN) == 0, "A: NaN rate refused");
  ok(omx_phaser_instance_init(&s, INFINITY) == 0, "A: infinite rate refused");
  ok(omx_phaser_instance_init(&s, -g_sr) == 0, "A: negative rate refused");
  ok(omx_phaser_instance_init(&s, 2.0f * (float)OMX_PHASER_F_TOP_HZ) == 0,
     "A: a rate whose Nyquist is the sweep's top is refused");
  omx_phaser_instance_resolve(&s, 0, 1.0f, 300.0f, 3.0f, 8.0f, 0.5f, 50.0f);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_phaser_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_phaser_instance_init(&s, g_sr) == 1, "A: the declared rate is accepted");
  ok(omx_phaser_instance_latency(&s) == 0u, "A: the published latency is zero frames");
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxPhaserInstance s;
  omx_phaser_instance_init(&s, g_sr);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_phaser_instance_resolve(&s, 1, 1.0f, 300.0f, 3.0f, 8.0f, 0.5f, 50.0f);
    omx_phaser_instance_run(&s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_phaser_instance_run(&s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const struct { float hz, base, oct; int st; float fb, mix; } K[] = {
      {0.5f, 200.0f, 4.0f, 6, 0.4f, 50.0f},
      {5.0f, 2000.0f, 0.0f, 2, -0.9f, 100.0f},
      {0.05f, 50.0f, 6.0f, 12, 0.9f, 30.0f},
      {2.2f, 640.0f, 2.5f, 8, -0.3f, 75.0f}};
  for (int k = 0; k < 4; k++) {
    OmxPhaserInstance s;
    omx_phaser_instance_init(&s, g_sr);
    struct omx_phaser_state ks;
    omx_phaser_state_init(&ks);
    const struct omx_phaser a = kernel_atom(g_sr, K[k].hz, K[k].base, K[k].oct, K[k].st, K[k].fb, K[k].mix);
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
      omx_phaser_instance_resolve(&s, 0, K[k].hz, K[k].base, K[k].oct, (float)K[k].st, K[k].fb, K[k].mix);
      omx_phaser_instance_run(&s, l, r, ol, or_, BLK);
      omx_phaser_process(l, r, BLK, &a, &ks, g_sr);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK) || !same_bytes(or_, r0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_phaser_process on the same atom, bit for bit");
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  OmxPhaserInstance s;
  omx_phaser_instance_init(&s, g_sr);
  float l[BLK], r[BLK];
  static const float LO[6] = {(float)OMX_PHASER_RATE_RANGE_MIN, (float)OMX_PHASER_BASE_RANGE_MIN,
                              (float)OMX_PHASER_DEPTH_RANGE_MIN, (float)OMX_PHASER_STAGES_RANGE_MIN,
                              (float)OMX_PHASER_FEEDBACK_RANGE_MIN, (float)OMX_PHASER_MIX_RANGE_MIN};
  static const float HI[6] = {(float)OMX_PHASER_RATE_RANGE_MAX, (float)OMX_PHASER_BASE_RANGE_MAX,
                              (float)OMX_PHASER_DEPTH_RANGE_MAX, (float)OMX_PHASER_STAGES_RANGE_MAX,
                              (float)OMX_PHASER_FEEDBACK_RANGE_MAX, (float)OMX_PHASER_MIX_RANGE_MAX};
  static const float DEF[6] = {OMX_PHASER_INSTANCE_RATE_HZ_DEFAULT, OMX_PHASER_INSTANCE_BASE_HZ_DEFAULT,
                               OMX_PHASER_INSTANCE_DEPTH_OCT_DEFAULT, OMX_PHASER_INSTANCE_STAGES_DEFAULT,
                               OMX_PHASER_INSTANCE_FEEDBACK_DEFAULT, OMX_PHASER_INSTANCE_MIX_DEFAULT};
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    for (int knob = 0; knob < 6; knob++) {
      float in[6] = {1.0f, 300.0f, 3.0f, 8.0f, 0.5f, 50.0f};
      in[knob] = x;
      omx_phaser_instance_resolve(&s, 0, in[0], in[1], in[2], in[3], in[4], in[5]);
      const struct omx_phaser *o = &s.atom;
      const float want = land(x, LO[knob], HI[knob], DEF[knob]);
      char what[128];
      snprintf(what, sizeof what, "D: knob %d at %g lands at %g", knob, (double)x, (double)want);
      int inside = omx_phaser_stages_legal(o->stages) && fabsf(o->feedback) <= OMX_PHASER_FB_MAX &&
                   o->mix >= 0.0f && o->mix <= 1.0f;
      switch (knob) {
      case 0: inside &= o->lfo_inc == omx_lfo_inc(want, g_sr); break;
      case 1: inside &= o->base_hz == want; break;
      case 2: inside &= o->depth_oct == want; break;
      case 3: inside &= o->stages == (int)want; break; /* every landing (2, 6, 12) is even */
      case 4: inside &= o->feedback == want; break;
      case 5: inside &= o->mix == 0.01f * want; break;
      }
      ok(inside, what);
      programme(l, r, BLK, (uint32_t)(h * 6 + knob) * BLK);
      omx_phaser_instance_run(&s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile knobs leave the output finite");
    }
  }
  static const struct { float in; int out; } ST[] = {{2.9f, 2}, {3.1f, 4}, {5.2f, 6}, {6.9f, 6}, {11.2f, 12}};
  for (int k = 0; k < 5; k++) {
    omx_phaser_instance_resolve(&s, 0, 1.0f, 300.0f, 3.0f, ST[k].in, 0.5f, 50.0f);
    ok(s.atom.stages == ST[k].out, "D: a section count between members rounds to the nearest even one");
  }
  drain_violations("D: no contract broken");
}

static void arm_reengage_clears(void) {
  OmxPhaserInstance s, fresh;
  omx_phaser_instance_init(&s, g_sr);
  omx_phaser_instance_init(&fresh, g_sr);
  float l[BLK], r[BLK], fl[BLK], fr[BLK];
  for (int b = 0; b < 5; b++) { /* build a resonance and move the oscillator off phase zero */
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_phaser_instance_resolve(&s, 0, 3.0f, 400.0f, 4.0f, 12.0f, 0.9f, 100.0f);
    omx_phaser_instance_run(&s, l, r, l, r, BLK);
  }
  omx_phaser_instance_resolve(&s, 1, 3.0f, 400.0f, 4.0f, 12.0f, 0.9f, 100.0f);
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)(b + 40) * BLK);
    memcpy(fl, l, sizeof l); memcpy(fr, r, sizeof r);
    omx_phaser_instance_resolve(&s, 0, 3.0f, 400.0f, 4.0f, 12.0f, 0.9f, 100.0f);
    omx_phaser_instance_resolve(&fresh, 0, 3.0f, 400.0f, 4.0f, 12.0f, 0.9f, 100.0f);
    omx_phaser_instance_run(&s, l, r, l, r, BLK);
    omx_phaser_instance_run(&fresh, fl, fr, fl, fr, BLK);
    same &= same_bytes(l, fl, BLK) && same_bytes(r, fr, BLK);
  }
  ok(same, "E: a re-engaged phaser is a fresh one, its state cleared");
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxPhaserInstance a, b, c, alone;
  omx_phaser_instance_init(&a, g_sr);
  omx_phaser_instance_init(&b, g_sr);
  omx_phaser_instance_init(&c, g_sr);
  omx_phaser_instance_init(&alone, g_sr);
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_phaser_instance_resolve(&a, 0, 0.8f, 250.0f, 3.5f, 6.0f, 0.6f, 60.0f);
    omx_phaser_instance_resolve(&b, 0, 0.8f, 250.0f, 3.5f, 6.0f, 0.6f, 60.0f);
    omx_phaser_instance_resolve(&alone, 0, 0.8f, 250.0f, 3.5f, 6.0f, 0.6f, 60.0f);
    omx_phaser_instance_resolve(&c, 0, 4.0f, 1000.0f, 1.0f, 12.0f, -0.8f, 100.0f);
    omx_phaser_instance_run(&a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_phaser_instance_run(&c, cl, cr, cl, cr, BLK); /* a different one between */
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_phaser_instance_run(&b, bl, br, bl, br, BLK);
    omx_phaser_instance_run(&alone, l, r, xl, xr, BLK);
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
    arm_reengage_clears();
    arm_alias_and_independence();
  }
  return finish("phaser_instance");
}
