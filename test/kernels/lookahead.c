
/* ---- look-ahead ring ------------------------------------------------------------------------ */

static void arm_lookahead(void) {
  g_arm = "lookahead";
  enum { LEN = 4096 };
  static float in[LEN];
  uint32_t seed = 0x9e3779b9u;
  for (uint32_t i = 0; i < LEN; i++) {
    seed = seed * 1664525u + 1013904223u;
    in[i] = (float)(int32_t)seed * (1.0f / 2147483648.0f);
  }
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    const uint32_t cap = (uint32_t)(0.0005f * sr) + 1u;
    float ring[97] = {0};
    int exact = 1;
    for (uint32_t d = 0; d < cap; d++) {
      memset(ring, 0, sizeof ring);
      uint32_t pos = 0;
      for (uint32_t n = 0; n < LEN; n++) {
        ring[pos] = in[n];
        const float out = ring[omx_lookahead_back(pos, d, cap)];
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
  }
}
