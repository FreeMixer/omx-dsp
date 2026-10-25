// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The tone face's golden digests (golden.h) at the nine oracle rates: the standard stimulus through the
 * tone face, the tilt swept from -6 to +6 dB about a 700 Hz pivot and the DJ filter from -90 to +90 at a
 * resonance of 1.5, resolved every 128-frame block.
 *
 *   make test-fx                                  compare
 *   build/fx_tone_instance_golden --write         print test/golden/tone_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_tone_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxToneInstance t;
  omx_tone_instance_init(&t, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float u = (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    omx_tone_instance_resolve(&t, 0, -6.0f + 12.0f * u, 700.0f, -90.0f + 180.0f * u, 1.5f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_tone_instance_run(&t, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "tone_instance", render); }
