// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The chorus's golden digests (test/golden/chorus.sha256), through the driver in golden.h: an
 * impulse, then integer-generated noise, through three voices on a running oscillator at every
 * declared rate — both legs one phase for the first half, the right leg a quarter turn ahead
 * (spread 0.25) for the second, so both read paths of the kernel are held.
 */
#include <stdint.h>
#include <stdlib.h>

#include <omxdsp/fx/omx_chorus.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

static void render(float sr, float *out) {
  float *rl = calloc(OMX_CHORUS_CAP, sizeof(float)), *rr = calloc(OMX_CHORUS_CAP, sizeof(float));
  struct omx_chorus_state s;
  memset(&s, 0, sizeof s);
  (void)omx_chorus_state_init(&s, rl, rr, OMX_CHORUS_CAP);
  struct omx_chorus p = {1, 3, OMX_CHORUS_BASE_MS * sr / 1000.0f, 4.0f * sr / 1000.0f,
                         omx_lfo_inc(0.8f, sr), 0.5f, 0.0f};
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) {
      float noise = omx_fx_golden_noise(&lcg);
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    p.spread = o < FRAMES / 2 ? 0.0f : 0.25f;
    omx_chorus_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
  free(rl);
  free(rr);
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "chorus", render); }
