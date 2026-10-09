// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * limiter_instance.test.c — the precision limiter instance core (omx_limiter_instance.h), every
 * arm at every rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, a rate the console does not declare, no memory, a ring short of
 *      the longest look-ahead) is the identity and publishes no latency;
 *   B  bypassed is the identity byte for byte, in place and out of place, and publishes no latency;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_limiter_process on a state armed
 *      at the same look-ahead and the atom of the same knobs, and it publishes that state's `T`;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default), the look-ahead as the armed frame count; the output is finite and
 *      inside the ceiling;
 *   E  a bypass->engaged edge re-arms the state: the re-engaged instance is a fresh one; a
 *      look-ahead move re-arms it at the new latency;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone (no shared state).
 *   make test-fx
 */
#include <math.h>
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_limiter_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 12

/* Where a hostile word must land: every non-finite word at the default, else inside [lo, hi]. */
static float land(float x, float lo, float hi, float def) {
  if (x - x != 0.0f) return def;
  return x < lo ? lo : x > hi ? hi : x;
}

/* One instance and the caller's memory for it. */
typedef struct {
  OmxLimiterInstance s;
  float *mem;
  uint32_t *idx;
  uint32_t cap;
} Box;

static void box_new(Box *b) {
  b->cap = omx_limiter_instance_cap_for(g_sr);
  b->mem = calloc(omx_limiter_mem_floats(b->cap), sizeof(float));
  b->idx = calloc(b->cap, sizeof(uint32_t));
  omx_limiter_instance_init(&b->s, g_sr, b->mem, b->idx, b->cap);
}

static void box_free(Box *b) {
  free(b->mem);
  free(b->idx);
}

/* A programme block, loud: peaks well above every ceiling the arms use. */
static void loud(float *l, float *r, uint32_t n, uint32_t t0) {
  programme(l, r, n, t0);
  for (uint32_t i = 0; i < n; i++) {
    l[i] *= 1.6f;
    r[i] *= 1.6f;
  }
}

