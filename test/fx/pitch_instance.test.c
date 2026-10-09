// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * pitch_instance.test.c — the pitch shifter instance core (omx_pitch_instance.h), every arm at
 * every rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, no ring, a ring short of the window) is the identity; the
 *      published latency is zero;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_pitch_process on the atom the
 *      kernel's own omx_pitch_resolve builds from the same knobs;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default): the atom is the kernel's resolve of the landed values; the output
 *      is finite;
 *   E  a bypass->engaged edge clears the rings: silence in is silence out;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone (no shared state).
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
  const uint32_t cap = omx_pitch_instance_cap_for(g_sr);
  float *rl = ring(cap), *rr = ring(cap);
  OmxPitchInstance s;
  ok(omx_pitch_instance_init(&s, 0.0f, rl, rr, cap) == 0, "A: rate 0 refused");
  ok(omx_pitch_instance_init(&s, NAN, rl, rr, cap) == 0, "A: NaN rate refused");
  ok(omx_pitch_instance_init(&s, g_sr, NULL, rr, cap) == 0, "A: no ring refused");
  ok(omx_pitch_instance_init(&s, g_sr, rl, rl, cap) == 0, "A: one ring for both legs refused");
  ok(omx_pitch_instance_init(&s, g_sr, rl, rr, cap - 1u) == 0, "A: short ring refused");
  omx_pitch_instance_resolve(&s, 0, 7.0f, 0.0f, 100.0f);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_pitch_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_pitch_instance_init(&s, g_sr, rl, rr, cap) == 1, "A: the sized ring is accepted");
  ok(omx_pitch_instance_latency(&s) == 0u, "A: the published latency is zero frames");
  free(rl); free(rr);
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  const uint32_t cap = omx_pitch_instance_cap_for(g_sr);
  float *rl = ring(cap), *rr = ring(cap);
  OmxPitchInstance s;
  omx_pitch_instance_init(&s, g_sr, rl, rr, cap);
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
  free(rl); free(rr);
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const struct { float st, ct, mix; } K[] = {
      {7.0f, 0.0f, 100.0f}, {-12.0f, -50.0f, 60.0f}, {12.0f, 50.0f, 100.0f}, {0.0f, 25.0f, 40.0f}};
  for (int k = 0; k < 4; k++) {
    const uint32_t cap = omx_pitch_instance_cap_for(g_sr);
    float *rl = ring(cap), *rr = ring(cap), *kl = ring(cap), *kr = ring(cap);
    OmxPitchInstance s;
    omx_pitch_instance_init(&s, g_sr, rl, rr, cap);
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
    free(rl); free(rr); free(kl); free(kr);
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  const uint32_t cap = omx_pitch_instance_cap_for(g_sr);
  float *rl = ring(cap), *rr = ring(cap);
  OmxPitchInstance s;
  omx_pitch_instance_init(&s, g_sr, rl, rr, cap);
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
  free(rl); free(rr);
  drain_violations("D: no contract broken");
}

static void arm_reengage_clears(void) {
  const uint32_t cap = omx_pitch_instance_cap_for(g_sr);
  float *rl = ring(cap), *rr = ring(cap);
  OmxPitchInstance s;
  omx_pitch_instance_init(&s, g_sr, rl, rr, cap);
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
  free(rl); free(rr);
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  const uint32_t cap = omx_pitch_instance_cap_for(g_sr);
  float *rg[8];
  for (int i = 0; i < 8; i++) rg[i] = ring(cap);
  OmxPitchInstance a, b, c, alone;
  omx_pitch_instance_init(&a, g_sr, rg[0], rg[1], cap);
  omx_pitch_instance_init(&b, g_sr, rg[2], rg[3], cap);
  omx_pitch_instance_init(&c, g_sr, rg[4], rg[5], cap);
  omx_pitch_instance_init(&alone, g_sr, rg[6], rg[7], cap);
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
  for (int i = 0; i < 8; i++) free(rg[i]);
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
  return finish("pitch_instance");
}
