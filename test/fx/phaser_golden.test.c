// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The phaser's golden digests (test/golden/phaser.sha256), through the driver in golden.h: an
 * impulse, then integer-generated noise, through the swept all-pass chain on a running oscillator
 * at every declared rate — six sections at feedback +0.6 for the first half, four at -0.5 for the
 * second, so the stage-count change and both signs of the loop are held.
 */
#include <stdint.h>
#include <stdlib.h>

#include <omxdsp/fx/omx_phaser.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

static void render(float sr, float *out) {
  static struct omx_phaser_state s;
  omx_phaser_state_init(&s);
  struct omx_phaser p = {1, 6, 300.0f, 4.0f, omx_lfo_inc(0.5f, sr), 0.6f, 0.5f};
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) {
      float noise = omx_fx_golden_noise(&lcg);
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    if (o >= FRAMES / 2) { p.stages = 4; p.feedback = -0.5f; }
    omx_phaser_process(l, r, BLOCK, &p, &s, sr);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "phaser", render); }
