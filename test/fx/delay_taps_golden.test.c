// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The FX delay's read taps, golden digests (golden.h), at the nine oracle rates: the stimulus through a
 * four-tap ping-pong delay, the tap-delay spec's `tripletCascade` pattern on a 20 ms base (factors
 * 1, 1/3, 2/3, 4/3, gains 1, 0.6, 0.45, 0.3, pans 0, -0.6, +0.6, 0), legs 20 ms / 17.5 ms, feedback
 * 0.25, mix 0.5, tone 0.4, in 128-frame blocks. The taps are resolved the way a caller resolves them:
 * omx_fxdelay_tap_ms, omx_fxdelay_ms_to_samples, omx_fxdelay_tap_legs.
 *
 *   make test-fx                            compare
 *   build/fx_delay_taps_golden --write      print the lines test/golden/delay_taps.sha256 holds
 */
#define OMX_FX_GOLDEN_ORACLE_RATES 1

#include <stdint.h>
#include <stdlib.h>

#include <omxdsp/fx/omx_delay.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  static const uint32_t NUM[4] = { 1u, 1u, 2u, 4u }, DEN[4] = { 1u, 3u, 3u, 3u };
  static const float GAIN[4] = { 1.0f, 0.6f, 0.45f, 0.3f }, PAN[4] = { 0.0f, -0.6f, 0.6f, 0.0f };
  const float base = 20.0f; /* short enough that the repeats fall inside 16384 frames at 192 kHz */
  float *rl = calloc(OMX_FXDELAY_CAP, sizeof(float)), *rr = calloc(OMX_FXDELAY_CAP, sizeof(float));
  struct omx_fx_delay_state s = {rl, rr, OMX_FXDELAY_CAP, 0u, 0.0f, 0.0f};
  struct omx_fx_delay p = {1, omx_fxdelay_ms_to_samples(base, sr), omx_fxdelay_ms_to_samples(17.5f, sr),
                           0.25f, 0.5f, 0.4f, 1};
  struct omx_fx_delay_taps t = {4u, {0u}, {0.0f}, {0.0f}};
  for (uint32_t k = 0; k < 4u; k++) {
    omx_fxdelay_tap_legs(GAIN[k], PAN[k], &t.gl[k], &t.gr[k]);
    if (k > 0u) t.d[k] = omx_fxdelay_ms_to_samples(omx_fxdelay_tap_ms(base, NUM[k], DEN[k], NULL), sr);
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
