// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The multiband instance's golden digests (golden.h) at the nine oracle rates: the standard stimulus
 * through four bands at 120, 900 and 5000 Hz, each band compressing differently, band 3 bypassed, the
 * mix at 70 % for the second quarter, band 2 soloed for the third, +3 dB out, in 128-frame blocks.
 *
 *   make test-fx                                 compare
 *   build/fx_mbcomp_instance_golden --write      print test/golden/mbcomp_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_mbcomp_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static const float hz[OMX_MBC_MAX_XOVERS] = {120, 900, 5000, 12000};
  static const float thr[OMX_MBC_MAX_BANDS] = {-30, -36, -24, -40, -20};
  static const float ratio[OMX_MBC_MAX_BANDS] = {3, 6, 2, 4, 2};
  static const float att[OMX_MBC_MAX_BANDS] = {20, 5, 1, 10, 10};
  static const float rel[OMX_MBC_MAX_BANDS] = {300, 120, 60, 150, 150};
  static const float mk[OMX_MBC_MAX_BANDS] = {2, 4, 0, 1, 0};
  static const int by[OMX_MBC_MAX_BANDS] = {0, 0, 0, 1, 0};
  static OmxMbcompInstance m;
  omx_mbcomp_instance_init(&m, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const int q = 4 * o / OMX_FX_GOLDEN_FRAMES;
    const int solo[OMX_MBC_MAX_BANDS] = {0, 0, q == 2, 0, 0};
    omx_mbcomp_instance_resolve(&m, 0, 4.0f, hz, thr, ratio, att, rel, mk, solo, by, q == 1 ? 70.0f : 100.0f, 3.0f);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_mbcomp_instance_run(&m, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "mbcomp_instance", render); }
