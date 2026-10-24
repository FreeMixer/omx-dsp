// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The dynamic EQ instance's golden digests (golden.h) at the nine oracle rates: the standard
 * stimulus through six bands (a low shelf cutting above its threshold, a bell boosting below its
 * threshold, a 0 dB bell cutting, a static bell with its dynamics off, a band that is off and a high
 * shelf cutting), the mix at 80 % for the third quarter, in 128-frame blocks with separate in/out
 * buffers.
 *
 *   make test-fx                                compare
 *   build/fx_dyneq_instance_golden --write      print test/golden/dyneq_instance.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_dyneq_instance.h>

#include "golden.h"

#define BLOCK 128
#define NB OMX_DYNEQ_INSTANCE_BANDS

static void render(float sr, float *out) {
  static const int on[NB] = {1, 1, 1, 1, 0, 1};
  static const int type[NB] = {1, 0, 0, 0, 0, 2};
  static const float freq[NB] = {150, 700, 3000, 5000, 9000, 8000};
  static const float gain[NB] = {2, -3, 0, 4, 0, -1};
  static const float q[NB] = {0.7f, 1.2f, 3, 2, 1, 0.7f};
  static const int mode[NB] = {1, 2, 1, 0, 1, 1};
  static const float thr[NB] = {-24, -40, -30, -30, -30, -36};
  static const float ratio[NB] = {3, 2, 6, 2, 2, 4};
  static const float range[NB] = {-9, 6, -12, -6, -6, -18};
  static const float att[NB] = {2, 5, 0.5f, 5, 5, 1};
  static const float rel[NB] = {120, 300, 60, 100, 100, 80};
  static const int listen[NB] = {0, 0, 0, 0, 0, 0};
  static OmxDyneqInstance d;
  omx_dyneq_instance_init(&d, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const float mix = (o >= OMX_FX_GOLDEN_FRAMES / 2 && o < 3 * OMX_FX_GOLDEN_FRAMES / 4) ? 80.0f : 100.0f;
    omx_dyneq_instance_resolve(&d, 0, on, type, freq, gain, q, mode, thr, ratio, range, att, rel, listen, mix);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_dyneq_instance_run(&d, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "dyneq_instance", render); }
