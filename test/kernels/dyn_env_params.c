// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/omx_dyn.h>

/* ---- the dynamics atom's detector parameters (lane/fx-seams, omx-dsp-dev#32): moved unchanged
 * from openmixer's mix_dsp.h. At every declared rate, for the band-dyn's and the de-esser's own
 * time constants in both detector domains, pins today's output bit for bit: the three fields copy
 * unchanged (attack stays attack, release stays release), and the detector driven through the
 * derived parameters — a burst then silence, at rate multipliers 1 and 4 — is sample for sample
 * the detector driven by parameters built by hand from the same poles. ---------------------- */

static void arm_dyn_env_params(void) {
  g_arm = "dyn_env_params";
  static const struct { float att_ms, rel_ms; } times[] = {{1.0f, 60.0f}, {5.0f, 120.0f}, {0.0f, 30.0f}};
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (size_t ti = 0; ti < sizeof times / sizeof times[0]; ti++) {
      for (int detect = OMX_DETECT_PEAK; detect <= OMX_DETECT_RMS; detect++) {
        struct omx_dyn d;
        memset(&d, 0, sizeof d);
        d.enabled = 1;
        d.gc.mode = OMX_DYN_ABOVE;
        d.gc.ratio = 4.0f;
        d.gc.makeup_lin = 1.0f;
        d.detect = detect;
        d.attack_ms = times[ti].att_ms;
        d.attack_coeff = omx_pole_from_time_ms(times[ti].att_ms, sr);
        d.release_coeff = omx_pole_from_time_ms(times[ti].rel_ms, sr);
        const struct omx_env_params e = omx_dyn_env_params(&d);
        ok(e.attack_pole == d.attack_coeff, "the attack pole is the atom's attack coeff, bit for bit", e.attack_pole, d.attack_coeff);
        ok(e.release_pole == d.release_coeff, "the release pole is the atom's release coeff, bit for bit", e.release_pole, d.release_coeff);
        ok(e.detect == detect, "the detector domain is the atom's", e.detect, detect);

        const struct omx_env_params ref = {omx_pole_from_time_ms(times[ti].att_ms, sr),
                                           omx_pole_from_time_ms(times[ti].rel_ms, sr), detect};
        for (uint32_t rate_mul = 1; rate_mul <= 4; rate_mul *= 4) {
          float ac, rc, ac_ref, rc_ref;
          omx_env_stage_poles(&e, rate_mul, &ac, &rc);
          omx_env_stage_poles(&ref, rate_mul, &ac_ref, &rc_ref);
          struct omx_env a, b;
          memset(&a, 0, sizeof a); /* a zeroed state is silence */
          memset(&b, 0, sizeof b);
          const uint32_t n = (uint32_t)(0.05f * sr) * rate_mul;
          uint32_t mismatches = 0;
          for (uint32_t i = 0; i < n; i++) {
            const float x = i < n / 2 ? 0.5f * sinf((float)(2.0 * M_PI * 1000.0 * (double)i / (sr * rate_mul))) : 0.0f;
            const float dx = detect == OMX_DETECT_RMS ? x * x : fabsf(x);
            const float ya = omx_env_step(&a, &e, dx, ac, rc);
            const float yb = omx_env_step(&b, &ref, dx, ac_ref, rc_ref);
            if (ya != yb || omx_env_level(&a, &e) != omx_env_level(&b, &ref)) mismatches++;
          }
          ok(mismatches == 0, "the detector through the atom's parameters is the hand-built one, bit for bit", mismatches, 0);
        }
      }
    }
  }
  expect_clean();
}
