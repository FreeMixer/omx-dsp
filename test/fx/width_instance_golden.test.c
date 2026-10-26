// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The width face's golden digests (golden.h) at the nine oracle rates: the standard stimulus through the
 * width face, the width swept from 0 to 200 %, a third of the run plain, a third with the comb at 9 ms and
 * a third with Haas at 14 ms, bass mono at 150 Hz throughout, resolved every 128-frame block.
 *
 *   make test-fx                                  compare
 *   build/fx_width_instance_golden --write        print test/golden/width_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_width_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxWidthInstance w;
  omx_width_instance_init(&w, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float u = (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    const int third = 3 * o / OMX_FX_GOLDEN_FRAMES;
    omx_width_instance_resolve(&w, 0, 200.0f * u, third, third == 2 ? 14.0f : 9.0f, (int)OMX_WIDTH_BASS_MONOS_ON, 150.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_width_instance_run(&w, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "width_instance", render); }
