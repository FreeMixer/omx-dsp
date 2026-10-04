// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The tremolo's golden digests (test/golden/tremolo.sha256), through the driver in golden.h: an
 * impulse, then integer-generated noise, through the gain stage on a running oscillator at every
 * declared rate — tremolo mode for the first half, pan mode for the second, so both read paths
 * of the kernel are held.
 */
#include <stdint.h>

#include <omxdsp/fx/omx_tremolo.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

static void render(float sr, float *out) {
  struct omx_tremolo_state s;
  omx_tremolo_state_init(&s);
  struct omx_tremolo p = {1, OMX_TREMOLO_MODE_TREMOLO, omx_lfo_inc(1.3f, sr), 0.8f, 0.6f};
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) {
      float noise = omx_fx_golden_noise(&lcg);
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    p.mode = o < FRAMES / 2 ? OMX_TREMOLO_MODE_TREMOLO : OMX_TREMOLO_MODE_PAN;
    omx_tremolo_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "tremolo", render); }
