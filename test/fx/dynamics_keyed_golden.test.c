// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The dynamics slot's golden digests (golden.h), omx_dynamics_keyed in 128-frame blocks over the
 * stimulus, four renders back to back per rate, so every path the kernel has is pinned:
 *   1  a stereo gate, self-detecting: peak, -30 dB, ratio 40, range -80 dB, attack 0.1 ms (the
 *      4x control path on `auto`), release 50 ms;
 *   2  a stereo comp, self-detecting: RMS, -20 dB, ratio 4, knee 6 dB, make-up +3 dB, attack
 *      5 ms, release 120 ms (the base path);
 *   3  the gate keyed: attack 1 ms, the key a noise burst of its own in the stimulus's second half;
 *   4  a mono comp, peak, -24 dB, ratio 8, whose attack crosses OMX_DYN_OVS_AUTO_MS (5 ms → 0.2 ms)
 *      halfway, so the handover's crossfade is in the digest.
 *
 *   make test-fx                                 compare
 *   build/fx_dynamics_keyed_golden --write       print the lines test/golden/dynamics_keyed.sha256 holds
 */
#define OMX_FX_GOLDEN_RME 1 /* held at the nine RME rates */

#include <stdint.h>

#include <omxdsp/omx_dyn.h>

#include "golden.h"

#define BLOCK 128
#define RENDERS 4

static float g_out[RENDERS * 2 * OMX_FX_GOLDEN_FRAMES];

static struct omx_dyn atom(int mode, int detect, float thresh_db, float ratio, float knee_db, float range_db,
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
  d.detect = detect;
  d.ovs_mode = OMX_DYN_OVS_AUTO;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(rel_ms, sr);
  return d;
}

static void render_one(int which, float sr, float *out) {
  const struct omx_dyn gate = atom(OMX_DYN_BELOW, OMX_DETECT_PEAK, -30.0f, 40.0f, 0.0f, -80.0f, 0.0f, 0.1f, 50.0f, sr);
  const struct omx_dyn comp = atom(OMX_DYN_ABOVE, OMX_DETECT_RMS, -20.0f, 4.0f, 6.0f, 0.0f, 3.0f, 5.0f, 120.0f, sr);
  const struct omx_dyn keyed = atom(OMX_DYN_BELOW, OMX_DETECT_PEAK, -30.0f, 40.0f, 0.0f, -80.0f, 0.0f, 1.0f, 50.0f, sr);
  const struct omx_dyn slow = atom(OMX_DYN_ABOVE, OMX_DETECT_PEAK, -24.0f, 8.0f, 0.0f, 0.0f, 0.0f, 5.0f, 60.0f, sr);
  const struct omx_dyn fast = atom(OMX_DYN_ABOVE, OMX_DETECT_PEAK, -24.0f, 8.0f, 0.0f, 0.0f, 0.0f, 0.2f, 60.0f, sr);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  uint32_t lcg = 0x1234567u, klcg = 0x89abcdeu;
  float l[BLOCK], r[BLOCK], key[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) {
      omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
      const float kn = omx_fx_golden_noise(&klcg);
      key[i] = (o + i) >= OMX_FX_GOLDEN_FRAMES / 4 && (o + i) < 3 * OMX_FX_GOLDEN_FRAMES / 4 ? kn : 0.0f;
    }
    switch (which) {
    case 0: omx_dynamics_keyed(l, r, NULL, BLOCK, &gate, &st); break;
    case 1: omx_dynamics_keyed(l, r, NULL, BLOCK, &comp, &st); break;
    case 2: omx_dynamics_keyed(l, r, key, BLOCK, &keyed, &st); break;
    default: omx_dynamics_keyed(l, NULL, NULL, BLOCK, o < OMX_FX_GOLDEN_FRAMES / 2 ? &slow : &fast, &st); break;
    }
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

static int render(float sr, float *out, size_t *bytes) {
  for (int k = 0; k < RENDERS; k++) render_one(k, sr, out + (size_t)k * 2u * OMX_FX_GOLDEN_FRAMES);
  *bytes = sizeof g_out;
  return 0;
}

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "dynamics_keyed", render, g_out); }
