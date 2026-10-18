// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The multiband compressor's golden digests (test/golden/mbcomp.sha256), through the driver in
 * golden.h: the standard impulse+noise stimulus, both legs, through a 4-band stage at its come-up
 * corners, every band compressing (RMS, -30 dB, 4:1, 6 dB knee), the third band off and the stage's
 * detector at 4x, at the nine oracle rates.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_mbcomp.h>
#include <omxdsp/omx_onepole.h>

#define OMX_FX_GOLDEN_ORACLE_RATES
#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128
#define BANDS 4u

static void render(float sr, float *out) {
  static struct omx_mbcomp m;
  memset(&m, 0, sizeof m);
  m.enabled = 1;
  m.ovs_mode = OMX_DYN_OVS_X4;
  omx_mbcomp_design(&m, BANDS, omx_mbcomp_come_up_hz(BANDS), sr);
  for (uint32_t k = 0; k < BANDS; k++) {
    struct omx_dyn *d = &m.band[k];
    d->enabled = k != 2u;
    d->gc.mode = OMX_DYN_ABOVE;
    d->gc.thresh_db = -30.0f;
    d->gc.ratio = 4.0f;
    d->gc.knee_db = 6.0f;
    d->gc.makeup_lin = 1.0f;
    d->detect = OMX_DETECT_RMS;
    d->attack_ms = 5.0f;
    d->attack_coeff = omx_pole_from_time_ms(5.0f, sr);
    d->release_coeff = omx_pole_from_time_ms(80.0f, sr);
  }
  static struct omx_mbcomp_state s;
  omx_mbcomp_state_init(&s, omx_mbcomp_factor(&m));
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_mbcomp_process(l, r, BLOCK, &m, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "mbcomp", render); }
