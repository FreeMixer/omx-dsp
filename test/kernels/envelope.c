// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- envelope ------------------------------------------------------------------------------- */

/* The N-stage cascade of identical one-poles q from rest: P(Bin(n+N, 1-q) >= N). */
static double cascade_step(uint32_t n, double q) {
  double below = 0.0, c = 1.0;
  for (int k = 0; k < OMX_DYN_ENV_STAGES; k++) {
    if (k > 0) c *= (double)(n + OMX_DYN_ENV_STAGES + 1u - (uint32_t)k) / (double)k;
    below += c * pow(1.0 - q, k) * pow(q, (double)(n + OMX_DYN_ENV_STAGES) - k);
  }
  return 1.0 - below;
}

static void arm_envelope(void) {
  g_arm = "envelope";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    const struct omx_env_params e = {omx_pole_from_time_ms(5.0f, sr), omx_pole_from_time_ms(120.0f, sr), OMX_DETECT_PEAK};
    float ac, rc;
    omx_env_stage_poles(&e, 1u, &ac, &rc);
    const double qa = pow((double)e.attack_pole, OMX_DYN_ENV_STAGES), qr = pow((double)e.release_pole, OMX_DYN_ENV_STAGES);
    ok(fabs(ac - qa) <= 1e-6 * qa, "the base-rate attack stage pole is pole^N", ac, qa);
    ok(fabs(rc - qr) <= 1e-6 * qr, "the base-rate release stage pole is pole^N", rc, qr);
    float a4, r4;
    omx_env_stage_poles(&e, 4u, &a4, &r4);
    const double q4 = pow((double)e.attack_pole, OMX_DYN_ENV_STAGES / 4.0);
    ok(fabs(a4 - q4) <= 1e-6 * q4, "at 4x the stage pole is pole^(N/4)", a4, q4);
    struct omx_env env;
    memset(&env, 0, sizeof env);
    const uint32_t len = (uint32_t)(0.03f * sr);
    double worst = 0.0;
    for (uint32_t i = 0; i < len; i++) {
      const float y = omx_env_step(&env, &e, 1.0f, ac, rc);
      const double d = fabs((double)y - cascade_step(i, ac));
      if (d > worst) worst = d;
    }
    ok(worst <= 1e-4, "the attack step response is the N-stage closed form (float state)", worst, 1e-4);
    ok(omx_env_level(&env, &e) == env.stage[OMX_DYN_ENV_STAGES - 1], "the peak level is the last stage", omx_env_level(&env, &e), env.stage[OMX_DYN_ENV_STAGES - 1]);
    for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) env.stage[s] = 1.0f;
    worst = 0.0;
    for (uint32_t i = 0; i < len; i++) {
      const float y = omx_env_step(&env, &e, 0.0f, ac, rc);
      const double d = fabs((double)y - (1.0 - cascade_step(i, rc)));
      if (d > worst) worst = d;
    }
    ok(worst <= 1e-4, "the release decay is the N-stage closed form (float state)", worst, 1e-4);
    const struct omx_env_params rms = {e.attack_pole, e.release_pole, OMX_DETECT_RMS};
    memset(&env, 0, sizeof env);
    float lev = 0.0f;
    for (uint32_t i = 0; i < (uint32_t)(0.2f * sr); i++) lev = omx_env_step(&env, &rms, 0.25f * 0.25f, ac, rc);
    ok(fabsf(lev - 0.25f) < 1e-4f, "RMS of a settled 0.25 square is 0.25", lev, 0.25);
    ok(lev == sqrtf(env.stage[OMX_DYN_ENV_STAGES - 1]) && lev == omx_env_level(&env, &rms), "the RMS level is the root of the last stage", lev, 0.0);
    const struct omx_env_params inst = {0.0f, 0.0f, OMX_DETECT_PEAK};
    float a0, r0;
    omx_env_stage_poles(&inst, 1u, &a0, &r0);
    memset(&env, 0, sizeof env);
    ok(omx_env_step(&env, &inst, 0.7f, a0, r0) == 0.7f, "pole 0 is instant", env.stage[OMX_DYN_ENV_STAGES - 1], 0.7);
  }
  ok(omx_env_state_size() == sizeof(struct omx_env) && omx_env_state_align() == _Alignof(struct omx_env), "the state layout is exported", (double)omx_env_state_size(), sizeof(struct omx_env));
  expect_clean();
}
