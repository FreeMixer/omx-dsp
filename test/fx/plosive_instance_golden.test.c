// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The plosive tamer's golden digests (golden.h) at the nine oracle rates: the standard stimulus through
 * the plosive face, both halves at their declared defaults but the sensitivities, swept from 0 to 20 dB,
 * resolved every 128-frame block.
 *
 *   make test-fx                                  compare
 *   build/fx_plosive_instance_golden --write      print test/golden/plosive_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_plosive_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxPlosiveInstance p;
  omx_plosive_instance_init(&p, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float s = 20.0f * (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    omx_plosive_instance_resolve(&p, 0, 6.0f, s, 8.0f, 4000.0f, 2.0f, 12.0f, s, 40.0f, 150.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_plosive_instance_run(&p, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "plosive_instance", render); }
