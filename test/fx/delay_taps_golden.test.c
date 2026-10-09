// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The FX delay's read taps, golden digests (golden.h): the stimulus through a ping-pong delay of
 * 23.5 ms / 31 ms, feedback 0.6, mix 0.5, tone 0.6, with four taps at 1/2, 3/4 and 3/2 of a 23.5 ms
 * base, gains 1, 0.75, 0.5, 0.375 and pans 0, -0.5, +0.5, +1, in 128-frame blocks.
 *
 *   make test-fx                            compare
 *   build/fx_delay_taps_golden --write      print the lines test/golden/delay_taps.sha256 holds
 */
#include <stdint.h>
#include <stdlib.h>

#include <omxdsp/fx/omx_delay.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static const uint32_t F[OMX_FXDELAY_MAX_TAPS][2] = {{1, 1}, {1, 2}, {3, 4}, {3, 2}};
  static const float GAIN[OMX_FXDELAY_MAX_TAPS] = {1.0f, 0.75f, 0.5f, 0.375f};
  static const float PAN[OMX_FXDELAY_MAX_TAPS] = {0.0f, -0.5f, 0.5f, 1.0f};
  float *rl = calloc(OMX_FXDELAY_CAP, sizeof(float)), *rr = calloc(OMX_FXDELAY_CAP, sizeof(float));
  struct omx_fx_delay_state s = {rl, rr, OMX_FXDELAY_CAP, 0u, 0.0f, 0.0f};
  struct omx_fx_delay p = {1, omx_fxdelay_ms_to_samples(23.5f, sr), omx_fxdelay_ms_to_samples(31.0f, sr),
                           0.6f, 0.5f, 0.6f, 1};
  struct omx_fx_delay_taps t = {OMX_FXDELAY_MAX_TAPS, {0}, {0}, {0}};
  for (int k = 0; k < OMX_FXDELAY_MAX_TAPS; k++) {
    t.d[k] = omx_fxdelay_ms_to_samples(omx_fxdelay_tap_ms(23.5f, F[k][0], F[k][1], NULL), sr);
    omx_fxdelay_tap_legs(GAIN[k], PAN[k], &t.gl[k], &t.gr[k]);
  }
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_fx_delay_process_taps(l, r, BLOCK, &p, &t, &s, sr);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
  free(rl);
  free(rr);
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "delay_taps", render); }
