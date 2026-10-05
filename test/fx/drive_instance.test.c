// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * drive_instance.test.c — the drive instance core (omx_drive_instance.h, formerly mix_drive_lv2.h),
 * every arm at every rate in OMX_DECLARED_RATES:
 *   A  init resolves the declared defaults: engaged, factor 4, the latency the kernel declares;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_drive_process on the atom the
 *      console's resolve_fx_drive builds from the same knobs and the same designed bank;
 *   D  every port, at every hostile host value, lands inside its declared travel (its clamp's
 *      own law for a non-finite word) and the output is finite;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves them each the instance alone (no shared state).
 *   make test-fx
 */
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_drive_instance.h>

#include "instance_oracle.h"

#define BLK 256u
#define NBLK 12

static struct omx_drive_lv2_ports defaults(void) {
  const struct omx_drive_lv2_ports d = OMX_DRIVE_LV2_PORT_DEFAULTS;
  return d;
}

/* The console's resolve_fx_drive, from the same units and the same designed bank, for arm C. */
static struct omx_drive console_atom(uint32_t rate, const struct omx_drive_lv2_ports *p) {
  struct omx_drive o;
  memset(&o, 0, sizeof(o));
  o.enabled = 1;
  o.curve = (int)p->curve;
  o.band = (int)p->band;
  o.drive_lin = omx_db_to_lin(p->drive_db);
  o.even_w = 0.5f * (p->character + 1.0f);
  o.mix = 0.01f * p->mix_pct;
  o.trim_lin = omx_db_to_lin(p->trim_db);
  o.auto_gain = p->auto_gain > 0.5f;
  o.stereo_link = p->stereo_link > 0.5f;
  o.hf_on = p->hf_rolloff > 0.0f;
  o.os_factor = (int)p->oversample;
  omx_drive_design_bank(&o, p->band_hz, p->hf_rolloff > 0.0f ? p->hf_rolloff : 20000.0f, rate);
  omx_drive_time_constants(&o, (float)rate);
  return o;
}

