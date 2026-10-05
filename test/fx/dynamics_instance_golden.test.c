// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The strip compressor instance's golden digests (golden.h): per host block of 1000 frames, the
 * ports resolved by value and the block run, as a plugin's `run` does through
 * fx/omx_dynamics_instance.h; four renders back to back per rate:
 *   1  every port at the declared OMX_COMP_* default (peak, `auto`, base path);
 *   2  threshold -36 dB, ratio 10, knee 0, attack 0.2 ms (`auto` engages 4x), release 60 ms,
 *      make-up 6 dB, RMS;
 *   3  threshold -24 dB, ratio 4, knee 12 dB, `4x`, bypass toggled every other host block;
 *   4  every port outside its travel (threshold +10 dB, ratio 1000, knee 99 dB, attack NaN,
 *      release 9000 ms, make-up 99 dB, mode 7), the outputs aliasing the inputs.
 *
 *   make test-fx                                    compare
 *   build/fx_dynamics_instance_golden --write       print the lines test/golden/dynamics_instance.sha256 holds
 */
#include <math.h>
#include <stdint.h>

#include <omxdsp/fx/omx_dynamics_instance.h>

#include "golden.h"

#define HOST_BLOCK 1000
#define RENDERS 4

static float g_out[RENDERS * 2 * OMX_FX_GOLDEN_FRAMES];
static float g_l[OMX_FX_GOLDEN_FRAMES], g_r[OMX_FX_GOLDEN_FRAMES];
static float g_ol[OMX_FX_GOLDEN_FRAMES], g_or[OMX_FX_GOLDEN_FRAMES];
static OmxDynamicsInstance g_inst;

static void render_one(int which, float sr, float *out) {
  uint32_t lcg = 0x1234567u;
  for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) omx_fx_golden_stimulus(i, &lcg, &g_l[i], &g_r[i]);
  float t = OMX_COMP_THRESHOLD_DB_DEFAULT, ra = OMX_COMP_RATIO_DEFAULT, k = OMX_COMP_KNEE_DB_DEFAULT,
        a = OMX_COMP_ATTACK_MS_DEFAULT, rl = OMX_COMP_RELEASE_MS_DEFAULT, mk = OMX_COMP_MAKEUP_DB_DEFAULT;
  int rms = 0, ovs = OMX_DYN_OVS_AUTO, alias = 0, toggle = 0;
  switch (which) {
  case 0: break;
  case 1: t = -36.0f; ra = 10.0f; k = 0.0f; a = 0.2f; rl = 60.0f; mk = 6.0f; rms = 1; break;
  case 2: t = -24.0f; ra = 4.0f; k = 12.0f; ovs = OMX_DYN_OVS_X4; toggle = 1; break;
  default:
    t = 10.0f; ra = 1000.0f; k = 99.0f; a = nanf(""); rl = 9000.0f; mk = 99.0f; ovs = 7; alias = 1;
    break;
  }
  omx_dynamics_instance_init(&g_inst, sr);
  float *ol = alias ? g_l : g_ol, *or_ = alias ? g_r : g_or;
  for (int o = 0, b = 0; o < OMX_FX_GOLDEN_FRAMES; o += HOST_BLOCK, b++) {
    const uint32_t n = OMX_FX_GOLDEN_FRAMES - o < HOST_BLOCK ? (uint32_t)(OMX_FX_GOLDEN_FRAMES - o) : HOST_BLOCK;
    omx_dynamics_instance_resolve(&g_inst, toggle && (b & 1), t, ra, k, a, rl, mk, rms, ovs);
    omx_dynamics_instance_run(&g_inst, g_l + o, g_r + o, ol + o, or_ + o, n);
  }
  for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) { out[2 * i] = ol[i]; out[2 * i + 1] = or_[i]; }
}

static int render(float sr, float *out, size_t *bytes) {
  for (int k = 0; k < RENDERS; k++) render_one(k, sr, out + (size_t)k * 2u * OMX_FX_GOLDEN_FRAMES);
  *bytes = sizeof g_out;
  return 0;
}

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "dynamics_instance", render, g_out); }
