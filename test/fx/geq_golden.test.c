// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The graphic EQ's golden digests: one SHA-256 per declared rate over the kernel's output for a
 * fixed stimulus and a fixed fader set, compared with test/golden/geq.sha256. A patch release may
 * not move one; a release that does is a minor and names the kernel.
 *
 *   make test-fx                    compare
 *   build/fx_geq_golden --write     print the lines test/golden/geq.sha256 holds
 *
 * The stimulus is an impulse, then integer-generated noise, so no libm call shapes the input. The
 * sections are designed by omx_eq_design at the ISO third-octave centres and Q; every third band
 * sits at exactly 0 dB, so the skip-and-prime path runs, and the fader set changes halfway, so an
 * engage edge on a skipped section is in the digest.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <omxdsp/omx_eq_design.h>
#include <omxdsp/fx/omx_geq.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

static void design(struct omx_geq *p, int second, double sr) {
  double c[OMX_GEQ_BANDS][5];
  float g[OMX_GEQ_BANDS];
  const double q = pow(2.0, 1.0 / 6.0) / (pow(2.0, 1.0 / 3.0) - 1.0);
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    g[k] = (k % 3 == (second ? 1 : 0)) ? 0.0f : 0.5f * (float)((k * 7 + (second ? 11 : 3)) % 61) - 15.0f;
    omx_eq_design(OMX_EQ_PEAKING, 1000.0 * pow(2.0, (double)(k - 17) / 3.0), q, g[k], sr, c[k]);
  }
  omx_geq_set(p, 1, (const double(*)[5])c, g);
}

static void render(float sr, float *out) {
  static struct omx_geq p;
  static struct omx_geq_state s;
  omx_geq_state_init(&s);
  design(&p, 0, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    if (o == FRAMES / 2) design(&p, 1, sr);
    for (int i = 0; i < BLOCK; i++) {
      float noise = omx_fx_golden_noise(&lcg);
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    omx_geq_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "geq", render); }
