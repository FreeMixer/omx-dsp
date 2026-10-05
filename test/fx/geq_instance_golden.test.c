// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The graphic EQ instance's golden digests (golden.h): the stimulus through the instance with a
 * fixed curve of 31 band words (some hostile: +40 dB -> +15, NaN -> 0), bypassed for the second
 * quarter and re-engaged, in 128-frame blocks with separate in/out buffers.
 *
 *   make test-fx                              compare
 *   build/fx_geq_instance_golden --write      print test/golden/geq_instance.sha256
 */
#include <math.h>
#include <stdint.h>

#include <omxdsp/fx/omx_geq_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  float g[OMX_GEQ_BANDS];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) g[k] = (float)((k * 5) % 25 - 12);
  g[3] = 40.0f;
  g[11] = NAN;
  OmxGeqInstance s;
  omx_geq_instance_init(&s, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const int bypass = o >= OMX_FX_GOLDEN_FRAMES / 4 && o < OMX_FX_GOLDEN_FRAMES / 2;
    omx_geq_instance_resolve(&s, bypass, g);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_geq_instance_run(&s, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "geq_instance", render); }
