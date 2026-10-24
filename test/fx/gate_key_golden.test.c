// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The gate's key tilt and listen golden digests (golden.h) at the nine oracle rates: the standard
 * stimulus through the gate face, keyed by itself, the key filter at 120 Hz and 8 kHz, the tilt moving
 * 0, +4, -3 and +6 dB per octave by quarters and the listen on in the last eighth, in 128-frame blocks.
 *
 *   make test-fx                          compare
 *   build/fx_gate_key_golden --write      print test/golden/gate_key.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_gate_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxGateInstance g;
  static const float TILT[4] = {0.0f, 4.0f, -3.0f, 6.0f};
  omx_gate_instance_init(&g, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const int listen = o >= 7 * OMX_FX_GOLDEN_FRAMES / 8;
    omx_gate_instance_resolve(&g, 0, 0, -36.0f, -50.0f, -40.0f, -32.0f, 1.0f, 20.0f, 80.0f, 3.0f, 8.0f, 120.0f, 8000.0f,
                              listen, TILT[4 * o / OMX_FX_GOLDEN_FRAMES]);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_gate_instance_run(&g, NULL, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "gate_key", render); }
