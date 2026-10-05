// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * reverb_instance.test.c — the reverb instance core (omx_reverb_instance.h), every arm at every
 * rate in OMX_DECLARED_RATES:
 *   A  a refused init (an undeclared rate, no pool, a pool the layout does not fit) is the identity;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_reverb_process on the atom the
 *      console's resolve_fx_reverb loads from the same ports, all five configurations;
 *   D  every port, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default) and the output is finite;
 *   E  a bypass->engaged edge clears the pool: silence in is silence out;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves them each the instance alone (no shared state).
 *   make test-fx
 */
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_reverb_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 8
#define NPORTS 13

static float *pool(void) { return calloc(OMX_REVERB_POOL_FLOATS, sizeof(float)); }

static struct omx_reverb_instance_ports defaults(void) {
  const struct omx_reverb_instance_ports d = OMX_REVERB_INSTANCE_PORT_DEFAULTS;
  return d;
}

/* Each port's declared travel, in port order, for arm D. */
static const float LO[NPORTS] = {OMX_REVERB_ROOM, OMX_REVERB_SIZE_RANGE_MIN, OMX_REVERB_DAMPING_RANGE_MIN,
                                 OMX_REVERB_PREDELAY_RANGE_MIN, OMX_REVERB_WIDTH_RANGE_MIN,
                                 OMX_REVERB_MIX_RANGE_MIN, OMX_REVERB_LOWCUT_RANGE_MIN,
                                 OMX_REVERB_HIGHCUT_RANGE_MIN, OMX_REVERB_REVERSE_RANGE_MIN,
                                 OMX_REVERB_HOLD_RANGE_MIN, OMX_REVERB_RELEASE_RANGE_MIN,
                                 OMX_REVERB_GATE_THRESHOLD_RANGE_MIN, OMX_REVERB_PLATE_MOD_DEPTH_RANGE_MIN};
static const float HI[NPORTS] = {OMX_REVERB_GATED, OMX_REVERB_SIZE_RANGE_MAX, OMX_REVERB_DAMPING_RANGE_MAX,
                                 OMX_REVERB_PREDELAY_RANGE_MAX, OMX_REVERB_WIDTH_RANGE_MAX,
                                 OMX_REVERB_MIX_RANGE_MAX, OMX_REVERB_LOWCUT_RANGE_MAX,
                                 OMX_REVERB_HIGHCUT_RANGE_MAX, OMX_REVERB_REVERSE_RANGE_MAX,
                                 OMX_REVERB_HOLD_RANGE_MAX, OMX_REVERB_RELEASE_RANGE_MAX,
                                 OMX_REVERB_GATE_THRESHOLD_RANGE_MAX, OMX_REVERB_PLATE_MOD_DEPTH_RANGE_MAX};

static float atom_port(const struct omx_reverb *o, int port) {
  switch (port) {
  case 0: return (float)o->algorithm;
  case 1: return o->size;
  case 2: return o->damping;
  case 3: return o->predelay_ms;
  case 4: return o->width;
  case 5: return o->mix;
  case 6: return o->lowcut;
  case 7: return o->highcut;
  case 8: return o->reverse_ms;
  case 9: return o->hold_ms;
  case 10: return o->release_ms;
  case 11: return o->gate_threshold_db;
  default: return o->plate_mod_depth;
  }
}

/* The console's resolve_fx_reverb: every port loaded as it is, for arm C. */
static struct omx_reverb console_atom(const struct omx_reverb_instance_ports *p) {
  struct omx_reverb o;
  memset(&o, 0, sizeof(o));
  o.enabled = 1;
  o.algorithm = (int)p->algorithm;
  o.size = p->size; o.damping = p->damping; o.predelay_ms = p->predelay_ms; o.width = p->width;
  o.mix = p->mix; o.lowcut = p->lowcut; o.highcut = p->highcut; o.reverse_ms = p->reverse_ms;
  o.hold_ms = p->hold_ms; o.release_ms = p->release_ms; o.gate_threshold_db = p->gate_threshold_db;
  o.plate_mod_depth = p->plate_mod_depth;
  return o;
}

