// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The feedback detector's oracle (omx_dyn.h, `OMX_DYN_TOPOLOGY_FEEDBACK`; omx_gaincomp.h,
 * omx_gaincomp_db_fb) against its math (openmixer docs/design/specs/2026-09-26-ssl-bus-compressor.md
 * §5: every O-row but O6, which waits for the `auto` release word, and O9, the drive's own latency
 * oracle). Contracts on; after each rate no violation the rate does not explain (fx_rates.h).
 * Every arm runs at every OMX_DECLARED_RATES rate; no rate is typed here.
 *
 * The stimulus for the static and timing rows is DC, not the spec's 1 kHz sine: a peak detector
 * on DC hears the level exactly, so the oracle's tolerance is the float round trip rather than a
 * pinned sine-ripple offset — a stricter reading of O2/O3/O4. O10's two tones are 997 Hz and
 * 15013 Hz: at a 1 kHz grid every aliased product of 1 k + 15 k lands on a harmonic bin and
 * cannot be told apart.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_dyn.h>

#include "fx_rates.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_checks = 0, g_fail = 0;
static void check(int ok, const char *what, float sr) {
  g_checks++;
  if (!ok) { g_fail++; fprintf(stderr, "FAIL @ %.0f Hz: %s\n", (double)sr, what); }
}

static struct omx_dyn fb_comp(float sr, float thresh, float ratio, float knee, float a_ms, float r_ms) {
  struct omx_dyn p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.gc.mode = OMX_DYN_ABOVE;
  p.detect = OMX_DETECT_PEAK;
  p.gc.thresh_db = thresh;
  p.gc.ratio = ratio;
  p.gc.knee_db = knee;
  p.gc.makeup_lin = 1.0f;
  p.attack_ms = a_ms;
  p.attack_coeff = omx_pole_from_time_ms(a_ms, sr);
  p.release_coeff = omx_pole_from_time_ms(r_ms, sr);
  p.ovs_mode = OMX_DYN_OVS_AUTO;
  p.topology = OMX_DYN_TOPOLOGY_FEEDBACK;
  return p;
}

/** Run `n` samples of DC `x` through the slot in quanta of 128; the last gain, dB, lands in `*gdb`. */
static void run_dc(const struct omx_dyn *p, struct omx_dyn_state *st, float x, uint32_t n, float *gdb, float *trace) {
  float buf[128];
  uint32_t done = 0;
  while (done < n) {
    const uint32_t m = n - done < 128u ? n - done : 128u;
    for (uint32_t i = 0; i < m; i++) buf[i] = x;
    omx_dynamics(buf, NULL, m, p, st);
    if (trace)
      for (uint32_t i = 0; i < m; i++) trace[done + i] = 20.0f * log10f(buf[i] / x);
    done += m;
    *gdb = 20.0f * log10f(buf[m - 1] / x);
  }
}

/* O1 — feedforward never reads the loop's state: poisoned feedback words, identical output. */
static void o1_feedforward_is_untouched(float sr) {
  enum { N = 4096 };
  static float a[N], b[N];
  for (int i = 0; i < N; i++) a[i] = b[i] = 0.8f * sinf(2.0f * (float)M_PI * 997.0f * (float)i / sr);
  struct omx_dyn p = fb_comp(sr, -20.0f, 4.0f, 6.0f, 1.0f, 100.0f);
  p.topology = OMX_DYN_TOPOLOGY_FEEDFORWARD;
  struct omx_dyn_state s1, s2;
  omx_dyn_state_init(&s1, 1u);
  omx_dyn_state_init(&s2, 1u);
  s2.fb_env = 0.37f;
  s2.fb_yl = -0.9f;
  s2.fb_yr = 0.4f;
  for (int o = 0; o < N; o += 256) {
    omx_dynamics(a + o, NULL, 256, &p, &s1);
    omx_dynamics(b + o, NULL, 256, &p, &s2);
  }
  check(memcmp(a, b, sizeof a) == 0, "O1 feedforward output is independent of the feedback state, bit for bit", sr);
}

/* O1b — a keyed slot detects its key, which a loop cannot hear through: feedback on a keyed slot
 * is the feed-forward keyed slot, bit for bit. */
