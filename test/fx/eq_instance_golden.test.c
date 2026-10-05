// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The strip EQ instance's golden digests (golden.h), at the default 8 bands: each leg of the
 * stimulus through its own instance with the same words — eight bands of every type, one hostile
 * (+40 dB at 1e6 Hz -> +15 dB at 20 kHz), HPF 60 Hz 24 dB/oct, LPF 15 kHz 12 dB/oct — the EQ
 * switched off for the second quarter, in 128-frame blocks with separate in/out buffers.
 *
 *   make test-fx                             compare
 *   build/fx_eq_instance_golden --write      print test/golden/eq_instance.sha256
 */
#include <stdint.h>

#include <omxdsp/fx/omx_eq_instance.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static const float words[8][5] = {
      {0, 120, 4, 1.4f, 1}, {1, 80, -3, 0.7f, 1}, {2, 8000, 5, 0.7f, 1}, {3, 3000, 0, 30, 1},
      {4, 600, 0, 1, 1},    {5, 1500, 0, 2, 1},    {0, 1e6f, 40, 2, 1},   {0, 400, 0, 1, 1},
  };
  float on = 1.0f, hpf[3] = {1, 60, 24}, lpf[3] = {1, 15000, 12};
  struct omx_eq_lv2_controls c = {&on, &hpf[0], &hpf[1], &hpf[2], &lpf[0], &lpf[1], &lpf[2], {{0}}};
  for (int i = 0; i < 8; i++)
    for (int j = 0; j < 5; j++) c.band[i][j] = &words[i][j];
  struct omx_eq_lv2 el, er;
  omx_eq_lv2_init(&el, sr);
  omx_eq_lv2_init(&er, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    on = (o >= OMX_FX_GOLDEN_FRAMES / 4 && o < OMX_FX_GOLDEN_FRAMES / 2) ? 0.0f : 1.0f;
    omx_eq_lv2_set_controls(&el, &c);
    omx_eq_lv2_set_controls(&er, &c);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_eq_lv2_run(&el, l, ol, BLOCK);
    omx_eq_lv2_run(&er, r, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "eq_instance", render); }
