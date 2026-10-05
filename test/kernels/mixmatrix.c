// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the summing matrix (lane/mix-matrix, 2026-10-04): Y = G·X, dense and sparse, ramped where
 * an entry moves inside the block. The matrix's own arithmetic is rate-free; this arm still runs
 * once per declared rate because each pass derives its block length `n` from the rate, so every
 * declared 44.1/48/88.2/96/176.4/192 kHz block size the engine can hand it is covered. --------- */

#define MIXMATRIX_STRIPS 8u
#define MIXMATRIX_OUT 3u
#define MIXMATRIX_N_MAX 2048u

static void mixmatrix_naive(float *const *out, const float *const *in, const float *g_prev,
                             const float *g_cur, uint32_t n_strips, uint32_t n_out, uint32_t n) {
  for (uint32_t o = 0; o < n_out; o++) {
    float *yo = out[o];
    for (uint32_t i = 0; i < n; i++) yo[i] = 0.0f;
    for (uint32_t s = 0; s < n_strips; s++) {
      const float *xs = in[s];
      const float gp = g_prev[o * n_strips + s], gc = g_cur[o * n_strips + s];
      const float step = (gc - gp) / (float)n;
      for (uint32_t i = 0; i < n; i++) yo[i] += (gp == gc ? gc : (gp + step * (float)i)) * xs[i];
    }
  }
}

static void arm_mixmatrix(void) {
  g_arm = "mixmatrix";
  static float in_buf[MIXMATRIX_STRIPS][MIXMATRIX_N_MAX];
  static float out_a[MIXMATRIX_OUT][MIXMATRIX_N_MAX];
  static float out_b[MIXMATRIX_OUT][MIXMATRIX_N_MAX];
  static float out_ref[MIXMATRIX_OUT][MIXMATRIX_N_MAX];
  static float g_prev[MIXMATRIX_OUT * MIXMATRIX_STRIPS];
  static float g_cur[MIXMATRIX_OUT * MIXMATRIX_STRIPS];
  const float *in_ptr[MIXMATRIX_STRIPS];
  float *out_a_ptr[MIXMATRIX_OUT], *out_b_ptr[MIXMATRIX_OUT], *out_ref_ptr[MIXMATRIX_OUT];
  struct omx_mixmatrix_entry entries[MIXMATRIX_OUT * MIXMATRIX_STRIPS];

  for (uint32_t s = 0; s < MIXMATRIX_STRIPS; s++) in_ptr[s] = in_buf[s];
  for (uint32_t o = 0; o < MIXMATRIX_OUT; o++) {
    out_a_ptr[o] = out_a[o];
    out_b_ptr[o] = out_b[o];
    out_ref_ptr[o] = out_ref[o];
  }

  int steady_exact = 1, sparse_exact = 1;
  double worst_ramp_ulp = 0.0;
  const double ulp = (double)nextafterf(1.0f, 2.0f) - 1.0;

  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const uint32_t n = (uint32_t)(OMX_DECLARED_RATES[ri] / 375.0f);
    for (uint32_t s = 0; s < MIXMATRIX_STRIPS; s++)
      for (uint32_t i = 0; i < n; i++) in_buf[s][i] = rnd(-1.0f, 1.0f);

    /* steady: every entry's g_prev == g_cur, including the zeros */
    for (uint32_t o = 0; o < MIXMATRIX_OUT; o++)
      for (uint32_t s = 0; s < MIXMATRIX_STRIPS; s++) {
        const float g = ((o + s) % 3) == 0 ? 0.0f : rnd(0.1f, 1.0f);
        g_prev[o * MIXMATRIX_STRIPS + s] = g;
        g_cur[o * MIXMATRIX_STRIPS + s] = g;
      }
    mixmatrix_naive(out_ref_ptr, in_ptr, g_prev, g_cur, MIXMATRIX_STRIPS, MIXMATRIX_OUT, n);
    omx_mixmatrix_dense(out_a_ptr, in_ptr, g_prev, g_cur, MIXMATRIX_STRIPS, MIXMATRIX_OUT, n);
    for (uint32_t o = 0; o < MIXMATRIX_OUT; o++)
      if (memcmp(out_a[o], out_ref[o], (size_t)n * sizeof(float)) != 0) steady_exact = 0;

    /* ramped and sparse together: some entries move, a third are declared zero */
    uint32_t n_entries = 0;
    for (uint32_t o = 0; o < MIXMATRIX_OUT; o++)
      for (uint32_t s = 0; s < MIXMATRIX_STRIPS; s++) {
        const int zero = ((o + s) % 3) == 0;
        const float gp = zero ? 0.0f : rnd(0.1f, 1.0f);
        const float gc = zero ? 0.0f : rnd(0.1f, 1.0f);
        g_prev[o * MIXMATRIX_STRIPS + s] = gp;
        g_cur[o * MIXMATRIX_STRIPS + s] = gc;
        if (!zero) {
          entries[n_entries].strip = s;
          entries[n_entries].out = o;
          entries[n_entries].g_prev = gp;
          entries[n_entries].g_cur = gc;
          n_entries++;
        }
      }
    mixmatrix_naive(out_ref_ptr, in_ptr, g_prev, g_cur, MIXMATRIX_STRIPS, MIXMATRIX_OUT, n);
    omx_mixmatrix_dense(out_a_ptr, in_ptr, g_prev, g_cur, MIXMATRIX_STRIPS, MIXMATRIX_OUT, n);
    omx_mixmatrix_sparse(out_b_ptr, in_ptr, entries, n_entries, MIXMATRIX_STRIPS, MIXMATRIX_OUT, n);
    for (uint32_t o = 0; o < MIXMATRIX_OUT; o++)
      for (uint32_t i = 0; i < n; i++) {
        if (memcmp(&out_a[o][i], &out_b[o][i], sizeof(float)) != 0) sparse_exact = 0;
        const double d = fabs((double)out_a[o][i] - (double)out_ref[o][i]) / ulp;
        if (d > worst_ramp_ulp) worst_ramp_ulp = d;
      }
  }
  printf("  mixmatrix: dense vs the naive reference, worst ramped |y - ref| %.3g ulp over %u declared rates\n",
         worst_ramp_ulp, (unsigned)OMX_DECLARED_RATE_COUNT);
  ok(steady_exact, "dense matches the naive reference bit for bit when every G is steady", 0.0, 0.0);
  ok(worst_ramp_ulp <= 4.0, "dense matches the naive reference within 4 ulp when G ramps", worst_ramp_ulp, 4.0);
  ok(sparse_exact, "sparse matches dense bit for bit over the same entries", 0.0, 0.0);
  expect_clean();
}