static void o1b_keyed_stays_feedforward(float sr) {
  enum { N = 4096 };
  static float a[N], b[N], key[N];
  for (int i = 0; i < N; i++) {
    a[i] = b[i] = 0.5f * sinf(2.0f * (float)M_PI * 997.0f * (float)i / sr);
    key[i] = 0.9f * sinf(2.0f * (float)M_PI * 211.0f * (float)i / sr);
  }
  struct omx_dyn fb = fb_comp(sr, -20.0f, 4.0f, 6.0f, 1.0f, 100.0f);
  struct omx_dyn ff = fb;
  ff.topology = OMX_DYN_TOPOLOGY_FEEDFORWARD;
  struct omx_dyn_state s1, s2;
  omx_dyn_state_init(&s1, 1u);
  omx_dyn_state_init(&s2, 1u);
  for (int o = 0; o < N; o += 256) {
    omx_dynamics_keyed(a + o, NULL, key + o, 256, &fb, &s1);
    omx_dynamics_keyed(b + o, NULL, key + o, 256, &ff, &s2);
  }
  check(memcmp(a, b, sizeof a) == 0, "O1b a keyed slot in feedback is the keyed feed-forward slot, bit for bit", sr);
}

/* O2 — the hard-knee static curve IS the feed-forward curve at the same ratio. */
static void o2_static_curve_hard_knee(float sr) {
  static const float ratios[] = {2.0f, 4.0f, 10.0f};
  static const float threshs[] = {-30.0f, -20.0f, -10.0f};
  double worst = 0.0;
  for (int ri = 0; ri < 3; ri++)
    for (int ti = 0; ti < 3; ti++)
      for (int L = -40; L <= 0; L++) {
        struct omx_dyn p = fb_comp(sr, threshs[ti], ratios[ri], 0.0f, 1.0f, 50.0f);
        struct omx_dyn_state st;
        omx_dyn_state_init(&st, 1u);
        float g;
        run_dc(&p, &st, powf(10.0f, (float)L / 20.0f), (uint32_t)(0.1f * sr), &g, NULL);
        const double want = -(1.0 - 1.0 / ratios[ri]) * fmax(0.0, (double)L - threshs[ti]);
        const double err = fabs((double)g - want);
        if (err > worst) worst = err;
      }
  printf("  O2 @ %6.0f Hz: worst |GR - (1-1/R)(L-T)| = %.5f dB\n", (double)sr, worst);
  check(worst <= 0.01, "O2 the hard-knee feedback static curve is the feed-forward curve within 0.01 dB", sr);
}

/* The output-domain characteristic in double, for the implicit solve O3 needs. */
static double gfb(double u, double R, double W) {
  if (u <= -W / 2.0) return 0.0;
  if (u >= W / 2.0) return -(R - 1.0) * u;
  return -(R - 1.0) * (u + W / 2.0) * (u + W / 2.0) / (2.0 * W);
}

/* O3 — the soft-knee static curve is the unique solution of y = x + g_fb(y), by bisection. */
static void o3_static_curve_soft_knee(float sr) {
  const double R = 4.0, T = -20.0, W = 6.0;
  double worst = 0.0;
  for (double L = -32.0; L <= -8.0; L += 0.5) {
    double lo = L - 100.0, hi = L;
    for (int k = 0; k < 200; k++) {
      const double mid = 0.5 * (lo + hi);
      if (mid - L - gfb(mid - T, R, W) > 0.0) hi = mid; else lo = mid;
    }
    const double want = 0.5 * (lo + hi) - L;
    struct omx_dyn p = fb_comp(sr, (float)T, (float)R, (float)W, 1.0f, 50.0f);
    struct omx_dyn_state st;
    omx_dyn_state_init(&st, 1u);
    float g;
    run_dc(&p, &st, powf(10.0f, (float)L / 20.0f), (uint32_t)(0.1f * sr), &g, NULL);
    const double err = fabs((double)g - want);
    if (err > worst) worst = err;
  }
  printf("  O3 @ %6.0f Hz: worst |GR - implicit solution| = %.5f dB\n", (double)sr, worst);
  check(worst <= 0.01, "O3 the soft-knee feedback static curve solves y = x + g_fb(y) within 0.01 dB", sr);
}

