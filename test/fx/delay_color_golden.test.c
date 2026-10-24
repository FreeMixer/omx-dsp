// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The delay colour's golden digests (golden.h) at the nine oracle rates: the standard stimulus through
 * the delay face, a quarter at 132 BPM, feedback 0.55, ping-pong, the mode moving digital, tape, analog,
 * tape by quarters, the wet ducked by 12 dB above -30 dBFS, in 128-frame blocks.
 *
 *   make test-fx                             compare
 *   build/fx_delay_color_golden --write      print test/golden/delay_color.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_delay_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxDelayInstance d;
  static const int MODE[4] = {0, 1, 2, 1};
  omx_delay_instance_init(&d, sr);
  omx_delay_instance_set_tempo(&d, 132.0f);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    omx_delay_instance_resolve(&d, 0, 300.0f, 0.55f, 0.2f, 0.5f, 1, (int)OMX_DELAY_SYNCS_TEMPO,
                               (int)OMX_DELAY_DIVISIONS_QUARTER, MODE[4 * o / OMX_FX_GOLDEN_FRAMES], -12.0f, -30.0f,
                               200.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_delay_instance_run(&d, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "delay_color", render); }
