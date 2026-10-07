// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "../fx/fx_rates.h"

/* ---- the balance law (lane/fx-seams, omx-dsp-dev#32): moved unchanged from openmixer's
 * mix_dsp.h. Pins today's outputs bit for bit: a fixed table (centre, the hard legs, the clamp
 * beyond them, interior points), and at each of the nine RME rates one second of the tremolo's
 * pan-mode sweep (pan = depth·sin(2π·f·i/sr)) against the closed form, with the law's own
 * claims — the near leg is unity, the far leg is 1 − |pan|, nothing boosts. The law reads no
 * rate, so the ledger stays empty at every one of them. -------------------------------------- */

static void arm_balance_law(void) {
  g_arm = "balance_law";
  static const struct { float pan, bl, br; } table[] = {
      {0.0f, 1.0f, 1.0f},     {1.0f, 0.0f, 1.0f},    {-1.0f, 1.0f, 0.0f},  {0.5f, 0.5f, 1.0f},
      {-0.5f, 1.0f, 0.5f},    {0.25f, 0.75f, 1.0f},  {-0.75f, 1.0f, 0.25f}, {5.0f, 0.0f, 1.0f},
      {-5.0f, 1.0f, 0.0f},    {-0.0f, 1.0f, 1.0f},
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    float bl = -1.0f, br = -1.0f;
    omx_balance_law(table[i].pan, &bl, &br);
    ok(bl == table[i].bl, "the table's left leg, bit for bit", bl, table[i].bl);
    ok(br == table[i].br, "the table's right leg, bit for bit", br, table[i].br);
  }
  ok(omx_clamp_pan(2.0f) == OMX_PAN_PAN_MAX, "the clamp holds the declared maximum", omx_clamp_pan(2.0f), OMX_PAN_PAN_MAX);
  ok(omx_clamp_pan(-2.0f) == OMX_PAN_PAN_MIN, "the clamp holds the declared minimum", omx_clamp_pan(-2.0f), OMX_PAN_PAN_MIN);
  ok(omx_clamp_pan(0.3f) == 0.3f, "the clamp passes an in-range pan", omx_clamp_pan(0.3f), 0.3);

  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    const uint32_t n = (uint32_t)sr;
    const float depth = 1.25f; /* past full scale: the clamp is on the swept path too */
    uint32_t mismatches = 0;
    double worst_boost = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const float pan = depth * sinf((float)(2.0 * M_PI * 5.0 * (double)i / sr));
      float bl, br;
      omx_balance_law(pan, &bl, &br);
      const float c = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
      const float want_l = c > 0.0f ? 1.0f - c : 1.0f;
      const float want_r = c < 0.0f ? 1.0f + c : 1.0f;
      if (bl != want_l || br != want_r) mismatches++;
      if (bl - 1.0 > worst_boost) worst_boost = bl - 1.0;
      if (br - 1.0 > worst_boost) worst_boost = br - 1.0;
      if (bl < 0.0f || br < 0.0f) mismatches++;
    }
    ok(mismatches == 0, "the swept legs are the closed form bit for bit at this rate", mismatches, 0);
    ok(worst_boost == 0.0, "no leg ever boosts at this rate", worst_boost, 0.0);
  }
  expect_clean();
}