/* O4 — a +1 dB step in steady reduction decays with the loop pole rho = p - (1-p)(R-1). */
static void o4_loop_time_constant(float sr) {
  const float R = 4.0f, T = -20.0f, a_ms = 10.0f;
  struct omx_dyn p = fb_comp(sr, T, R, 0.0f, a_ms, 200.0f);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  float g0;
  run_dc(&p, &st, powf(10.0f, (T + 12.0f) / 20.0f), (uint32_t)(1.0f * sr), &g0, NULL);
  const uint32_t n = (uint32_t)(0.1f * sr);
  float *tr = malloc(n * sizeof *tr), g1;
  run_dc(&p, &st, powf(10.0f, (T + 13.0f) / 20.0f), n, &g1, tr);
  const double gfinal = -(1.0 - 1.0 / R) * 13.0;
  const double e0 = fabs((double)tr[0] - gfinal);
  uint32_t k = 0;
  while (k < n && fabs((double)tr[k] - gfinal) > e0 / M_E) k++;
  const double pa = exp(-1.0 / (a_ms * 0.001 * sr));
  const double rho = pa - (1.0 - pa) * (R - 1.0);
  const double tau = -1.0 / log(rho);
  printf("  O4 @ %6.0f Hz: 1/e after %u samples, closed form tau_eff %.1f (%.3f ms)\n", (double)sr, k, tau,
         tau / sr * 1000.0);
  check(fabs((double)k - tau) <= 0.05 * tau, "O4 the loop decays with tau/R's closed form within 5 %", sr);
  free(tr);
}

/* The reduction's overshoot past its final value, in milli-dB, after a +1 dB step from
 * equilibrium (the small-signal regime L3/L4 are stated in): a sign-alternating loop pole shows as
 * an attack that overshoots, which the slow release then recovers. `pa` < 0 runs the KERNEL;
 * otherwise the SABOTAGE replica — the kernel's own words with the floor left out, at pole `pa`. */
static int step_overshoot_mdb(float sr, float R, float a_ms, float pa, float *settle_err) {
  const float T = -20.0f;
  const float x0 = powf(10.0f, (T + 12.0f) / 20.0f), x1 = powf(10.0f, (T + 13.0f) / 20.0f);
  const uint32_t warm = (uint32_t)(1.0f * sr), n = (uint32_t)(0.02f * sr) + 64u;
  float *tr = malloc(n * sizeof *tr);
  if (pa < 0.0f) {
    struct omx_dyn p = fb_comp(sr, T, R, 0.0f, a_ms, 100.0f);
    struct omx_dyn_state st;
    omx_dyn_state_init(&st, 1u);
    float g;
    run_dc(&p, &st, x0, warm, &g, NULL);
    run_dc(&p, &st, x1, n, &g, tr);
  } else {
    struct omx_gaincomp_params gc = {OMX_DYN_ABOVE, T, R, 0.0f, 0.0f, 1.0f};
    const float pr = omx_pole_from_time_ms(100.0f, sr);
    const float eq = powf(10.0f, (T + 12.0f / R) / 20.0f);
    float env = eq, y = eq;
    for (uint32_t i = 0; i < n; i++) {
      const float d = fabsf(y);
      const float e = omx_onepole(&env, d, d > env ? pa : pr);
      const float gdb = omx_gaincomp_db_fb(&gc, omx_lin_to_db(e));
      y = x1 * omx_db_to_lin(gdb);
      tr[i] = gdb;
      if (!(fabsf(y) < 1e6f)) { for (uint32_t k = i; k < n; k++) tr[k] = (k & 1u) ? 60.0f : -60.0f; break; }
    }
  }
  const float gfinal = (float)(-(1.0 - 1.0 / R) * 13.0);
  float over = 0.0f;
  for (uint32_t i = 0; i < n; i++)
    if (gfinal - tr[i] > over) over = gfinal - tr[i];
  *settle_err = fabsf(tr[n - 1] - gfinal);
  free(tr);
  return (int)lrintf(over * 1000.0f);
}

/* O5 — the loop never oscillates: over the declared attack travel and ratios {2, 4, 10, 20} a
 * small step settles without a sign-alternating move; the loop's attack at the declared minimum is
 * L4's table to 1e-4 ms; and the SABOTAGE — the same loop with the floor removed at 10:1 and the
 * declared minimum attack — goes red on the same metric wherever the floor engages. */
