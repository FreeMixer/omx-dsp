// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The FX delay's golden digests (golden.h): the stimulus through a ping-pong delay of 23.5 ms /
 * 31 ms, feedback 0.6, mix 0.5, tone 0.6, in 128-frame blocks.
 *
 *   make test-fx                      compare
 *   build/fx_delay_golden --write     print the lines test/golden/delay.sha256 holds
 */
#include <stdint.h>
#include <stdlib.h>

#include <omxdsp/fx/omx_delay.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  float *rl = calloc(OMX_FXDELAY_CAP, sizeof(float)), *rr = calloc(OMX_FXDELAY_CAP, sizeof(float));
  struct omx_fx_delay_state s = {rl, rr, OMX_FXDELAY_CAP, 0u, 0.0f, 0.0f};
  struct omx_fx_delay p = {1, omx_fxdelay_ms_to_samples(23.5f, sr), omx_fxdelay_ms_to_samples(31.0f, sr),
                           0.6f, 0.5f, 0.6f, 1};
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_fx_delay_process(l, r, BLOCK, &p, &s, sr);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
  free(rl);
  free(rr);
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "delay", render); }
