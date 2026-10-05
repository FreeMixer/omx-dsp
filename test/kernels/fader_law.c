// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the fader law (lane/mix-matrix, 2026-10-04): fader + pan + send + DCA + mute -> one linear
 * G entry, checked against an independent closed form and its own declared laws. Rate-free: no
 * rate enters either word, so one pass covers every declared rate. --------------------------- */

static double fader_law_closed(float fader_db, float pan, int leg, float send, float dca, int mute,
                                int dca_mute) {
  double g = pow(10.0, (double)fader_db / 20.0);
  if (leg == OMX_PAN_LEG_L) g *= cos(((double)pan + 1.0) * 0.25 * M_PI);
  else if (leg == OMX_PAN_LEG_R) g *= sin(((double)pan + 1.0) * 0.25 * M_PI);
  g *= (double)send * (double)dca;
  if (mute || dca_mute) g = 0.0;
  return g;
}

static void arm_fader_law(void) {
  g_arm = "fader_law";
  double worst = 0.0;
  for (int i = 0; i < 500; i++) {
    struct omx_fader_law_params p;
    p.fader_db = rnd(-60.0f, 12.0f);
    p.pan = rnd(-1.0f, 1.0f);
    const int which = i % 3;
    p.pan_leg = which == 0 ? OMX_PAN_LEG_NONE : (which == 1 ? OMX_PAN_LEG_L : OMX_PAN_LEG_R);
    p.send_lin = rnd(0.0f, 2.0f);
    p.dca_lin = rnd(0.0f, 2.0f);
    p.mute = (i % 7) == 0;
    p.dca_mute = (i % 11) == 0;
    const float g = omx_fader_law_coeff(&p);
    const double ref =
        fader_law_closed(p.fader_db, p.pan, p.pan_leg, p.send_lin, p.dca_lin, p.mute, p.dca_mute);
    const double rel = fabs((double)g - ref) / (fabs(ref) > 1e-6 ? fabs(ref) : 1.0);
    if (rel > worst) worst = rel;
  }
  printf("  fader_law: worst |g - closed form| %.3g relative over 500 draws\n", worst);
  ok(worst <= 1e-5, "the coefficient is the closed form fader*pan*send*dca*mute", worst, 1e-5);

  /* constant-power pan: gL^2 + gR^2 = 1 at every pan */
  double worst_power = 0.0;
  for (int i = 0; i <= 64; i++) {
    const float pan = -1.0f + 2.0f * (float)i / 64.0f;
    const float gl = omx_pan_law_gain(pan, OMX_PAN_LEG_L);
    const float gr = omx_pan_law_gain(pan, OMX_PAN_LEG_R);
    const double power = (double)gl * gl + (double)gr * gr;
    if (fabs(power - 1.0) > worst_power) worst_power = fabs(power - 1.0);
  }
  ok(worst_power <= 1e-6, "the pan law holds constant power at every pan", worst_power, 1e-6);

  /* the declared defaults are unity: 0 dB fader, no pan, unity send and DCA, nothing muted */
  struct omx_fader_law_params unity = {0.0f, 0.0f, OMX_PAN_LEG_NONE, 1.0f, 0, 1.0f, 0};
  const float g_unity = omx_fader_law_coeff(&unity);
  ok(g_unity == 1.0f, "the defaults resolve to unity bit for bit", g_unity, 1.0);

  /* mute (channel or DCA) always zeros the coefficient, whatever else moved */
  struct omx_fader_law_params m1 = {6.0f, 0.3f, OMX_PAN_LEG_L, 1.5f, 1, 1.2f, 0};
  struct omx_fader_law_params m2 = {6.0f, 0.3f, OMX_PAN_LEG_L, 1.5f, 0, 1.2f, 1};
  const float g_m1 = omx_fader_law_coeff(&m1), g_m2 = omx_fader_law_coeff(&m2);
  ok(g_m1 == 0.0f, "a channel mute zeros the coefficient", g_m1, 0.0);
  ok(g_m2 == 0.0f, "a DCA mute zeros the coefficient", g_m2, 0.0);
  expect_clean();
}