static void arm_init(void) {
  OmxDriveLv2 c;
  omx_drive_lv2_init(&c, (uint32_t)g_sr);
  ok(c.atom.enabled == 1, "A: an instance a host racks is engaged");
  ok(c.atom.os_factor == (int)OMX_OVS_MAX_FACTOR && c.state.factor == c.atom.os_factor,
     "A: the default factor is the oversampler's largest, the state built for it");
  ok(omx_drive_lv2_latency(&c) == omx_drive_latency(&c.atom), "A: the latency is the kernel's");
  ok(c.atom.mix == 0.01f * OMX_DRIVE_MIX_DEFAULT && c.atom.drive_lin == omx_db_to_lin(OMX_DRIVE_DRIVE_DB_DEFAULT),
     "A: the declared defaults are what init resolved");
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxDriveLv2 c;
  omx_drive_lv2_init(&c, (uint32_t)g_sr);
  struct omx_drive_lv2_ports p = defaults();
  p.bypass = 1.0f;
  p.drive_db = 24.0f;
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    omx_drive_lv2_resolve(&c, &p);
    omx_drive_lv2_run(&c, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_drive_lv2_run(&c, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  ok(omx_drive_lv2_latency(&c) == 0, "B: a bypassed instance declares no latency");
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  struct omx_drive_lv2_ports K[3];
  K[0] = defaults();
  K[1] = defaults();
  K[1].curve = (float)OMX_DRIVE_EXCITER; K[1].drive_db = 30.0f; K[1].character = 0.6f;
  K[1].band = (float)OMX_DRIVE_BAND_TILT; K[1].band_hz = 900.0f; K[1].mix_pct = 70.0f;
  K[1].trim_db = -6.0f; K[1].auto_gain = 0.0f; K[1].hf_rolloff = 12000.0f; K[1].oversample = 2.0f;
  K[2] = defaults();
  K[2].curve = 1.0f; K[2].drive_db = 12.0f; K[2].character = -1.0f; K[2].band = 1.0f;
  K[2].band_hz = 5000.0f; K[2].stereo_link = 0.0f; K[2].hf_rolloff = 16000.0f; K[2].oversample = 1.0f;
  for (int k = 0; k < 3; k++) {
    OmxDriveLv2 c;
    omx_drive_lv2_init(&c, (uint32_t)g_sr);
    const struct omx_drive a = console_atom((uint32_t)g_sr, &K[k]);
    struct omx_drive_state ks;
    omx_drive_state_init(&ks, a.os_factor);
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l);
      omx_drive_lv2_resolve(&c, &K[k]);
      omx_drive_lv2_run(&c, l, r, ol, or_, BLK);
      omx_drive_process(l, r, BLK, &a, &ks);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_drive_process on the console's atom, bit for bit");
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  OmxDriveLv2 c;
  omx_drive_lv2_init(&c, (uint32_t)g_sr);
  float l[BLK], r[BLK];
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    const int nonfinite = x - x != 0.0f;
    const int hi = x > 1.0f;
    for (int port = 1; port < 12; port++) { /* 0 is bypass, a toggle */
      struct omx_drive_lv2_ports p = defaults();
      ((float *)&p)[port] = x;
      omx_drive_lv2_resolve(&c, &p);
      const struct omx_drive *o = &c.atom;
      char what[128];
      snprintf(what, sizeof what, "D: port %d at %g lands inside its travel", port, (double)x);
      int inside = o->mix >= 0.0f && o->mix <= 1.0f && o->even_w >= 0.0f && o->even_w <= 1.0f &&
                   (o->os_factor == 1 || o->os_factor == 2 || o->os_factor == 4) &&
                   c.state.factor == o->os_factor && o->drive_lin >= 1.0f &&
                   o->drive_lin <= omx_db_to_lin(OMX_DRIVE_DRIVE_DB_MAX) &&
                   o->trim_lin >= omx_db_to_lin(OMX_DRIVE_TRIM_DB_MIN) &&
                   o->trim_lin <= omx_db_to_lin(OMX_DRIVE_TRIM_DB_MAX) &&
                   o->curve >= OMX_DRIVE_SOFT && o->curve <= OMX_DRIVE_EXCITER &&
                   o->band >= OMX_DRIVE_BAND_FULL && o->band <= OMX_DRIVE_BAND_TILT;
      /* The exact landings: omx_clampf ports (drive, mix) saturate +Inf at the ceiling and floor NaN;
       * omx_port_int ports (curve, oversample) read every non-finite word as the default. */
      switch (port) {
      case 1: inside &= o->curve == (nonfinite ? OMX_DRIVE_SOFT : hi ? OMX_DRIVE_EXCITER : OMX_DRIVE_SOFT); break;
      case 2: inside &= o->drive_lin == omx_db_to_lin(hi ? OMX_DRIVE_DRIVE_DB_MAX : OMX_DRIVE_DRIVE_DB_MIN); break;
      case 6: inside &= o->mix == 0.01f * (hi ? OMX_DRIVE_MIX_MAX : OMX_DRIVE_MIX_MIN); break;
      case 11: inside &= o->os_factor == (nonfinite || hi ? (int)OMX_OVS_MAX_FACTOR : 1); break;
      default: break;
      }
      ok(inside, what);
      programme(l, r, BLK, (uint32_t)(h * 12 + port) * BLK);
      omx_drive_lv2_run(&c, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile ports leave the output finite");
    }
  }
  drain_violations("D: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxDriveLv2 a, b, c;
  omx_drive_lv2_init(&a, (uint32_t)g_sr);
  omx_drive_lv2_init(&b, (uint32_t)g_sr);
  omx_drive_lv2_init(&c, (uint32_t)g_sr);
  struct omx_drive_lv2_ports p = defaults(), q = defaults();
  p.drive_db = 18.0f; p.character = 0.3f; p.band = 2.0f; p.band_hz = 700.0f; p.mix_pct = 80.0f;
  q.curve = 2.0f; q.drive_db = 33.0f; q.oversample = 2.0f; q.hf_rolloff = 12000.0f;
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK];
  int alias = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_drive_lv2_resolve(&a, &p);
    omx_drive_lv2_resolve(&b, &p);
    omx_drive_lv2_resolve(&c, &q); /* a different one between */
    omx_drive_lv2_run(&a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_drive_lv2_run(&c, cl, cr, cl, cr, BLK);
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_drive_lv2_run(&b, bl, br, bl, br, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
  }
  ok(alias, "F/G: in place, with another instance between, is the out-of-place answer");
  drain_violations("F/G: no contract broken");
}

int main(void) {
  omx_fx_require_rate_floor();
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    arm_init();
    arm_bypass();
    arm_is_the_kernel();
    arm_clamps();
    arm_alias_and_independence();
  }
  return finish("drive_instance");
}