static void arm_refused(void) {
  float *pl = pool();
  OmxReverbInstance s;
  ok(omx_reverb_instance_init(&s, 50000.0f, pl, OMX_REVERB_POOL_FLOATS) == 0, "A: an undeclared rate refused");
  ok(omx_reverb_instance_init(&s, NAN, pl, OMX_REVERB_POOL_FLOATS) == 0, "A: a NaN rate refused");
  ok(omx_reverb_instance_init(&s, g_sr, NULL, OMX_REVERB_POOL_FLOATS) == 0, "A: no pool refused");
  ok(omx_reverb_instance_init(&s, g_sr, pl, 4096u) == 0, "A: a pool the layout does not fit refused");
  struct omx_reverb_instance_ports p = defaults();
  p.mix = 1.0f;
  omx_reverb_instance_resolve(&s, 0, &p);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_reverb_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_reverb_instance_init(&s, g_sr, pl, OMX_REVERB_POOL_FLOATS) == 1, "A: the sized pool is accepted");
  free(pl);
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  float *pl = pool();
  OmxReverbInstance s;
  omx_reverb_instance_init(&s, g_sr, pl, OMX_REVERB_POOL_FLOATS);
  struct omx_reverb_instance_ports p = defaults();
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_reverb_instance_resolve(&s, 1, &p);
    omx_reverb_instance_run(&s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_reverb_instance_run(&s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  free(pl);
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  float *pl = pool(), *kp = pool();
  for (int alg = OMX_REVERB_ROOM; alg <= OMX_REVERB_GATED; alg++) {
    struct omx_reverb_instance_ports p = defaults();
    p.algorithm = (float)alg; p.mix = 0.6f; p.predelay_ms = 12.0f; p.lowcut = 80.0f;
    p.highcut = 9000.0f; p.width = 0.8f; p.size = 0.55f; p.gate_threshold_db = -30.0f;
    OmxReverbInstance s;
    omx_reverb_instance_init(&s, g_sr, pl, OMX_REVERB_POOL_FLOATS);
    struct omx_reverb_state ks;
    memset(&ks, 0, sizeof ks);
    omx_reverb_state_layout(&ks, kp, OMX_REVERB_POOL_FLOATS, g_sr);
    const struct omx_reverb a = console_atom(&p);
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l);
      omx_reverb_instance_resolve(&s, 0, &p);
      omx_reverb_instance_run(&s, l, r, ol, or_, BLK);
      omx_reverb_process(l, r, BLK, &a, &ks, g_sr);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    char what[128];
    snprintf(what, sizeof what,
             "C: algorithm %d: the engaged instance is omx_reverb_process on the console's atom, bit for bit", alg);
    ok(same, what);
  }
  free(pl); free(kp);
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  float *pl = pool();
  OmxReverbInstance s;
  omx_reverb_instance_init(&s, g_sr, pl, OMX_REVERB_POOL_FLOATS);
  const struct omx_reverb_instance_ports d = defaults();
  float l[BLK], r[BLK];
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    const int nonfinite = x - x != 0.0f;
    for (int port = 0; port < NPORTS; port++) {
      struct omx_reverb_instance_ports p = d;
      ((float *)&p)[port] = x;
      omx_reverb_instance_resolve(&s, 0, &p);
      /* The exact landing: non-finite at the declared default, a finite word at its travel's edge. */
      const float want = nonfinite ? ((const float *)&d)[port] : x < LO[port] ? LO[port] : x > HI[port] ? HI[port] : x;
      const float got = atom_port(&s.atom, port);
      char what[128];
      snprintf(what, sizeof what, "D: port %d at %g lands at %g (got %g)", port, (double)x, (double)want, (double)got);
      ok(got == want, what);
      programme(l, r, BLK, (uint32_t)(h * NPORTS + port) * BLK);
      omx_reverb_instance_run(&s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile ports leave the output finite");
    }
  }
  free(pl);
  drain_violations("D: no contract broken");
}

static void arm_reengage_clears(void) {
  float *pl = pool();
  OmxReverbInstance s;
  omx_reverb_instance_init(&s, g_sr, pl, OMX_REVERB_POOL_FLOATS);
  struct omx_reverb_instance_ports p = defaults();
  p.mix = 1.0f; p.size = 1.0f;
  float l[BLK], r[BLK];
  for (int b = 0; b < 4; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    omx_reverb_instance_resolve(&s, 0, &p);
    omx_reverb_instance_run(&s, l, r, l, r, BLK);
  }
  omx_reverb_instance_resolve(&s, 1, &p);
  omx_reverb_instance_resolve(&s, 0, &p);
  memset(l, 0, sizeof l); memset(r, 0, sizeof r);
  omx_reverb_instance_run(&s, l, r, l, r, BLK);
  ok(all_zero(l, BLK) && all_zero(r, BLK), "E: a re-engaged reverb starts silent");
  free(pl);
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  float *pa = pool(), *pb = pool(), *pc = pool();
  OmxReverbInstance a, b, c;
  omx_reverb_instance_init(&a, g_sr, pa, OMX_REVERB_POOL_FLOATS);
  omx_reverb_instance_init(&b, g_sr, pb, OMX_REVERB_POOL_FLOATS);
  omx_reverb_instance_init(&c, g_sr, pc, OMX_REVERB_POOL_FLOATS);
  struct omx_reverb_instance_ports p = defaults(), q = defaults();
  p.algorithm = (float)OMX_REVERB_PLATE; p.mix = 0.5f;
  q.algorithm = (float)OMX_REVERB_HALL; q.mix = 0.9f; q.size = 0.9f;
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK];
  int alias = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_reverb_instance_resolve(&a, 0, &p);
    omx_reverb_instance_resolve(&b, 0, &p);
    omx_reverb_instance_resolve(&c, 0, &q); /* a different one between */
    omx_reverb_instance_run(&a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_reverb_instance_run(&c, cl, cr, cl, cr, BLK);
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_reverb_instance_run(&b, bl, br, bl, br, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
  }
  ok(alias, "F/G: in place, with another instance between, is the out-of-place answer");
  free(pa); free(pb); free(pc);
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
  return finish("reverb_instance");
}
