// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The band-dynamics word's golden digests (test/golden/band_dyn.sha256), through the driver in
 * golden.h: the standard impulse+noise stimulus, both legs, through a bell-shaped `B` section in
 * ABOVE mode (a dynamic-EQ cut) at every declared rate.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_band_dyn.h>
#include <omxdsp/omx_eq_design.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

static void render(float sr, float *out) {
  float bc[5];
  omx_eq_design_f(OMX_EQ_BANDPASS, 3000.0, 2.0, 0.0, sr, bc);
  struct omx_band_dyn p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.dyn.enabled = 1;
  p.dyn.gc.mode = OMX_DYN_ABOVE;
  p.dyn.gc.thresh_db = -35.0f;
  p.dyn.gc.ratio = 4.0f;
  p.dyn.gc.knee_db = 6.0f;
  p.dyn.gc.range_db = -15.0f;
  p.dyn.gc.makeup_lin = 1.0f;
  p.dyn.detect = OMX_DETECT_PEAK;
  p.dyn.ovs_mode = OMX_DYN_OVS_OFF;
  p.dyn.attack_ms = 5.0f;
  p.dyn.attack_coeff = omx_pole_from_time_ms(5.0f, sr);
  p.dyn.release_coeff = omx_pole_from_time_ms(80.0f, sr);
  memcpy(p.b_c, bc, sizeof p.b_c);
  struct omx_band_dyn_state s;
  omx_band_dyn_state_init(&s);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_band_dyn_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "band_dyn", render); }
