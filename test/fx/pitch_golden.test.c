// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The pitch shifter's golden digests: one SHA-256 per declared rate over the kernel's output for a
 * fixed stimulus and a fixed fader set, compared with test/golden/pitch.sha256. A patch release may
 * not move one; a release that does is a minor and names the kernel.
 *
 *   make test-fx                    compare
 *   build/fx_pitch_golden --write   print the lines test/golden/pitch.sha256 holds
 *
 * The stimulus is an impulse, then integer-generated noise, so no libm call shapes the input. The
 * shifter runs at −5 semitones +12 cents, 60 % wet, and turns to +7 semitones, 100 % wet,
 * halfway, so the direction hand-over is in the digest.
 */
#include <stdint.h>
#include <stdlib.h>

#include <omxdsp/fx/omx_pitch.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

static void render(float sr, float *out) {
  const uint32_t cap = omx_pitch_cap_for(sr);
  float *ring = calloc(2u * cap, sizeof(float));
  if (ring == NULL) abort();
  struct omx_pitch p;
  struct omx_pitch_state s;
  if (omx_pitch_state_init(&s, ring, ring + cap, cap) != OMX_FDELAY_OK) abort();
  omx_pitch_resolve(&p, 1, -5.0f, 12.0f, 60.0f, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    if (o == FRAMES / 2) omx_pitch_resolve(&p, 1, 7.0f, 0.0f, 100.0f, sr);
    for (int i = 0; i < BLOCK; i++) {
      float noise = omx_fx_golden_noise(&lcg);
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    omx_pitch_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
  free(ring);
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "pitch", render); }
