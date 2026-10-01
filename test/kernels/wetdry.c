// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- convex wet/dry (dsp-primitives §1 rows 22 and 22a) — rate-free: no rate enters either
 * word, so one pass covers every declared rate ------------------------------------------------ */

static void arm_wetdry(void) {
  g_arm = "wetdry";
  double worst = 0.0;
  int zero_dry = 1, one_wet = 1, loop_same = 1;
  for (int k = 0; k <= 64; k++) {
    const float mix = (float)k / 64.0f, dry = 1.0f - mix;
    for (int n = 0; n < 256; n++) {
      const float xl = rnd(-1.0f, 1.0f), xr = rnd(-1.0f, 1.0f);
      const float wl = rnd(-1.0f, 1.0f), wr = rnd(-1.0f, 1.0f);
      float l, r;
      omx_wetdry_mix(&l, &r, xl, xr, wl, wr, dry, mix);
      const double el = fabs((double)l - ((double)dry * xl + (double)mix * wl));
      const double er = fabs((double)r - ((double)dry * xr + (double)mix * wr));
      const double ulp = (double)nextafterf(1.0f, 2.0f) - 1.0;
      if (el / ulp > worst) worst = el / ulp;
      if (er / ulp > worst) worst = er / ulp;
      if (k == 0 && (memcmp(&l, &xl, sizeof l) != 0 || memcmp(&r, &xr, sizeof r) != 0)) zero_dry = 0;
      if (k == 64 && (memcmp(&l, &wl, sizeof l) != 0 || memcmp(&r, &wr, sizeof r) != 0)) one_wet = 0;
      float ll, lr, fl = 0.0f, fr = 0.0f;
      omx_wetdry_loop(&ll, &lr, &fl, &fr, xl, xr, wl, wr, dry, mix);
      const float want_fl = omx_flush(wl), want_fr = omx_flush(wr);
      if (memcmp(&ll, &l, sizeof l) != 0 || memcmp(&lr, &r, sizeof r) != 0 ||
          memcmp(&fl, &want_fl, sizeof fl) != 0 || memcmp(&fr, &want_fr, sizeof fr) != 0)
        loop_same = 0;
    }
  }
  printf("  wetdry: worst |y − closed form| %.3g ulp over 65 mixes × 256 frames\n", worst);
  ok(worst <= 1.0, "the mix is the closed form to 1 ulp", worst, 1.0);
  ok(zero_dry, "mix = 0 is the dry sample bit for bit", 0.0, 0.0);
  ok(one_wet, "mix = 1 is the wet sample bit for bit", 0.0, 0.0);
  ok(loop_same, "the loop word is flush-then-mix bit for bit", 0.0, 0.0);
  float fl = 1.0f, fr = 1.0f, l, r;
  omx_wetdry_loop(&l, &r, &fl, &fr, 0.0f, 0.0f, 1e-30f, -1e-30f, 0.5f, 0.5f);
  ok(fl == 0.0f && fr == 0.0f, "a subnormal-range loop word is flushed to zero", fl, 0.0);
  expect_clean();
}
