// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The strip gate instance's golden digests (golden.h): per host block of 1000 frames (not a
 * multiple of OMX_GATE_CHUNK), the ports resolved by value and the block run, as a plugin's `run`
 * does through fx/omx_gate_instance.h; four renders back to back per rate:
 *   1  keyed: threshold -30 dB, the rest at the declared defaults, the key a noise burst over the
 *      stimulus's middle half;
 *   2  the key connected but SELF chosen: threshold -36 dB, ratio 40, range -60 dB, attack 0.1 ms
 *      (the 4x control path), release 40 ms;
 *   3  no key, threshold -30 dB, bypass toggled every other host block (state held across it);
 *   4  every port outside its travel (threshold +10 dB, ratio 1000, range -200 dB, attack NaN,
 *      release 9000 ms), the outputs aliasing the inputs.
 *
 *   make test-fx                                compare
 *   build/fx_gate_instance_golden --write       print the lines test/golden/gate_instance.sha256 holds
 */
#include <math.h>
#include <stdint.h>

#include <omxdsp/fx/omx_gate_instance.h>

#include "golden.h"

#define HOST_BLOCK 1000
#define RENDERS 4

static float g_out[RENDERS * 2 * OMX_FX_GOLDEN_FRAMES];
static float g_l[OMX_FX_GOLDEN_FRAMES], g_r[OMX_FX_GOLDEN_FRAMES], g_k[OMX_FX_GOLDEN_FRAMES];
static float g_ol[OMX_FX_GOLDEN_FRAMES], g_or[OMX_FX_GOLDEN_FRAMES];
static OmxGateInstance g_inst;

static void render_one(int which, float sr, float *out) {
  uint32_t lcg = 0x1234567u, klcg = 0x89abcdeu;
  for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) {
    omx_fx_golden_stimulus(i, &lcg, &g_l[i], &g_r[i]);
    const float kn = omx_fx_golden_noise(&klcg);
    g_k[i] = i >= OMX_FX_GOLDEN_FRAMES / 4 && i < 3 * OMX_FX_GOLDEN_FRAMES / 4 ? kn : 0.0f;
  }
  float t = -30.0f, ra = OMX_GATE_RATIO_DEFAULT, rg = OMX_GATE_RANGE_DB_DEFAULT, a = OMX_GATE_ATTACK_MS_DEFAULT,
        rl = OMX_GATE_RELEASE_MS_DEFAULT;
  int ke = 1, alias = 0, toggle = 0;
  const float *key = g_k;
  switch (which) {
  case 0: break;
  case 1: ke = 0; t = -36.0f; ra = 40.0f; rg = -60.0f; a = 0.1f; rl = 40.0f; break;
  case 2: key = NULL; toggle = 1; break;
  default:
    key = NULL; t = 10.0f; ra = 1000.0f; rg = -200.0f; a = nanf(""); rl = 9000.0f; alias = 1;
    break;
  }
  omx_gate_instance_init(&g_inst, sr);
  float *ol = alias ? g_l : g_ol, *or_ = alias ? g_r : g_or;
  for (int o = 0, b = 0; o < OMX_FX_GOLDEN_FRAMES; o += HOST_BLOCK, b++) {
    const uint32_t n = OMX_FX_GOLDEN_FRAMES - o < HOST_BLOCK ? (uint32_t)(OMX_FX_GOLDEN_FRAMES - o) : HOST_BLOCK;
    omx_gate_instance_resolve(&g_inst, toggle && (b & 1), ke, t, rg, a, 0.0f, rl, 0.0f, ra);
    omx_gate_instance_run(&g_inst, key ? key + o : NULL, g_l + o, g_r + o, ol + o, or_ + o, n);
  }
  for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) { out[2 * i] = ol[i]; out[2 * i + 1] = or_[i]; }
}

static int render(float sr, float *out, size_t *bytes) {
  for (int k = 0; k < RENDERS; k++) render_one(k, sr, out + (size_t)k * 2u * OMX_FX_GOLDEN_FRAMES);
  *bytes = sizeof g_out;
  return 0;
}

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "gate_instance", render, g_out); }
