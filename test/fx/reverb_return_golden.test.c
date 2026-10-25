// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The reverb return's golden digests (golden.h) at the nine oracle rates: the standard stimulus through
 * the reverb face, a room then a plate by halves, early reflections at -4 dB and 70 % size, the tail at
 * -2 dB, the wet ducked by 9 dB above -32 dBFS and de-essed at 6 kHz, in 128-frame blocks.
 *
 *   make test-fx                               compare
 *   build/fx_reverb_return_golden --write      print test/golden/reverb_return.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_reverb_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxReverbInstance v;
  omx_reverb_instance_init(&v, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const int algo = o < OMX_FX_GOLDEN_FRAMES / 2 ? OMX_REVERB_ROOM : OMX_REVERB_PLATE;
    omx_reverb_instance_resolve(&v, 0, 100.0f, 0.45f, 0.55f, 0.35f, 0.9f, 8.0f, 80.0f, 16000.0f, 300.0f, 100.0f,
                                200.0f, -40.0f, algo, -4.0f, -2.0f, 70.0f, -9.0f, -32.0f, 180.0f,
                                (int)OMX_REVERB_WET_DEESSES_ON, 6000.0f, -36.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_reverb_instance_run(&v, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "reverb_return", render); }