static void arm_refused(void) {
  const uint32_t cap = omx_limiter_instance_cap_for(g_sr);
  float *mem = calloc(omx_limiter_mem_floats(cap), sizeof(float));
  uint32_t *idx = calloc(cap, sizeof(uint32_t));
  OmxLimiterInstance s;
  ok(omx_limiter_instance_init(&s, 0.0f, mem, idx, cap) == 0, "A: rate 0 refused");
  ok(omx_limiter_instance_init(&s, NAN, mem, idx, cap) == 0, "A: NaN rate refused");
  ok(omx_limiter_instance_init(&s, g_sr + 1.0f, mem, idx, cap) == 0, "A: an undeclared rate refused");
  ok(omx_limiter_instance_init(&s, g_sr, NULL, idx, cap) == 0, "A: no memory refused");
  ok(omx_limiter_instance_init(&s, g_sr, mem, NULL, cap) == 0, "A: no index memory refused");
  ok(omx_limiter_instance_init(&s, g_sr, mem, idx, cap - 1u) == 0, "A: short ring refused");
  omx_limiter_instance_resolve(&s, 0, -6.0f, 2.0f, 50.0f);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  loud(l, r, BLK, 0);
  omx_limiter_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_limiter_instance_latency(&s) == 0u, "A: a refused instance publishes no latency");
  ok(omx_limiter_instance_init(&s, g_sr, mem, idx, cap) == 1, "A: the sized ring is accepted");
  free(mem); free(idx);
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  Box b;
  box_new(&b);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int k = 0; k < NBLK; k++) {
    loud(l, r, BLK, (uint32_t)k * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_limiter_instance_resolve(&b.s, 1, -6.0f, 2.0f, 50.0f);
    omx_limiter_instance_run(&b.s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_limiter_instance_run(&b.s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  ok(omx_limiter_instance_latency(&b.s) == 0u, "B: bypassed publishes no latency");
  box_free(&b);
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const struct { float ceil, la, rel; } K[] = {
      {-6.0f, 1.5f, 50.0f}, {-12.0f, 0.5f, 1.0f}, {-1.0f, 5.0f, 1000.0f}, {-3.3f, 2.7f, 120.0f}};
  for (int k = 0; k < 4; k++) {
    Box b;
    box_new(&b);
    float *kmem = calloc(omx_limiter_mem_floats(b.cap), sizeof(float));
    uint32_t *kidx = calloc(b.cap, sizeof(uint32_t));
    struct omx_limiter_state ks;
    omx_limiter_init(&ks, K[k].la, g_sr, kmem, kidx, b.cap);
    const struct omx_limiter a = {1, K[k].ceil, K[k].rel};
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
    int same = 1, wet = 0;
    for (int n = 0; n < NBLK; n++) {
      loud(l, r, BLK, (uint32_t)n * BLK);
      memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
      omx_limiter_instance_resolve(&b.s, 0, K[k].ceil, K[k].la, K[k].rel);
      omx_limiter_instance_run(&b.s, l, r, ol, or_, BLK);
      omx_limiter_process(l, r, BLK, &a, &ks);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK) || !same_bytes(or_, r0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_limiter_process on the same state and atom, bit for bit");
    ok(omx_limiter_instance_latency(&b.s) == omx_limiter_latency(&ks),
       "C: the engaged instance publishes the armed state's latency");
    box_free(&b);
    free(kmem); free(kidx);
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  Box b;
  box_new(&b);
  float l[BLK], r[BLK];
  static const float LO[3] = {OMX_LIMITER_CEILING_DB_MIN, OMX_LIMITER_LOOKAHEAD_MS_MIN, OMX_LIMITER_RELEASE_MS_MIN};
  static const float HI[3] = {OMX_LIMITER_CEILING_DB_MAX, OMX_LIMITER_LOOKAHEAD_MS_MAX, OMX_LIMITER_RELEASE_MS_MAX};
  static const float DEF[3] = {OMX_LIMITER_CEILING_DB_DEFAULT, OMX_LIMITER_LOOKAHEAD_MS_DEFAULT,
                               OMX_LIMITER_RELEASE_MS_DEFAULT};
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    for (int knob = 0; knob < 3; knob++) {
      float in[3] = {-6.0f, 2.0f, 80.0f};
      in[knob] = x;
      omx_limiter_instance_resolve(&b.s, 0, in[0], in[1], in[2]);
      float want[3] = {in[0], in[1], in[2]};
      want[knob] = land(x, LO[knob], HI[knob], DEF[knob]);
      char what[128];
      snprintf(what, sizeof what, "D: knob %d at %g lands at %g", knob, (double)x, (double)want[knob]);
      ok(b.s.atom.ceiling_db == want[0] && b.s.state.d == omx_limiter_lookahead_frames(want[1], g_sr) &&
             b.s.atom.release_ms == want[2],
         what);
      loud(l, r, BLK, (uint32_t)(h * 3 + knob) * BLK);
      omx_limiter_instance_run(&b.s, l, r, l, r, BLK);
      const float c = omx_db_to_lin(b.s.atom.ceiling_db);
      ok(all_finite(l, BLK) && all_finite(r, BLK) && omx_block_absmax(l, BLK) <= c &&
             omx_block_absmax(r, BLK) <= c,
         "D: hostile knobs leave the output finite and inside the ceiling");
    }
  }
  box_free(&b);
  drain_violations("D: no contract broken");
}

static void arm_reengage_and_move(void) {
  Box b, fresh;
  box_new(&b);
  box_new(&fresh);
  float l[BLK], r[BLK], fl[BLK], fr[BLK];
  for (int k = 0; k < 5; k++) { /* pull the gain down and fill the rings */
    loud(l, r, BLK, (uint32_t)k * BLK);
    omx_limiter_instance_resolve(&b.s, 0, -9.0f, 3.0f, 400.0f);
    omx_limiter_instance_run(&b.s, l, r, l, r, BLK);
  }
  omx_limiter_instance_resolve(&b.s, 1, -9.0f, 3.0f, 400.0f);
  int same = 1;
  for (int k = 0; k < NBLK; k++) {
    loud(l, r, BLK, (uint32_t)(k + 40) * BLK);
    memcpy(fl, l, sizeof l); memcpy(fr, r, sizeof r);
    omx_limiter_instance_resolve(&b.s, 0, -9.0f, 3.0f, 400.0f);
    omx_limiter_instance_resolve(&fresh.s, 0, -9.0f, 3.0f, 400.0f);
    omx_limiter_instance_run(&b.s, l, r, l, r, BLK);
    omx_limiter_instance_run(&fresh.s, fl, fr, fl, fr, BLK);
    same &= same_bytes(l, fl, BLK) && same_bytes(r, fr, BLK);
  }
  ok(same, "E: a re-engaged limiter is a fresh one, its state re-armed");
  const uint32_t before = omx_limiter_instance_latency(&b.s);
  omx_limiter_instance_resolve(&b.s, 0, -9.0f, 5.0f, 400.0f);
  const uint32_t after = omx_limiter_instance_latency(&b.s);
  ok(after == omx_truepeak_delay() + omx_limiter_lookahead_frames(5.0f, g_sr) && after > before,
     "E: a look-ahead move re-arms the state at the new latency");
  box_free(&b);
  box_free(&fresh);
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  Box a, b, c, alone;
  box_new(&a);
  box_new(&b);
  box_new(&c);
  box_new(&alone);
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    loud(l, r, BLK, (uint32_t)k * BLK);
    omx_limiter_instance_resolve(&a.s, 0, -4.0f, 1.0f, 30.0f);
    omx_limiter_instance_resolve(&b.s, 0, -4.0f, 1.0f, 30.0f);
    omx_limiter_instance_resolve(&alone.s, 0, -4.0f, 1.0f, 30.0f);
    omx_limiter_instance_resolve(&c.s, 0, -11.0f, 4.0f, 700.0f);
    omx_limiter_instance_run(&a.s, l, r, al, ar, BLK);
    loud(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_limiter_instance_run(&c.s, cl, cr, cl, cr, BLK); /* a different one between */
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_limiter_instance_run(&b.s, bl, br, bl, br, BLK);
    omx_limiter_instance_run(&alone.s, l, r, xl, xr, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
    indep &= same_bytes(al, xl, BLK) && same_bytes(ar, xr, BLK);
  }
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
  box_free(&a); box_free(&b); box_free(&c); box_free(&alone);
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
    arm_reengage_and_move();
    arm_alias_and_independence();
  }
  return finish("limiter_instance");
}