static void o5_no_oscillation(float sr) {
  static const float ratios[] = {2.0f, 4.0f, 10.0f, 20.0f};
  double worst_ms = 0.0;
  int worst_flips = 0;
  float worst_settle = 0.0f;
  for (int ri = 0; ri < 4; ri++) {
    for (int k = 0; k <= 8; k++) {
      const float a_ms = OMX_COMP_ATTACK_MS_MIN * powf(10.0f, (float)k / 8.0f);
      float se;
      const int f = step_overshoot_mdb(sr, ratios[ri], a_ms, -1.0f, &se);
      if (f > worst_flips) worst_flips = f;
      if (se > worst_settle) worst_settle = se;
    }
    const float pa = omx_dyn_fb_attack_pole(omx_pole_from_time_ms(OMX_COMP_ATTACK_MS_MIN, sr), ratios[ri]);
    const double eff = -1000.0 / ((double)sr * log((double)pa));
    const double floor_ms = -1000.0 / ((double)sr * log((ratios[ri] - 1.0) / ratios[ri]));
    const double d = fabs(eff - fmax((double)OMX_COMP_ATTACK_MS_MIN, floor_ms));
    if (d > worst_ms) worst_ms = d;
    printf("  O5 @ %6.0f Hz, %4.1f:1: effectiveAttackMs at the declared minimum %.4f ms\n", (double)sr,
           (double)ratios[ri], eff);
  }
  printf("  O5 @ %6.0f Hz: overshoot %d mdB, worst settle error %.4f dB, |effectiveAttackMs - L4| %.2e ms\n",
         (double)sr, worst_flips, (double)worst_settle, worst_ms);
  /* 75 mdB: at the floor rho is 0 and only the one-pole's linear-domain average against the log-domain
   * law is left — measured 53-54 mdB on a 1 dB step at 20:1, rate-independent; the unfloored loop
   * is 116 mdB at rho = -0.07 and 874-1012 mdB at rho = -1. */
  check(worst_flips <= 75, "O5 a small step overshoots its final reduction by at most 75 mdB", sr);
  check(worst_settle <= 0.01f, "O5 a small step settles on the static curve within 0.01 dB", sr);
  check(worst_ms <= 1e-4, "O5 the loop's effective attack at the minimum is L4's table to 1e-4 ms", sr);
  const float raw = omx_pole_from_time_ms(OMX_COMP_ATTACK_MS_MIN, sr);
  if (raw < 0.9f) {
    float se;
    const int sf = step_overshoot_mdb(sr, 10.0f, OMX_COMP_ATTACK_MS_MIN, raw, &se);
    printf("  O5 sabotage @ %6.0f Hz (floor removed, 10:1): overshoot %d mdB\n", (double)sr, sf);
    check(sf > 75, "O5 sabotage: the unfloored loop overshoots past the 75 mdB bound", sr);
  }
}

/* O7 — zero latency, the 4x path refused inside the loop whatever the choice says. */
static void o7_zero_latency(float sr) {
  struct omx_dyn p = fb_comp(sr, -10.0f, 4.0f, 0.0f, OMX_COMP_ATTACK_MS_MIN, 300.0f);
  p.ovs_mode = OMX_DYN_OVS_X4;
  check(omx_dyn_oversample_factor(&p) == 1u, "O7 feedback answers factor 1 with detectorOversampling 4x", sr);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  float buf[256] = {0};
  buf[0] = 0.25f;
  omx_dynamics(buf, NULL, 256, &p, &st);
  check(buf[0] != 0.0f, "O7 an impulse is heard at index 0", sr);
}

/* O8 — no denormal state after a burst and 2 s of silence (the thread's FTZ is clear here). */
static void o8_no_denormal_state(float sr) {
  struct omx_dyn p = fb_comp(sr, -30.0f, 4.0f, 6.0f, 1.0f, 50.0f);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  float g;
  run_dc(&p, &st, 0.9f, (uint32_t)(0.05f * sr), &g, NULL);
  float buf[128];
  for (uint32_t done = 0; done < (uint32_t)(2.0f * sr); done += 128u) {
    memset(buf, 0, sizeof buf);
    omx_dynamics(buf, NULL, 128u, &p, &st);
  }
  const float w[3] = {st.fb_env, st.fb_yl, st.fb_yr};
  int ok = 1;
  for (int i = 0; i < 3; i++) ok &= (w[i] == 0.0f || fabsf(w[i]) >= OMX_FLUSH_THRESHOLD);
  check(ok, "O8 every feedback state word is 0 or normal after 2 s of silence", sr);
}

