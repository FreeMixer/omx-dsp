// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The feedback detector's golden digests (golden.h), omx_dynamics in 128-frame blocks over the
 * stimulus, two renders back to back per rate, both in `OMX_DYN_TOPOLOGY_FEEDBACK`:
 *   1  a stereo comp: peak, -10 dB, ratio 4, hard knee, make-up +1.5 dB, attack 30 ms, release
 *      300 ms (the bus-glue seed);
 *   2  a mono comp: RMS, -20 dB, ratio 10, knee 6 dB, attack 0.1 ms (floored by the loop),
 *      release 100 ms, detectorOversampling 4x (refused inside the loop).
 *
 *   make test-fx                                      compare
 *   build/fx_dynamics_feedback_golden --write         print the lines test/golden/dynamics_feedback.sha256 holds
 */
#define OMX_FX_GOLDEN_RME 1 /* held at the nine RME rates */

#include <stdint.h>

#include <omxdsp/omx_dyn.h>

#include "golden.h"

#define BLOCK 128
#define RENDERS 2

static float g_out[RENDERS * 2 * OMX_FX_GOLDEN_FRAMES];

static struct omx_dyn atom(int detect, float thresh_db, float ratio, float knee_db, float makeup_db,
                           float att_ms, float rel_ms, int ovs_mode, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = OMX_DYN_ABOVE;
  d.gc.thresh_db = thresh_db;
  d.gc.ratio = ratio;
  d.gc.knee_db = knee_db;
  d.gc.makeup_lin = powf(10.0f, makeup_db / 20.0f);
  d.detect = detect;
  d.ovs_mode = ovs_mode;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(rel_ms, sr);
  d.topology = OMX_DYN_TOPOLOGY_FEEDBACK;
  return d;
}

static void render_one(int which, float sr, float *out) {
  const struct omx_dyn glue = atom(OMX_DETECT_PEAK, -10.0f, 4.0f, 0.0f, 1.5f, 30.0f, 300.0f, OMX_DYN_OVS_AUTO, sr);
  const struct omx_dyn fast = atom(OMX_DETECT_RMS, -20.0f, 10.0f, 6.0f, 0.0f, 0.1f, 100.0f, OMX_DYN_OVS_X4, sr);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    if (which == 0) omx_dynamics(l, r, BLOCK, &glue, &st);
    else omx_dynamics(l, NULL, BLOCK, &fast, &st);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

static int render(float sr, float *out, size_t *bytes) {
  for (int k = 0; k < RENDERS; k++) render_one(k, sr, out + (size_t)k * 2u * OMX_FX_GOLDEN_FRAMES);
  *bytes = sizeof g_out;
  return 0;
}

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "dynamics_feedback", render, g_out); }
