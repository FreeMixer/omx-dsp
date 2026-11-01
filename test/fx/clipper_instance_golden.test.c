// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The clipper face's golden digests (golden.h) at the nine oracle rates: the standard stimulus through the
 * face, the drive swept from 0 to 24 dB, the ceiling from 0 to -12 dB, a hard clip for the first half and a
 * softness of 60 % for the second, resolved every 128-frame block.
 *
 *   make test-fx                                 compare
 *   build/fx_clipper_instance_golden --write     print test/golden/clipper_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_clipper_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxClipperInstance w;
  omx_clipper_instance_init(&w, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float u = (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    omx_clipper_instance_resolve(&w, 0, 24.0f * u, -12.0f * u, u < 0.5f ? 0.0f : 60.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_clipper_instance_run(&w, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "clipper_instance", render); }
