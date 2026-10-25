// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- the wet ducker (omx_duck.h) at the nine oracle rates ---------------------------------- */
#include <omxdsp/omx_duck.h>

/* A one-pole's coefficient for a time constant, here so the arm reads no rate-checked word at 32, 64
 * and 128 kHz. */
static float duck_pole(float ms, float sr) { return (float)exp(-1.0 / (0.001 * (double)ms * (double)sr)); }

static void arm_duck(void) {
  g_arm = "duck";
  static const float RATES[] = OMX_ORACLE_FULL_RATES_INIT;
  static const float LEVEL_DB[] = {-60.0f, -30.0f, -20.0f, -12.0f, -6.0f, 0.0f};
  static float key[400000], wl[400000], wr[400000];
  double worst = 0.0;
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const float sr = RATES[ri];
    const uint32_t n = (uint32_t)sr;
    /* The static law on a steady key, the follower settled. */
    for (uint32_t c = 0; c < 6u; c++)
      for (int dp = 0; dp < 2; dp++) {
        const struct omx_duck d = {-24.0f, dp ? -9.0f : -40.0f, duck_pole(1.0f, sr),
                                   duck_pole(150.0f, sr)};
        struct omx_duck_state st = {0.0f};
        const float k = powf(10.0f, LEVEL_DB[c] / 20.0f);
        for (uint32_t i = 0; i < n; i++) {
          key[i] = (i & 1u) ? -k : k;
          wl[i] = wr[i] = 0.5f;
        }
        omx_duck_run(&d, &st, key, wl, wr, n);
        const double got = 20.0 * log10(wl[n - 1u] / 0.5);
        const double want = fmax(d.depth_db, fmin(0.0, d.thresh_db - LEVEL_DB[c]));
        worst = fmax(worst, fabs(got - want));
        ok(fabs(got - want) < 1e-3, "the wet gain is max(depth, min(0, T - L)) on a steady key (1e-3 dB)", got, want);
        ok(wl[n - 1u] == wr[n - 1u], "one gain serves both legs", wl[n - 1u], wr[n - 1u]);
      }
    /* The release: after the key stops the level falls to 1/e at releaseMs, to the sample. */
    const float rel_ms = 80.0f;
    const struct omx_duck d = {-24.0f, -12.0f, duck_pole(1.0f, sr), duck_pole(rel_ms, sr)};
    struct omx_duck_state st = {1.0f};
    const uint32_t want_n = (uint32_t)(rel_ms * 0.001f * sr + 0.5f);
    uint32_t at = 0;
    for (uint32_t i = 0; i < 2u * want_n && !at; i++) {
      float z = 0.0f, w = 1.0f;
      omx_duck_run(&d, &st, &z, &w, NULL, 1u);
      if (st.level <= (float)exp(-1.0)) at = i + 1u;
    }
    /* within one sample, or 0.05 % of the time where the float pole's rounding, raised to the time's
     * thousands of samples, moves the crossing further */
    const uint32_t tol = want_n / 2000u > 1u ? want_n / 2000u : 1u;
    ok(at + tol >= want_n && at <= want_n + tol, "the level falls to 1/e at releaseMs, within 0.05 % or one sample", at, want_n);
    /* Below the threshold, and at a depth of 0 dB, the wet passes bit for bit. */
    for (uint32_t i = 0; i < 4096u; i++) {
      key[i] = rnd(-0.01f, 0.01f);
      wl[i] = rnd(-1.0f, 1.0f);
      wr[i] = wl[i];
    }
    struct omx_duck_state q = {0.0f};
    const struct omx_duck below = {-24.0f, -12.0f, duck_pole(1.0f, sr), duck_pole(80.0f, sr)};
    omx_duck_run(&below, &q, key, wl, NULL, 4096u);
    ok(memcmp(wl, wr, 4096u * sizeof(float)) == 0, "a key below the threshold leaves the wet bit for bit", 0, 0);
    for (uint32_t i = 0; i < 4096u; i++) key[i] = 1.0f;
    const struct omx_duck off = {-24.0f, 0.0f, duck_pole(1.0f, sr), duck_pole(80.0f, sr)};
    struct omx_duck_state z = {0.25f};
    omx_duck_run(&off, &z, key, wl, NULL, 4096u);
    ok(memcmp(wl, wr, 4096u * sizeof(float)) == 0 && z.level == 0.25f,
       "a depth of 0 dB ducks nothing and moves no state word", z.level, 0.25);
  }
  printf("  duck: worst static-law gap %.2e dB over nine rates\n", worst);
  expect_clean();
}
