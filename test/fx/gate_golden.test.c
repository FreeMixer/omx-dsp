// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The keyed gate's golden digests (golden.h): per host block of 1000 frames (not a multiple of
 * OMX_GATE_CHUNK), the controls resolved and the block run, as a plugin's `run` does; four renders
 * back to back per rate:
 *   1  keyed: threshold -30 dB, the rest at the declared defaults, the key a noise burst over
 *      the stimulus's middle half;
 *   2  the key connected but SELF chosen: threshold -36 dB, ratio 40, range -60 dB, attack 0.1 ms
 *      (the 4x control path), release 40 ms;
 *   3  every control unconnected, no key: the declared defaults;
 *   4  every control outside its travel (threshold +10 dB, ratio 1000, range -200 dB, attack NaN,
 *      release 9000 ms), the outputs aliasing the inputs.
 *
 *   make test-fx                         compare
 *   build/fx_gate_golden --write         print the lines test/golden/gate.sha256 holds
 */
#include <math.h>
#include <stdint.h>

#include <omxdsp/omx_gate.h>

#include "golden.h"

#define HOST_BLOCK 1000
#define RENDERS 4

static float g_out[RENDERS * 2 * OMX_FX_GOLDEN_FRAMES];
static float g_l[OMX_FX_GOLDEN_FRAMES], g_r[OMX_FX_GOLDEN_FRAMES], g_k[OMX_FX_GOLDEN_FRAMES];
static float g_ol[OMX_FX_GOLDEN_FRAMES], g_or[OMX_FX_GOLDEN_FRAMES];

static void render_one(int which, float sr, float *out) {
  uint32_t lcg = 0x1234567u, klcg = 0x89abcdeu;
  for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) {
    omx_fx_golden_stimulus(i, &lcg, &g_l[i], &g_r[i]);
    const float kn = omx_fx_golden_noise(&klcg);
    g_k[i] = i >= OMX_FX_GOLDEN_FRAMES / 4 && i < 3 * OMX_FX_GOLDEN_FRAMES / 4 ? kn : 0.0f;
  }
  static const float one = 1.0f, zero = 0.0f, t30 = -30.0f, t36 = -36.0f, r40 = 40.0f, rg60 = -60.0f,
                     a01 = 0.1f, rl40 = 40.0f, t10 = 10.0f, r1000 = 1000.0f, rg200 = -200.0f, rl9000 = 9000.0f;
  const float nan_ = nanf("");
  struct omx_gate_controls c;
  memset(&c, 0, sizeof c);
  const float *key = NULL;
  int alias = 0;
  switch (which) {
  case 0: c.enabled = &one; c.key_external = &one; c.threshold = &t30; key = g_k; break;
  case 1:
    c.enabled = &one; c.key_external = &zero; c.threshold = &t36; c.ratio = &r40; c.range = &rg60;
    c.attack = &a01; c.release = &rl40; key = g_k;
    break;
  case 2: break;
  default:
    c.threshold = &t10; c.ratio = &r1000; c.range = &rg200; c.attack = &nan_; c.release = &rl9000;
    alias = 1;
    break;
  }
  static struct omx_gate g;
  omx_gate_init(&g, sr);
  float *ol = alias ? g_l : g_ol, *or_ = alias ? g_r : g_or;
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += HOST_BLOCK) {
    const uint32_t n = OMX_FX_GOLDEN_FRAMES - o < HOST_BLOCK ? (uint32_t)(OMX_FX_GOLDEN_FRAMES - o) : HOST_BLOCK;
    struct omx_dyn p;
    omx_gate_resolve(&g, &c, &p);
    omx_gate_run(&g, &p, key ? key + o : NULL, c.key_external, g_l + o, g_r + o, ol + o, or_ + o, n);
  }
  for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) { out[2 * i] = ol[i]; out[2 * i + 1] = or_[i]; }
}

static int render(float sr, float *out, size_t *bytes) {
  for (int k = 0; k < RENDERS; k++) render_one(k, sr, out + (size_t)k * 2u * OMX_FX_GOLDEN_FRAMES);
  *bytes = sizeof g_out;
  return 0;
}

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "gate", render, g_out); }
