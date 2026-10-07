// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The rotor word's golden digests (golden.h), at the nine RME rates: the stimulus through one
 * rotor on two order-3 lines, resolved every 128 frames. It starts settled at the horn's slow
 * speed and spins up toward the fast one for the first half, then the drum's constants take over
 * and it slows toward a stop, so the Doppler read, the amplitude swing, the spin-up and the
 * spin-down are all held.
 *
 *   make test-fx                     compare
 *   build/fx_rotor_golden --write    print the lines test/golden/rotor.sha256 holds
 */
#define OMX_FX_GOLDEN_RME 1 /* held at the nine RME rates */

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_rotor.h>

#include "golden.h"

#define FRAMES OMX_FX_GOLDEN_FRAMES
#define BLOCK 128

#define HORN_SLOW 0.8f
#define HORN_FAST 6.8f

static float g_rl[OMX_ROTARY_RING_FLOATS], g_rr[OMX_ROTARY_RING_FLOATS];

static void render(float sr, float *out) {
  struct omx_fdelay ll, lr;
  memset(g_rl, 0, sizeof g_rl);
  memset(g_rr, 0, sizeof g_rr);
  omx_fdelay_init(&ll, g_rl, OMX_ROTARY_RING_FLOATS, OMX_ROTOR_ORDER);
  omx_fdelay_init(&lr, g_rr, OMX_ROTARY_RING_FLOATS, OMX_ROTOR_ORDER);
  struct omx_rotor_state s;
  memset(&s, 0, sizeof s);
  omx_rotor_settle(&s, HORN_SLOW, 0.0f);
  struct omx_rotor p;
  uint32_t lcg = 0x1234567u;
  for (int o = 0; o < FRAMES; o += BLOCK) {
    if (o < FRAMES / 2)
      omx_rotor_resolve(&p, &s, HORN_FAST, OMX_ROTARY_HORN_ACCEL_MS, OMX_ROTARY_HORN_DECEL_MS, 1.0f,
                        OMX_ROTARY_HORN_DOPPLER_MS, OMX_ROTARY_HORN_AM, sr);
    else
      omx_rotor_resolve(&p, &s, 0.0f, OMX_ROTARY_DRUM_ACCEL_MS, OMX_ROTARY_DRUM_DECEL_MS, 0.25f,
                        OMX_ROTARY_DRUM_DOPPLER_MS, OMX_ROTARY_DRUM_AM, sr);
    for (int i = 0; i < BLOCK; i++) {
      float xl, xr, yl = 0.0f, yr = 0.0f;
      omx_fx_golden_stimulus(o + i, &lcg, &xl, &xr);
      omx_rotor_tick(&p, &s, &ll, &lr, xl, xr, &yl, &yr);
      out[2 * (o + i)] = yl;
      out[2 * (o + i) + 1] = yr;
    }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "rotor", render); }