/* O10 — the aliasing L5 costs, measured and pinned (±1 dB), one figure per declared rate. */
static void fft(double *re, double *im, uint32_t n) {
  for (uint32_t i = 1, j = 0; i < n; i++) {
    uint32_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (uint32_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * M_PI / len;
    for (uint32_t i = 0; i < n; i += len)
      for (uint32_t k = 0; k < len / 2; k++) {
        const double wr = cos(ang * k), wi = sin(ang * k);
        const double ur = re[i + k], ui = im[i + k];
        const double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
        const double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
      }
  }
}

/* The largest spectral line in 20 Hz … 20 kHz that no in-band product a·f1 + b·f2 explains,
 * relative to the f1 line, dBc: what the loop's rectifier folded back from above Nyquist. */
static double o10_worst_alias_dbc(float sr) {
  enum { N = 65536 };
  const double f1 = 997.0, f2 = 15013.0;
  struct omx_dyn p = fb_comp(sr, -20.0f, 4.0f, 0.0f, OMX_COMP_ATTACK_MS_MIN, 100.0f);
  const uint32_t warm = (uint32_t)(0.5f * sr), tot = warm + N;
  float *x = malloc(tot * sizeof *x);
  double *re = malloc(N * sizeof *re), *im = malloc(N * sizeof *im);
  for (uint32_t i = 0; i < tot; i++)
    x[i] = 0.5f * (float)sin(2.0 * M_PI * f1 * i / sr) + 0.5f * (float)sin(2.0 * M_PI * f2 * i / sr);
  struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  for (uint32_t o = 0; o < tot; o += 256u) omx_dynamics(x + o, NULL, (tot - o) < 256u ? tot - o : 256u, &p, &st);
  for (uint32_t i = 0; i < N; i++) {
    const double t = 2.0 * M_PI * i / (N - 1);
    re[i] = x[warm + i] * (0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2 * t) - 0.01168 * cos(3 * t));
    im[i] = 0.0;
  }
  fft(re, im, N);
  const double bin = sr / N;
  double ref = 0.0, worst = 0.0;
  for (uint32_t k = 1; k < N / 2; k++) {
    const double f = k * bin, m = re[k] * re[k] + im[k] * im[k];
    if (fabs(f - f1) <= 4.0 * bin && m > ref) ref = m;
    if (f < 20.0 || f > 20000.0) continue;
    int explained = 0;
    for (int a = -60; a <= 60 && !explained; a++)
      for (int b = -8; b <= 8 && !explained; b++) {
        const double u = a * f1 + b * f2;
        if (u >= 0.0 && u <= sr / 2.0 && fabs(u - f) <= 4.0 * bin) explained = 1;
      }
    if (!explained && m > worst) worst = m;
  }
  free(x); free(re); free(im);
  return 10.0 * log10(worst / ref + 1e-30);
}

int main(void) {
  omx_fx_require_rate_floor();
  /* O10's pinned figures, one per OMX_DECLARED_RATES entry, in its order — measured by this file
   * and recorded, not a target (ssl-bus-compressor §5 O10). */
  static const double O10_PINNED_DBC[OMX_DECLARED_RATE_COUNT] = {-57.37, -33.19, -57.31, -70.19, -64.57, -80.90};
  printf("fx/dynamics_feedback: the feedback detector against its math\n");
  for (unsigned k = 0; k < OMX_DECLARED_RATE_COUNT; k++) {
    const float sr = OMX_DECLARED_RATES[k];
    o1_feedforward_is_untouched(sr);
    o1b_keyed_stays_feedforward(sr);
    o2_static_curve_hard_knee(sr);
    o3_static_curve_soft_knee(sr);
    o4_loop_time_constant(sr);
    o5_no_oscillation(sr);
    o7_zero_latency(sr);
    o8_no_denormal_state(sr);
    const double al = o10_worst_alias_dbc(sr);
    printf("  O10 @ %6.0f Hz: worst aliased product %.2f dBc (pinned %.2f)\n", (double)sr, al, O10_PINNED_DBC[k]);
    check(fabs(al - O10_PINNED_DBC[k]) <= 1.0, "O10 the worst aliased product stays at its pinned figure +-1 dB", sr);
    const uint32_t unexplained = omx_fx_drain_ledger(sr);
    check(unexplained == 0u, "no contract violation in this rate's arms", sr);
  }
  printf("fx/dynamics_feedback: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
