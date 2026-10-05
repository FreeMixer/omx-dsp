// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The self-detecting dynamics slot's golden digests (golden.h), omx_dynamics in 128-frame blocks
 * over the stimulus, two renders back to back per rate:
 *   1  a stereo comp: peak, -18 dB, ratio 6, knee 4 dB, make-up +2 dB, attack 3 ms, release
 *      90 ms (the base path);
 *   2  a mono gate: peak, -36 dB, ratio 20, range -60 dB, attack 0.25 ms (the 4x control path
 *      on `auto`), release 40 ms.
 *
 *   make test-fx                             compare
 *   build/fx_dynamics_golden --write         print the lines test/golden/dynamics.sha256 holds
 */
#include <stdint.h>

#include <omxdsp/omx_dyn.h>

#include "golden.h"

#define BLOCK 128
#define RENDERS 2

static float g_out[RENDERS * 2 * OMX_FX_GOLDEN_FRAMES];

static struct omx_dyn atom(int mode, float thresh_db, float ratio, float knee_db, float range_db,
                           float makeup_db, float att_ms, float rel_ms, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = mode;
  d.gc.thresh_db = thresh_db;
  d.gc.ratio = ratio;
  d.gc.knee_db = knee_db;
  d.gc.range_db = range_db;
  d.gc.makeup_lin = powf(10.0f, makeup_db / 20.0f);
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = OMX_DYN_OVS_AUTO;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(rel_ms, sr);
  return d;
}

static void render_one(int which, float sr, float *out) {
  const struct omx_dyn comp = atom(OMX_DYN_ABOVE, -18.0f, 6.0f, 4.0f, 0.0f, 2.0f, 3.0f, 90.0f, sr);
  const struct omx_dyn gate = atom(OMX_DYN_BELOW, -36.0f, 20.0f, 0.0f, -60.0f, 0.0f, 0.25f, 40.0f, sr);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    if (which == 0) omx_dynamics(l, r, BLOCK, &comp, &st);
    else omx_dynamics(l, NULL, BLOCK, &gate, &st);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

static int render(float sr, float *out, size_t *bytes) {
  for (int k = 0; k < RENDERS; k++) render_one(k, sr, out + (size_t)k * 2u * OMX_FX_GOLDEN_FRAMES);
  *bytes = sizeof g_out;
  return 0;
}

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "dynamics", render, g_out); }
