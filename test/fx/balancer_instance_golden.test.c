// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The balancer face's golden digests (golden.h) at the nine oracle rates: the standard stimulus through the
 * face, an amount of 100 %, a range of 6 dB, 6 dB per second, a slope of -3 dB per octave,
 * resolved every 128-frame block.
 *
 *   make test-fx                                 compare
 *   build/fx_balancer_instance_golden --write     print test/golden/balancer_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_balancer_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxBalancerInstance w;
  omx_balancer_instance_init(&w, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float u = (float)o / (float)OMX_FX_GOLDEN_FRAMES;
    (void)u; omx_balancer_instance_resolve(&w, 0, 100.0f, 6.0f, 6.0f, -3.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_balancer_instance_run(&w, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "balancer_instance", render); }
