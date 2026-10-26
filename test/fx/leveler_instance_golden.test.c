// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The one-knob leveler's golden digests (golden.h) at the nine oracle rates: the standard stimulus
 * through the leveler face, both directions at the fast speed, the amount swept from 10 to 100 %,
 * resolved every 128-frame block.
 *
 *   make test-fx                                  compare
 *   build/fx_leveler_instance_golden --write      print test/golden/leveler_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_leveler_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxLevelerInstance v;
  omx_leveler_instance_init(&v, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float u = (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    omx_leveler_instance_resolve(&v, 0, 10.0f + 90.0f * u, (int)OMX_LEVELER_DIRECTIONS_BOTH, (int)OMX_LEVELER_SPEEDS_FAST);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_leveler_instance_run(&v, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "leveler_instance", render); }
