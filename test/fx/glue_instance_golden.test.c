// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The glue face's golden digests (golden.h) at the nine oracle rates: the standard stimulus through the
 * face, the threshold swept from -40 to -6 dB, each ratio a third of the run, the attack and
 * release detents stepping, a corner of 120 Hz, resolved every 128-frame block.
 *
 *   make test-fx                                 compare
 *   build/fx_glue_instance_golden --write     print test/golden/glue_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_glue_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxGlueInstance w;
  omx_glue_instance_init(&w, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float u = (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    omx_glue_instance_resolve(&w, 0, -40.0f + 34.0f * u, 3 * o / OMX_FX_GOLDEN_FRAMES, (o / BLOCK / 64) % 6, (o / BLOCK / 64) % 5, 2.0f, 120.0f, 100.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_glue_instance_run(&w, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "glue_instance", render); }
