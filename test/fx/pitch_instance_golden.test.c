// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The pitch face's golden digests (golden.h): the stimulus through omx_pitch_instance_run, block by
 * block, with separate in/out buffers, the face resolved from the host's control words each block.
 * The words walk the defaults (0 st, 0 ct, 100 %), a shift at 100 % and at 50 %, the mix crossing
 * 100 % both ways and a bypass span with its re-engage, so the face's whole audio path is in the
 * digest and a change to anything but its audio leaves it where it is.
 *
 *   make test-fx                                compare
 *   build/fx_pitch_instance_golden --write      print the lines test/golden/pitch_instance.sha256 holds
 */
#include <stdint.h>

#include <omxdsp/fx/omx_pitch_instance.h>

#include "golden.h"

#define BLOCK 128

struct words {
  int bypass;
  float semitones, cents, mix;
};

/* The control words for block `b` of OMX_FX_GOLDEN_FRAMES / BLOCK. */
static struct words words_at(int b) {
  const int nb = OMX_FX_GOLDEN_FRAMES / BLOCK;
  if (b < nb / 4) return (struct words){0, 0.0f, 0.0f, 100.0f};
  if (b < 3 * nb / 8) return (struct words){0, -5.0f, 12.0f, 100.0f};
  if (b < nb / 2) return (struct words){0, -5.0f, 12.0f, 50.0f};
  if (b < 9 * nb / 16) return (struct words){1, -5.0f, 12.0f, 50.0f};
  if (b < 3 * nb / 4) return (struct words){0, 7.0f, 0.0f, 50.0f};
  return (struct words){0, 7.0f, 0.0f, 100.0f};
}

static void render(float sr, float *out) {
  static OmxPitchInstance s;
  omx_pitch_instance_init(&s, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const struct words w = words_at(o / BLOCK);
    omx_pitch_instance_resolve(&s, w.bypass, w.semitones, w.cents, w.mix);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_pitch_instance_run(&s, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "pitch_instance", render); }
