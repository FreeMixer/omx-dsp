// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * chorus_instance.test.c — the chorus instance core (omx_chorus_instance.h), every arm at every
 * rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, no ring, a ring short of the deepest sweep) is the identity;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_chorus_process on the atom the
 *      console's resolve_fx_chorus builds from the same knobs;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default) and the output is finite;
 *   E  a bypass->engaged edge clears the rings: silence in is silence out;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  two instances interleaved are each the instance alone (no shared state).
 *   make test-fx
 */
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_chorus_instance.h>

#include "instance_oracle.h"

#define BLK 256u
#define NBLK 12

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
  const uint32_t cap = omx_chorus_instance_cap_for(g_sr);
  float *rl = ring(cap), *rr = ring(cap);
  OmxChorusInstance s;
  ok(omx_chorus_instance_init(&s, 0.0f, rl, rr, cap) == 0, "A: rate 0 refused");
  ok(omx_chorus_instance_init(&s, NAN, rl, rr, cap) == 0, "A: NaN rate refused");
  ok(omx_chorus_instance_init(&s, g_sr, NULL, rr, cap) == 0, "A: no ring refused");
  ok(omx_chorus_instance_init(&s, g_sr, rl, rr, cap - 1u) == 0, "A: short ring refused");
  omx_chorus_instance_resolve(&s, 0, 4.0f, 12.0f, 8.0f, 100.0f, 0.5f);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_chorus_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_chorus_instance_init(&s, g_sr, rl, rr, cap) == 1, "A: the sized ring is accepted");
  free(rl); free(rr);
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  const uint32_t cap = omx_chorus_instance_cap_for(g_sr);
  float *rl = ring(cap), *rr = ring(cap);
  OmxChorusInstance s;
  omx_chorus_instance_init(&s, g_sr, rl, rr, cap);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_chorus_instance_resolve(&s, 1, 3.0f, 4.0f, 0.6f, 35.0f, 0.0f);
    omx_chorus_instance_run(&s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_chorus_instance_run(&s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  free(rl); free(rr);
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const struct { float v, d, hz, pct, sp; } K[] = {
      {3.0f, 4.0f, 0.6f, 35.0f, 0.0f}, {1.0f, 12.0f, 8.0f, 100.0f, 0.5f}, {4.0f, 0.5f, 0.05f, 60.0f, 0.25f}};
  for (int k = 0; k < 3; k++) {
    const uint32_t cap = omx_chorus_instance_cap_for(g_sr);
    float *rl = ring(cap), *rr = ring(cap), *kl = ring(cap), *kr = ring(cap);
    OmxChorusInstance s;
    omx_chorus_instance_init(&s, g_sr, rl, rr, cap);
    struct omx_chorus_state ks;
    omx_chorus_state_init(&ks, kl, kr, cap);
    const struct omx_chorus a =
        console_atom(g_sr, (int)K[k].v, K[k].d, K[k].hz, 0.01f * K[k].pct, K[k].sp);
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l);
      omx_chorus_instance_resolve(&s, 0, K[k].v, K[k].d, K[k].hz, K[k].pct, K[k].sp);
      omx_chorus_instance_run(&s, l, r, ol, or_, BLK);
      omx_chorus_process(l, r, BLK, &a, &ks);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_chorus_process on the console's atom, bit for bit");
    free(rl); free(rr); free(kl); free(kr);
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  const uint32_t cap = omx_chorus_instance_cap_for(g_sr);
  float *rl = ring(cap), *rr = ring(cap);
  OmxChorusInstance s;
  omx_chorus_instance_init(&s, g_sr, rl, rr, cap);
  float l[BLK], r[BLK];
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    const int nan = x - x != 0.0f; /* every non-finite word reads as the default */
    for (int knob = 0; knob < 5; knob++) {
      float in[5] = {3.0f, 4.0f, 0.6f, 35.0f, 0.0f};
      in[knob] = x;
      omx_chorus_instance_resolve(&s, 0, in[0], in[1], in[2], in[3], in[4]);
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
  free(rl); free(rr);
  drain_violations("D: no contract broken");
}

static void arm_reengage_clears(void) {
  const uint32_t cap = omx_chorus_instance_cap_for(g_sr);
  float *rl = ring(cap), *rr = ring(cap);
  OmxChorusInstance s;
  omx_chorus_instance_init(&s, g_sr, rl, rr, cap);
  float l[BLK], r[BLK];
  for (int b = 0; b < 4; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_chorus_instance_resolve(&s, 0, 3.0f, 12.0f, 0.6f, 100.0f, 0.0f);
    omx_chorus_instance_run(&s, l, r, l, r, BLK);
  }
  omx_chorus_instance_resolve(&s, 1, 3.0f, 12.0f, 0.6f, 100.0f, 0.0f);
  omx_chorus_instance_resolve(&s, 0, 3.0f, 12.0f, 0.6f, 100.0f, 0.0f);
  memset(l, 0, sizeof l); memset(r, 0, sizeof r);
  omx_chorus_instance_run(&s, l, r, l, r, BLK);
  ok(all_zero(l, BLK) && all_zero(r, BLK), "E: a re-engaged chorus starts silent");
  free(rl); free(rr);
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  const uint32_t cap = omx_chorus_instance_cap_for(g_sr);
  float *ring_[6];
  for (int i = 0; i < 6; i++) ring_[i] = ring(cap);
  OmxChorusInstance a, b, c;
  omx_chorus_instance_init(&a, g_sr, ring_[0], ring_[1], cap);
  omx_chorus_instance_init(&b, g_sr, ring_[2], ring_[3], cap);
  omx_chorus_instance_init(&c, g_sr, ring_[4], ring_[5], cap);
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_chorus_instance_resolve(&a, 0, 2.0f, 6.0f, 1.5f, 50.0f, 0.3f);
    omx_chorus_instance_resolve(&b, 0, 2.0f, 6.0f, 1.5f, 50.0f, 0.3f);
    omx_chorus_instance_resolve(&c, 0, 4.0f, 11.0f, 7.0f, 90.0f, 0.1f); /* a different one between */
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
  for (int i = 0; i < 6; i++) free(ring_[i]);
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
  return finish("chorus_instance");
}
