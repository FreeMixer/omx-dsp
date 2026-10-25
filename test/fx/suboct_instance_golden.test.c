// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The sub-octaver's golden digests (golden.h) at the nine oracle rates: the standard stimulus through the
 * sub-octaver, the three ranges in turn, oct1 80 %, oct2 swept from 0 to 100 %, dry 70 %, the gate at
 * -60 dBFS, resolved every 128-frame block.
 *
 *   make test-fx                                  compare
 *   build/fx_suboct_instance_golden --write       print test/golden/suboct_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_suboct_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxSuboctInstance s;
  omx_suboct_instance_init(&s, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float u = (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    omx_suboct_instance_resolve(&s, 0, 3 * o / OMX_FX_GOLDEN_FRAMES, 80.0f, 100.0f * u, 70.0f, -60.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_suboct_instance_run(&s, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "suboct_instance", render); }
