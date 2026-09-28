/* ---- look-ahead ----------------------------------------------------------------------------- */

static void arm_lookahead(void) {
  g_arm = "lookahead";
  enum { N = 3000, CAP = 1024 };
  static float x[N], ring[CAP], val[CAP];
  static uint32_t when[CAP];
  for (int i = 0; i < N; i++) x[i] = rnd(-2.0f, 2.0f);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    const uint32_t ms_tap = (uint32_t)lrintf(OMX_LIMITER_LOOKAHEAD_MS_DEFAULT * sr / 1000.0f);
    const uint32_t taps[] = {0u, 1u, 7u, ms_tap, CAP - 1u};
    for (int ti = 0; ti < 5; ti++) {
      const uint32_t d = taps[ti] < CAP ? taps[ti] : CAP - 1u;
      struct omx_lookahead la;
      omx_lookahead_init(&la, ring, CAP, 0.25f);
      int exact = 1;
      for (int i = 0; i < N; i++) {
        const float y = omx_lookahead_tick(&la, x[i], d);
        const float want = (uint32_t)i >= d ? x[i - (int)d] : 0.25f;
        exact &= memcmp(&y, &want, sizeof y) == 0;
      }
      ok(exact, "out[n] is in[n - D] bit for bit, the fill before D writes", (double)d, 0.0);
    }
    const uint32_t wins[] = {1u, 2u, ms_tap + 2u < CAP ? ms_tap + 2u : CAP, CAP};
    for (int wi = 0; wi < 4; wi++) {
      struct omx_lookahead_min h;
      omx_lookahead_min_init(&h, val, when, CAP);
      int exact = 1;
      for (int i = 0; i < N; i++) {
        const float m = omx_lookahead_min_push(&h, x[i], wins[wi]);
        float want = x[i];
        for (int k = i; k > i - (int)wins[wi] && k >= 0; k--) want = x[k] < want ? x[k] : want;
        exact &= memcmp(&m, &want, sizeof m) == 0;
      }
      ok(exact, "the hold is the brute-force minimum of the last W pushes, bit for bit", (double)wins[wi], 0.0);
    }
  }
  expect_clean();
}
