// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The rotary's golden digests (test/golden/rotary.sha256), through the driver in golden.h: an
 * impulse, then integer-generated noise, through the drum/horn rotors on a running split at every
 * declared rate — speed fast and balance toward the horn for the first half, speed slow and
 * balance toward the drum for the second, so both rotors' sweep and both signs of the balance law
 * are held.
 */
#define OMX_FX_GOLDEN_RME 1 /* held at the nine RME rates */

#include <stdint.h>

#include <omxdsp/fx/omx_rotary.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

#define HORN_SLOW 0.8f
#define HORN_FAST 6.8f
#define DRUM_SLOW 0.7f
#define DRUM_FAST 5.9f

static void render(float sr, float *out) {
  static struct omx_rotary_state s;
  omx_rotary_init(&s);
  struct omx_rotary p;
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) {
      float noise = omx_fx_golden_noise(&lcg);
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    const int speed = o < FRAMES / 2 ? OMX_ROTARY_FAST : OMX_ROTARY_SLOW;
    const float balance = o < FRAMES / 2 ? 0.3f : -0.6f;
    omx_rotary_resolve(&p, &s, 1, speed, HORN_SLOW, HORN_FAST, DRUM_SLOW, DRUM_FAST, 1.0f, balance,
                       0.7f, sr);
    omx_rotary_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "rotary", render); }
