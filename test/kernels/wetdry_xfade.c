// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- the one-block crossfade, omx_wetdry_xfade_block — rate-free: no rate enters it, so one pass
 * covers every declared rate ----------------------------------------------------------------- */

static void arm_wetdry_xfade(void) {
  g_arm = "wetdry_xfade";
  static const uint32_t sizes[] = {1u, 2u, 7u, 64u, 480u, 4096u};
  const double ulp = (double)nextafterf(1.0f, 2.0f) - 1.0;
  double worst = 0.0, worst_ramp = 0.0;
  int first_from = 1, same_exact = 1;
  for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
    const uint32_t n = sizes[s];
    float *from = malloc(n * sizeof(float)), *to = malloc(n * sizeof(float));
    float *dst = malloc(n * sizeof(float)), *ones = malloc(n * sizeof(float)), *zeros = malloc(n * sizeof(float));
    for (uint32_t i = 0; i < n; i++) {
      from[i] = rnd(-1.0f, 1.0f);
      to[i] = rnd(-1.0f, 1.0f);
      ones[i] = 1.0f;
      zeros[i] = 0.0f;
    }
    omx_wetdry_xfade_block(dst, from, to, n);
    if (memcmp(&dst[0], &from[0], sizeof(float)) != 0) first_from = 0;
    for (uint32_t i = 0; i < n; i++) {
      const double g = (double)i / (double)n;
      const double want = (double)from[i] + ((double)to[i] - (double)from[i]) * g;
      const double e = fabs((double)dst[i] - want) / ulp;
      if (e > worst) worst = e;
    }
    omx_wetdry_xfade_block(dst, from, from, n);
    if (memcmp(dst, from, n * sizeof(float)) != 0) same_exact = 0;
    /* 0 -> 1 reads the incoming gain itself: the ramp i/n, so the two gains sum to 1 */
    omx_wetdry_xfade_block(dst, zeros, ones, n);
    for (uint32_t i = 0; i < n; i++) {
      const double e = fabs((double)dst[i] - (double)i / (double)n) / ulp;
      if (e > worst_ramp) worst_ramp = e;
    }
    free(from);
    free(to);
    free(dst);
    free(ones);
    free(zeros);
  }
  float d[1] = {42.0f};
  const float a[1] = {1.0f}, b[1] = {2.0f};
  omx_wetdry_xfade_block(d, a, b, 0u);
  printf("  wetdry_xfade: worst |y − closed form| %.3g ulp, ramp %.3g ulp over 6 block sizes\n", worst, worst_ramp);
  /* four roundings (to − from, the gain step·i, the product, the sum), each at most half an ulp of a word under 2 */
  ok(worst <= 2.0, "the crossfade is from + (to − from)·i/n to 2 ulp", worst, 2.0);
  ok(first_from, "the first sample is the outgoing body bit for bit", 0.0, 0.0);
  ok(same_exact, "two identical bodies read back bit for bit", 0.0, 0.0);
  ok(worst_ramp <= 1.0, "the incoming gain is the ramp i/n to 1 ulp, so the two gains sum to 1", worst_ramp, 1.0);
  ok(d[0] == 42.0f, "a zero-length block writes nothing", d[0], 42.0);
  expect_clean();
}
