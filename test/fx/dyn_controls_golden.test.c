// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The dynamics controls at their NEUTRAL values — hold 0 ms, hysteresis 0 dB, knee 0 dB, mix
 * 100 % — against the kernel that had none of them (golden.h), at the six declared rates.
 *
 * test/golden/dyn_controls.sha256 was written by THIS source compiled with
 * -DOMX_DYN_PREVIOUS_KERNEL against omx-dsp 9adfb61's include/ (the kernel before hold,
 * hysteresis and mix existed), where the four neutral assignments below are compiled out. The
 * current kernel, given the neutral values explicitly, must render the same bytes: a control at
 * its neutral value is not there.
 *
 * Five renders back to back per rate, 128-frame blocks over golden.h's stimulus:
 *   1  a mono gate, -36 dB, ratio 20, range -60 dB, attack 5 ms, release 40 ms (the base path);
 *   2  the same gate at attack 0.25 ms (the 4x control path on `auto`);
 *   3  the same gate keyed from R, gating L;
 *   4  a stereo comp, -18 dB, ratio 6, hard knee, make-up +2 dB, attack 3 ms, release 90 ms, RMS;
 *   5  the same comp at attack 0.25 ms (the 4x control path), peak.
 *
 *   make test-fx                                compare
 *   build/fx_dyn_controls_golden --write        print the lines the digest file holds
 */
#include <stdint.h>

#include <omxdsp/omx_dyn.h>

#include "golden.h"

#define BLOCK 128
#define RENDERS 5

static float g_out[RENDERS * 2 * OMX_FX_GOLDEN_FRAMES];

static struct omx_dyn atom(int mode, float thresh_db, float ratio, float range_db, float makeup_db,
                           int detect, float att_ms, float rel_ms, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = mode;
  d.gc.thresh_db = thresh_db;
  d.gc.ratio = ratio;
  d.gc.knee_db = 0.0f; /* the comp's knee at its neutral value: hard */
  d.gc.range_db = range_db;
  d.gc.makeup_lin = powf(10.0f, makeup_db / 20.0f);
  d.detect = detect;
  d.ovs_mode = OMX_DYN_OVS_AUTO;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(rel_ms, sr);
#ifndef OMX_DYN_PREVIOUS_KERNEL
  d.hold_frames = omx_dyn_hold_frames(0.0f, sr);
  d.hyst_db = 0.0f;
  d.dry = omx_dyn_dry_share(100.0f);
#endif
  return d;
}

static void render_one(int which, float sr, float *out) {
  struct omx_dyn d;
  if (which < 3) d = atom(OMX_DYN_BELOW, -36.0f, 20.0f, -60.0f, 0.0f, OMX_DETECT_PEAK, which == 1 ? 0.25f : 5.0f, 40.0f, sr);
  else d = atom(OMX_DYN_ABOVE, -18.0f, 6.0f, 0.0f, 2.0f, which == 3 ? OMX_DETECT_RMS : OMX_DETECT_PEAK,
                which == 3 ? 3.0f : 0.25f, 90.0f, sr);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    if (which == 2) omx_dynamics_keyed(l, NULL, r, BLOCK, &d, &st);
    else if (which < 2) omx_dynamics(l, NULL, BLOCK, &d, &st);
    else omx_dynamics(l, r, BLOCK, &d, &st);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

static int render(float sr, float *out, size_t *bytes) {
  for (int k = 0; k < RENDERS; k++) render_one(k, sr, out + (size_t)k * 2u * OMX_FX_GOLDEN_FRAMES);
  *bytes = sizeof g_out;
  return 0;
}

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "dyn_controls", render, g_out); }
