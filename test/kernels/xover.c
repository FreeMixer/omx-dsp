// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- Linkwitz-Riley crossover, LR2/LR4 (dsp-primitives §1 row 6) ------------------------------ */

/* The polynomial product `p ⊛ q` of degrees `np − 1` and `nq − 1`. */
static void poly_mul(const double *p, int np, const double *q, int nq, double *out) {
  for (int i = 0; i < np + nq - 1; i++) out[i] = 0.0;
  for (int i = 0; i < np; i++)
    for (int j = 0; j < nq; j++) out[i + j] += p[i] * q[j];
}

/* One crossover over `n` samples of noise: the worst |lo + hi − AP(x)|, lo in place over x. */
static double xover_partition(const struct omx_xover *c, uint32_t n) {
  static float x[20000], lo[20000], hi[20000], ap[20000];
  struct omx_xover_state s;
  struct omx_xover_ap_state as;
  memset(&s, 0, sizeof s);
  memset(&as, 0, sizeof as);
  for (uint32_t i = 0; i < n; i++) x[i] = lo[i] = ap[i] = rnd(-0.5f, 0.5f);
  omx_xover_process(lo, lo, hi, n, c, &s);
  omx_xover_allpass(ap, n, c, &as);
  double worst = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    const double d = fabs((double)lo[i] + hi[i] - ap[i]);
    if (d > worst) worst = d;
  }
  static float lo2[20000], hi2[20000];
  memset(&s, 0, sizeof s);
  omx_xover_process(x, lo2, hi2, n, c, &s);
  ok(memcmp(lo, lo2, n * sizeof(float)) == 0 && memcmp(hi, hi2, n * sizeof(float)) == 0,
     "the crossover in place over its input is the crossover into fresh buffers", 0.0, 0.0);
  return worst;
}

