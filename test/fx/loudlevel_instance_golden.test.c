// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The loudlevel face's golden digests (golden.h) at the nine oracle rates: the standard stimulus through the
 * face, a target of -20 LUFS, a range of 12 dB, 6 dB per second, the momentary window,
 * resolved every 128-frame block.
 *
 *   make test-fx                                 compare
 *   build/fx_loudlevel_instance_golden --write     print test/golden/loudlevel_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_loudlevel_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxLoudlevelInstance w;
  omx_loudlevel_instance_init(&w, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float u = (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    (void)u; omx_loudlevel_instance_resolve(&w, 0, -20.0f, 12.0f, 6.0f, -60.0f, 0);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_loudlevel_instance_run(&w, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "loudlevel_instance", render); }
