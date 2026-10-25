// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- envdiff_ramp (omx_envelope.h): the contrast's gain-law ramp, one word for the transient
 * designer (offset 0) and the plosive tamer (its sensitivity). At offset 0 it is the transient's ramp
 * as it shipped, `min(1, Δ·(1/REF))`, bit for bit, over 2^20 contrasts; at an offset S it is 0 exactly
 * up to S, `(Δ − S)/REF` past it and 1 from S + REF, TRANSIENT_REF_DB written out here (6 dB). ---- */

static void arm_envdiff_ramp(void) {
  g_arm = "envdiff_ramp";
  const float ref = 6.0f; /* TRANSIENT_REF_DB, stated so a moved one turns this arm red */
  int same = 1, law = 1, zero = 1, full = 1;
  /* Its own generator: the suite's rnd() stream stays the other arms'. */
  uint32_t lcg = 0x5eed0001u;
#define RAMP_RND(lo, hi) ((lcg = lcg * 1664525u + 1013904223u), (lo) + ((hi) - (lo)) * ((float)(lcg >> 8) / 16777216.0f))
  for (uint32_t k = 0; k < (1u << 20); k++) {
    const float d = RAMP_RND(0.0f, 40.0f);
    same &= omx_envdiff_ramp(d, 0.0f) == fminf(1.0f, d * (1.0f / OMX_TRANSIENT_REF_DB));
    const float s = RAMP_RND(0.0f, 30.0f);
    const float u = omx_envdiff_ramp(d, s);
    if (d <= s) zero &= u == 0.0f;
    else if (d >= s + ref) full &= u == 1.0f;
    else law &= fabsf(u - (d - s) / ref) <= 2e-6f;
  }
  ok(same, "at offset 0 the ramp is the transient's as it shipped, bit for bit", same, 1);
  ok(zero, "0 exactly up to the offset", zero, 1);
  ok(law, "(delta - S)/TRANSIENT_REF_DB between", law, 1);
#undef RAMP_RND
  ok(full, "1 from the offset plus TRANSIENT_REF_DB", full, 1);
  expect_clean();
}
