// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The compressor's key path's golden digests (golden.h) at the nine oracle rates: the standard
 * stimulus as the programme, a second stream of the stimulus generator as a stereo sidechain, through
 * the key filter (high-pass 120 Hz, low-pass 6 kHz), a +3 dB/oct tilt, a 40 ms hold and the ducker
 * at -15 dB, the fold moving through linked, mid, side and max by quarters, in 128-frame blocks.
 *
 *   make test-fx                          compare
 *   build/fx_comp_key_golden --write      print test/golden/comp_key.sha256
 */
#include <stdint.h>

#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <omxdsp/fx/omx_comp_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static OmxCompInstance c;
  omx_comp_instance_init(&c, sr);
  uint32_t lcg = 0x1234567u, key = 0x89abcdeu;
  float l[BLOCK], r[BLOCK], kl[BLOCK], kr[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const int fold = 4 * o / OMX_FX_GOLDEN_FRAMES;
    omx_comp_instance_resolve(&c, 0, -30.0f, 4.0f, 6.0f, 2.0f, 120.0f, 3.0f, 100.0f, (int)OMX_COMP_KINDS_COMP,
                              OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, (int)OMX_COMP_KEY_SOURCES_SIDECHAIN,
                              120.0f, 6000.0f, 3.0f, (int)OMX_COMP_KEY_LISTENS_OFF, 40.0f, fold, (int)OMX_COMP_DUCKS_ON,
                              -15.0f, (int)OMX_COMP_RELEASE_MODES_FIXED);
    for (int i = 0; i < BLOCK; i++) {
      omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
      const float n = omx_fx_golden_noise(&key);
      kl[i] = (o + i) % 4096 < 1024 ? n : 0.05f * n;
      kr[i] = (o + i) % 3000 < 1500 ? -0.5f * n : 0.3f * n;
    }
    omx_comp_instance_run_keyed(&c, kl, kr, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "comp_key", render); }
