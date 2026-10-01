// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- look-ahead ring ------------------------------------------------------------------------ */

static void arm_lookahead(void) {
  g_arm = "lookahead";
  enum { LEN = 4096, N = 3000, CAP = 1024 };
  static float in[LEN];
  static float x[N], ring[CAP], val[CAP], cursor_ring[CAP];
  static uint32_t when[CAP];
  uint32_t seed = 0x9e3779b9u;
  for (uint32_t i = 0; i < LEN; i++) {
    seed = seed * 1664525u + 1013904223u;
    in[i] = (float)(int32_t)seed * (1.0f / 2147483648.0f);
  }
  for (int i = 0; i < N; i++) x[i] = rnd(-2.0f, 2.0f);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    const uint32_t cap = (uint32_t)(0.0005f * sr) + 1u;
    ok(cap <= CAP, "the cursor ring of 0.5 ms fits the arm's buffer at every declared rate", cap, CAP);
    if (cap > CAP) continue;
    int exact = 1;
    for (uint32_t d = 0; d < cap; d++) {
      memset(cursor_ring, 0, cap * sizeof cursor_ring[0]);
      uint32_t pos = 0;
      for (uint32_t n = 0; n < LEN; n++) {
        cursor_ring[pos] = in[n];
        const float out = cursor_ring[omx_lookahead_back(pos, d, cap)];
        const float want = n >= d ? in[n - d] : 0.0f;
        if (out != want) exact = 0;
        pos = omx_lookahead_fwd(pos, 1u, cap);
      }
    }
    ok(exact, "out[n] == in[n - D] bit for bit at every D in [0, cap)", exact, 1.0);
    int wrap = 1;
    for (uint32_t pos = 0; pos < cap; pos++)
      for (uint32_t k = 0; k <= cap; k++)
        if (omx_lookahead_fwd(pos, k, cap) != (pos + k) % cap ||
            (k < cap && omx_lookahead_back(omx_lookahead_fwd(pos, k, cap), k, cap) != pos))
          wrap = 0;
    ok(wrap, "fwd is (pos + k) mod cap and back undoes it", wrap, 1.0);

    const uint32_t ms_tap = (uint32_t)lrintf(OMX_LIMITER_LOOKAHEAD_MS_DEFAULT * sr / 1000.0f);
    const uint32_t taps[] = {0u, 1u, 7u, ms_tap, CAP - 1u};
    for (int ti = 0; ti < 5; ti++) {
      const uint32_t d = taps[ti] < CAP ? taps[ti] : CAP - 1u;
      struct omx_lookahead la;
      omx_lookahead_init(&la, ring, CAP, 0.25f);
      int tick_exact = 1;
      for (int i = 0; i < N; i++) {
        const float y = omx_lookahead_tick(&la, x[i], d);
        const float want = (uint32_t)i >= d ? x[i - (int)d] : 0.25f;
        tick_exact &= memcmp(&y, &want, sizeof y) == 0;
      }
      ok(tick_exact, "tick: out[n] is in[n - D] bit for bit, the fill before D writes", (double)d, 0.0);
    }
    const uint32_t wins[] = {1u, 2u, ms_tap + 2u < CAP ? ms_tap + 2u : CAP, CAP};
    for (int wi = 0; wi < 4; wi++) {
      struct omx_lookahead_min h;
      omx_lookahead_min_init(&h, val, when, CAP);
      int min_exact = 1;
      for (int i = 0; i < N; i++) {
        const float m = omx_lookahead_min_push(&h, x[i], wins[wi]);
        float want = x[i];
        for (int k = i; k > i - (int)wins[wi] && k >= 0; k--) want = x[k] < want ? x[k] : want;
        min_exact &= memcmp(&m, &want, sizeof m) == 0;
      }
      ok(min_exact, "the hold is the brute-force minimum of the last W pushes, bit for bit", (double)wins[wi], 0.0);
    }
  }
  {
    struct omx_lookahead_min h;
    omx_lookahead_min_init(&h, val, when, 1u);
    int one = 1;
    for (int i = 0; i < 64; i++) one &= omx_lookahead_min_push(&h, x[i], 1u) == x[i];
    ok(one, "a hold of cap 1 returns every push", one, 1.0);
  }
  expect_clean();
}
