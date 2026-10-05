// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The transient designer instance's golden digests (golden.h): the stimulus through the instance
 * with hostile port words (attack +40 dB -> +24, sustain NaN -> 0, attack time 10 ms, sustain time
 * 250 ms, output +1.5 dB), bypassed for the second quarter and re-engaged, in 128-frame blocks with
 * separate in/out buffers.
 *
 *   make test-fx                                   compare
 *   build/fx_transient_instance_golden --write     print test/golden/transient_instance.sha256
 */
#include <math.h>
#include <stdint.h>

#include <omxdsp/fx/omx_transient_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  OmxTransientInstance s;
  omx_transient_instance_init(&s, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const int bypass = o >= OMX_FX_GOLDEN_FRAMES / 4 && o < OMX_FX_GOLDEN_FRAMES / 2;
    omx_transient_instance_resolve(&s, bypass, 40.0f, NAN, 10.0f, 250.0f, 1.5f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_transient_instance_run(&s, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "transient_instance", render); }