static void arm_xover(void) {
  g_arm = "xover";
  static const double fcs[5] = {20.0, 100.0, 1000.0, 10000.0, 20000.0};
  double worst_sum = 0.0, worst_fc = 0.0, worst_phase = 0.0, worst_ap = 0.0, worst_poly = 0.0, worst_time = 0.0;
  double worst_tree = 0.0, worst_tree_time = 0.0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    double f[10];
    ten_freqs(sr, f);
    for (uint32_t order = 2; order <= 4; order += 2) {
      for (int fi = 0; fi < 5; fi++) {
        const double fc = fcs[fi];
        struct omx_xover c;
        ok(omx_xover_design(&c, order, fc, sr) == OMX_XOVER_OK, "a corner inside the band designs", fc, sr / 2.0);
        const double sgn = order == 2 ? -1.0 : 1.0;
        for (int k = 0; k <= 10; k++) {
          const double w = 2.0 * M_PI * (k < 10 ? f[k] : fc) / sr;
          double complex L = section_h(c.lp, w), Hh = sgn * section_h(c.hp, w);
          if (order == 4) { L *= section_h(c.lp, w); Hh = section_h(c.hp, w) * section_h(c.hp, w); }
          const double ml = cabs(L), mh = cabs(Hh);
          if (fabs(ml + mh - 1.0) > worst_sum) worst_sum = fabs(ml + mh - 1.0);
          if (ml > 1e-3 && mh > 1e-3) {
            const double dphi = fabs(remainder(carg(L) - carg(Hh), 2.0 * M_PI));
            if (dphi > worst_phase) worst_phase = dphi;
          }
          const double dap = cabs(L + Hh - section_h(c.ap, w));
          if (dap > worst_ap) worst_ap = dap;
          if (k == 10) {
            const double d = fmax(fabs(20.0 * log10(ml) + 6.0206), fabs(20.0 * log10(mh) + 6.0206));
            if (d > worst_fc) worst_fc = d;
          }
        }
        double lhs[5], rhs[5], t1[5], t2[5], scale = 0.0;
        const double den[3] = {1.0, c.lp[3], c.lp[4]};
        if (order == 4) {
          poly_mul(c.lp, 3, c.lp, 3, t1);
          poly_mul(c.hp, 3, c.hp, 3, t2);
          for (int i = 0; i < 5; i++) lhs[i] = t1[i] + t2[i];
          poly_mul(c.ap, 3, den, 3, rhs);
          for (int i = 0; i < 5; i++) { scale = fmax(scale, fabs(rhs[i])); worst_poly = fmax(worst_poly, fabs(lhs[i] - rhs[i])); }
        } else {
          const double diff[3] = {c.lp[0] - c.hp[0], c.lp[1] - c.hp[1], c.lp[2] - c.hp[2]};
          const double ap_den[2] = {1.0, c.ap[3]};
          poly_mul(diff, 3, ap_den, 2, lhs);
          poly_mul(c.ap, 2, den, 3, rhs);
          for (int i = 0; i < 4; i++) { scale = fmax(scale, fabs(rhs[i])); worst_poly = fmax(worst_poly, fabs(lhs[i] - rhs[i])); }
          ok(c.ap[2] == 0.0 && c.ap[4] == 0.0 && c.ap[1] == 1.0 && c.ap[0] == c.ap[3] &&
             (float)c.ap[0] == (float)omx_allpass1_coef_d(fc, sr),
             "LR2's all-pass is omx_allpass1's section {a, 1, 0, a, 0}", c.ap[0], 0.0);
        }
        ok(scale > 0.0, "the all-pass polynomial is not empty", scale, 0.0);
        const double t = xover_partition(&c, 20000u);
        if (t > worst_time) worst_time = t;
      }
    }
    /* a three-band LR4 tree at rest: band 1 = AP(f2)·LP(f1), band 2 = LP(f2)·HP(f1), band 3 =
     * HP(f2)·HP(f1); Σ|B_k| = 1 and |Σ B_k| = 1, and in time Σ bands = AP(f2)·AP(f1) */
    struct omx_xover x1, x2;
    ok(omx_xover_design(&x1, 4u, 200.0, sr) == OMX_XOVER_OK && omx_xover_design(&x2, 4u, 2000.0, sr) == OMX_XOVER_OK,
       "the tree's two corners design", 0.0, 0.0);
    for (int k = 0; k < 10; k++) {
      const double w = 2.0 * M_PI * f[k] / sr;
      const double complex l1 = cpow(section_h(x1.lp, w), 2), h1 = cpow(section_h(x1.hp, w), 2);
      const double complex l2 = cpow(section_h(x2.lp, w), 2), h2 = cpow(section_h(x2.hp, w), 2);
      const double complex b1 = l1 * section_h(x2.ap, w), b2 = h1 * l2, b3 = h1 * h2;
      worst_tree = fmax(worst_tree, fabs(cabs(b1) + cabs(b2) + cabs(b3) - 1.0));
      worst_tree = fmax(worst_tree, fabs(cabs(b1 + b2 + b3) - 1.0));
    }
    enum { N = 4096 };
    static float x[N], b1[N], hi1[N], b2[N], b3[N], ref[N];
    struct omx_xover_state s1, s2;
    struct omx_xover_ap_state a2, r1, r2;
    memset(&s1, 0, sizeof s1); memset(&s2, 0, sizeof s2);
    memset(&a2, 0, sizeof a2); memset(&r1, 0, sizeof r1); memset(&r2, 0, sizeof r2);
    for (int i = 0; i < N; i++) x[i] = ref[i] = rnd(-0.5f, 0.5f);
    omx_xover_process(x, b1, hi1, N, &x1, &s1);
    omx_xover_allpass(b1, N, &x2, &a2);
    omx_xover_process(hi1, b2, b3, N, &x2, &s2);
    omx_xover_allpass(ref, N, &x1, &r1);
    omx_xover_allpass(ref, N, &x2, &r2);
    for (int i = 0; i < N; i++) worst_tree_time = fmax(worst_tree_time, fabs((double)b1[i] + b2[i] + b3[i] - ref[i]));
  }
  ok(worst_sum < 1e-6, "|LP| + |HP| = 1 at ten frequencies, LR2 and LR4, every rate", worst_sum, 1e-6);
  ok(worst_fc < 1e-6, "each band is -6.02 dB at its corner", worst_fc, 1e-6);
  ok(worst_phase < 1e-6, "the two bands are in phase", worst_phase, 1e-6);
  ok(worst_ap < 1e-6, "lo + hi is the all-pass at ten frequencies", worst_ap, 1e-6);
  ok(worst_poly < 1e-12, "the sum's polynomial is the all-pass's", worst_poly, 1e-12);
  ok(worst_time < OMX_XOVER_PARTITION_TOLERANCE, "lo + hi = AP(x) over noise, corners 20 Hz to 20 kHz", worst_time, OMX_XOVER_PARTITION_TOLERANCE);
  ok(worst_tree < 1e-6, "a three-band tree at rest partitions unity and is an all-pass", worst_tree, 1e-6);
  ok(worst_tree_time < 2.0 * OMX_XOVER_PARTITION_TOLERANCE, "a three-band tree's bands sum to AP(f2)·AP(f1) in time", worst_tree_time, 2.0 * OMX_XOVER_PARTITION_TOLERANCE);
  printf("xover: |LP|+|HP|-1 %.3g, at fc %.3g dB, phase %.3g rad, vs AP %.3g, poly %.3g, time %.3g, tree %.3g / %.3g\n",
         worst_sum, worst_fc, worst_phase, worst_ap, worst_poly, worst_time, worst_tree, worst_tree_time);
  expect_clean();
}
