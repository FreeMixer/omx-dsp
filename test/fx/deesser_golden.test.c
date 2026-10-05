// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The de-esser's golden digests (test/golden/deesser.sha256), through the driver in golden.h: an
 * impulse, then integer-generated noise, through the band-dynamics split at a running 7 kHz
 * bandpass for the first half, then the wideband mode for the second, so both application paths
 * are held.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_deesser.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* RBJ band-pass (constant 0 dB peak), the cookbook, normalised to a0 — {b0,b1,b2,a1,a2} with the
 * a-terms SUBTRACTED, which is what omx_biquad expects. The same design the control thread runs
 * (`deEsserBandSection`); see deesser.test.c for the closed-form proof of this shape. */
static void rbj_bandpass(float c[5], double f0, double q, double sr) {
  double w = 2.0 * M_PI * f0 / sr, cw = cos(w), sw = sin(w), alpha = sw / (2.0 * q);
  double a0 = 1.0 + alpha;
  c[0] = (float)(alpha / a0);
  c[1] = 0.0f;
  c[2] = (float)(-alpha / a0);
  c[3] = (float)((-2.0 * cw) / a0);
  c[4] = (float)((1.0 - alpha) / a0);
}
/* core's `bandwidthOctavesQ`, which `deEsserBandQ` is. */
static double bw_q(double f0, double bw_oct, double sr) {
  double w0 = 2.0 * M_PI * f0 / sr;
  return 1.0 / (2.0 * sinh(log(2.0) / 2.0 * bw_oct * w0 / sin(w0)));
}

static void render(float sr, float *out) {
  struct omx_deess_state s;
  omx_deess_state_init(&s);
  struct omx_deess p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.mode = OMX_DEESS_SPLIT;
  p.dyn.enabled = 1;
  p.dyn.gc.mode = OMX_DYN_ABOVE;
  p.dyn.detect = OMX_DETECT_PEAK;
  p.dyn.gc.thresh_db = -30.0f;
  p.dyn.gc.ratio = 4.0f;
  p.dyn.gc.knee_db = 6.0f;
  p.dyn.gc.range_db = -24.0f;
  p.dyn.gc.makeup_lin = 1.0f;
  p.dyn.attack_ms = 1.0f;
  p.dyn.ovs_mode = OMX_DYN_OVS_OFF;
  p.dyn.attack_coeff = omx_pole_from_time_ms(1.0f, sr);
  p.dyn.release_coeff = omx_pole_from_time_ms(60.0f, sr);
  rbj_bandpass(p.bp_c, 7000.0, bw_q(7000.0, 1.0, (double)sr), sr);

  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) {
      float noise = omx_fx_golden_noise(&lcg);
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    if (o >= FRAMES / 2) p.mode = OMX_DEESS_WIDEBAND;
    omx_deess_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "deesser", render); }
