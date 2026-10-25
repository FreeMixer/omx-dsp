// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The vocal doubler's golden digests (golden.h) at the nine oracle rates: the standard stimulus through
 * the doubler face, two voices then four by halves, 14 ms, 9 cents, a 0.7 Hz drift 0.6 ms deep, 80 %
 * spread, 60 % mix, in 128-frame blocks.
 *
 *   make test-fx                                  compare
 *   build/fx_doubler_instance_golden --write      print test/golden/doubler_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_doubler_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxDoublerInstance d;
  omx_doubler_instance_init(&d, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    omx_doubler_instance_resolve(&d, 0, o < OMX_FX_GOLDEN_FRAMES / 2 ? 2.0f : 4.0f, 14.0f, 9.0f, 0.7f, 0.6f, 80.0f, 60.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_doubler_instance_run(&d, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "doubler_instance", render); }
