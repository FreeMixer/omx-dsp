// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The transient designer's golden digests (golden.h): the stimulus through attack +9 dB,
 * sustain -6 dB, attack time 10 ms, sustain time 250 ms, output +1.5 dB, in 128-frame blocks.
 *
 *   make test-fx                          compare
 *   build/fx_transient_golden --write     print the lines test/golden/transient.sha256 holds
 */
#include <stdint.h>

#include <omxdsp/fx/omx_transient.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  struct omx_transient p;
  omx_transient_resolve(&p, 0, 9.0f, -6.0f, 10.0f, 250.0f, 1.5f, sr);
  struct omx_transient_state st;
  omx_transient_state_init(&st);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_transient_process(l, r, BLOCK, &p, &st);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "transient", render); }
