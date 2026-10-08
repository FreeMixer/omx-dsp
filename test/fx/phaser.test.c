// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * phaser.test.c — the phaser's oracle, docs/design/specs/2026-09-22-native-phaser.md §7 arms
 * B, B′, C, D, E, F, G, G′ and H at every declared rate (OMX_DECLARED_RATES). Arm A, the section,
 * is the library suite's `allpass1` arm. Every closed form is §2's, evaluated in double; every
 * measurement is a direct DTFT (Goertzel) of the kernel's own output. No arm compares the kernel
 * with a copy of itself.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <complex.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_phaser.h>

#include "fx_rates.h"

static int g_checks = 0, g_failed = 0;
static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s — measured %.9g, limit %.9g\n", what, measured, limit);
  }
}
static void drain(const char *where) {
  const uint32_t seen = omx_contract_log.count;
  for (uint32_t i = 0; i < seen && i < OMX_CONTRACT_MAX; i++)
    printf("VIOLATION [%s] %s %s\n", omx_contract_log.rec[i].stage, omx_contract_log.rec[i].kind,
           omx_contract_log.rec[i].token);
  ok(seen == 0u, where, (double)seen, 0.0);
  omx_contract_reset();
}

#define MAXLEN (1u << 21)
static float g_l[MAXLEN], g_r[MAXLEN];

static struct omx_phaser atom(int stages, double base, double depth, double rate_hz, double fb,
                              double mix, double sr) {
  struct omx_phaser p = {1, stages, (float)base, (float)depth, omx_lfo_inc((float)rate_hz, (float)sr),
                         (float)fb, (float)mix};
  return p;
}

/* §2's centre at a frozen phase, in double from the word's own shape. */
static double fc_at(const struct omx_phaser *p, double u) {
  const double s = omx_lfo_shape((float)u);
  const double fc = p->base_hz * pow(2.0, p->depth_oct * 0.5 * (1.0 + s));
  return fc < OMX_PHASER_F_TOP_HZ ? fc : OMX_PHASER_F_TOP_HZ;
}

/* §2's H(ω) in double. */
static double complex h_closed(int n, double fc, double fb, double mix, double f, double sr) {
  const double t = tan(M_PI * fc / sr), a = (t - 1.0) / (t + 1.0);
  const double complex z1 = cexp(-I * 2.0 * M_PI * f / sr);
  const double complex an = cpow((a + z1) / (1.0 + a * z1), n);
  return (1.0 - mix) + mix * an / (1.0 - fb * z1 * an);
}

/* The frozen stage's impulse response. With fb ≠ 0 the length is §7's closed form
 * L = ⌈sr·T(1e-9)⌉ + 1024; at fb = 0 the chain's own tail is run until its last 1024 samples
 * are below 1e-10. Either way the last 1024 samples are MEASURED below 1e-9. */
static uint32_t impulse(const struct omx_phaser *p, double u, double sr) {
  struct omx_phaser_state s;
  omx_phaser_state_init(&s);
  s.lfo.phase = (float)u;
  struct omx_phaser q = *p;
  q.lfo_inc = 0.0f;
  uint32_t len;
  if (q.feedback != 0.0f) {
    const double t = tan(M_PI * fc_at(&q, u) / sr);
    const double loop = q.stages / t + 1.0;
    len = (uint32_t)ceil(loop * log(1e-9) / log(fabs(q.feedback))) + 1024u;
    if (len > MAXLEN) len = MAXLEN;
    memset(g_l, 0, len * sizeof(float));
    memset(g_r, 0, len * sizeof(float));
    g_l[0] = 1.0f;
    omx_phaser_process(g_l, g_r, len, &q, &s, (float)sr);
  } else {
    len = 0u;
    memset(g_l, 0, 4096 * sizeof(float));
    g_l[0] = 1.0f;
    for (;;) {
      memset(g_r, 0, 4096 * sizeof(float));
      omx_phaser_process(g_l + len, g_r, 4096u, &q, &s, (float)sr);
      len += 4096u;
      float m = 0.0f;
      for (uint32_t i = len - 1024u; i < len; i++) m = fmaxf(m, fabsf(g_l[i]));
      if (m < 1e-10f || len + 4096u > MAXLEN) break;
      memset(g_l + len, 0, 4096 * sizeof(float));
    }
  }
  float tail = 0.0f;
  for (uint32_t i = len - 1024u; i < len; i++) tail = fmaxf(tail, fabsf(g_l[i]));
  ok(tail < 1e-9f, "the response's last 1024 samples are below 1e-9 (the truncation is measured)", tail, 1e-9);
  return len;
}

static double complex dtft(const float *h, uint32_t len, double f, double sr) {
  const double w = 2.0 * M_PI * f / sr, c = 2.0 * cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (uint32_t i = 0; i < len; i++) {
    const double s0 = h[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double complex y = s1 - cexp(-I * w) * s2;
  return y * cexp(-I * w * (double)(len - 1u));
}
static double mag_db(const float *h, uint32_t len, double f, double sr) {
  return 20.0 * log10(cabs(dtft(h, len, f, sr)) + 1e-300);
}

/* ---- arms B and B′: frozen notch positions and count ---------------------------------------- */

static void arm_b(double sr) {
  double worst = 0.0, worst_depth = -400.0;
  for (int n = 2; n <= OMX_PHASER_MAX_STAGES; n += 2) {
    const struct omx_phaser p = atom(n, 200.0, 4.0, 0.0, 0.0, 0.5, sr);
    for (int j = 0; j < 8; j++) {
      const double u = j / 8.0, fc = fc_at(&p, u), t = tan(M_PI * fc / sr);
      const uint32_t len = impulse(&p, u, sr);
      for (int k = 0; k < n / 2; k++) {
        const double fk = sr / M_PI * atan(t * tan((2 * k + 1) * M_PI / (2.0 * n)));
        const double pk0 = sr / M_PI * atan(t * tan(k * M_PI / n));
        const double pk1 = (2 * (k + 1) == n) ? 0.5 * sr : sr / M_PI * atan(t * tan((k + 1) * M_PI / n));
        double lo = 0.5 * (pk0 + fk), hi = 0.5 * (fk + pk1);
        const double g = 0.5 * (sqrt(5.0) - 1.0);
        double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
        double f1 = cabs(dtft(g_l, len, x1, sr)), f2 = cabs(dtft(g_l, len, x2, sr));
        while (hi - lo > 1e-7 * fk) {
          if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - g * (hi - lo); f1 = cabs(dtft(g_l, len, x1, sr)); }
          else { lo = x1; x1 = x2; f1 = f2; x2 = lo + g * (hi - lo); f2 = cabs(dtft(g_l, len, x2, sr)); }
        }
        const double rel = fabs(0.5 * (lo + hi) - fk) / fk;
        if (rel > worst) worst = rel;
        const double d = mag_db(g_l, len, fk, sr);
        if (d > worst_depth) worst_depth = d;
      }
      if (j == 0 || j == 2 || j == 6) {
        int count = 0;
        double prev2 = 0.0, prev1 = 0.0;
        for (int i = 0; i < 2000; i++) {
          const double f = 20.0 * pow(0.45 * sr / 20.0, i / 1999.0);
          const double m = mag_db(g_l, len, f, sr);
          if (i >= 2 && prev1 < prev2 && prev1 < m && prev1 < -30.0) count++;
          prev2 = prev1;
          prev1 = m;
        }
        ok(count == n / 2, "arm B′: exactly N/2 notches below -30 dB on the log grid", count, n / 2);
      }
    }
  }
  {
    /* An odd request never reaches the chain: 5 runs as 4, the precondition records it. */
    struct omx_phaser odd = atom(5, 200.0, 4.0, 0.0, 0.0, 0.5, sr);
    const uint32_t len = impulse(&odd, 0.0, sr);
    ok(omx_contract_log.count >= 1u && strcmp(omx_contract_log.rec[0].token, "stages-is-a-legal-member") == 0,
       "arm B′: an odd stages is recorded by the precondition", omx_contract_log.count, 1.0);
    omx_contract_reset();
    const double fc = fc_at(&odd, 0.0);
    double dev = 0.0;
    for (int i = 0; i < 50; i++) {
      const double f = 20.0 * pow(0.45 * sr / 20.0, i / 49.0);
      const double c = 20.0 * log10(cabs(h_closed(4, fc, 0.0, 0.5, f, sr)) + 1e-300);
      if (c > -40.0) dev = fmax(dev, fabs(mag_db(g_l, len, f, sr) - c));
    }
    ok(dev <= 0.01, "arm B′: an odd stages 5 runs the 4-section closed form", dev, 0.01);
  }
  ok(worst <= 1e-4, "arm B: every frozen notch sits at the closed-form f_k", worst, 1e-4);
  ok(worst_depth <= -40.0, "arm B: |H| at the closed-form f_k is at most -40 dB", worst_depth, -40.0);
  printf("  %6.0f arm B: worst notch error %.3g relative, shallowest notch %.1f dB\n", sr, worst, worst_depth);
}

/* ---- arms C and G′: frozen, with feedback; the level bound ---------------------------------- */

static void arm_c(double sr) {
  static const double FB[4] = {-0.9, -0.4, 0.4, 0.9}, MIX[2] = {0.5, 1.0}, U[2] = {0.25, 0.75};
  double worst = 0.0, worst_floor = -400.0, worst_excess = -400.0;
  for (int ui = 0; ui < 2; ui++)
    for (int fi = 0; fi < 4; fi++)
      for (int mi = 0; mi < 2; mi++) {
        const struct omx_phaser p = atom(12, 200.0, 4.0, 0.0, FB[fi], MIX[mi], sr);
        const uint32_t len = impulse(&p, U[ui], sr);
        const double fc = fc_at(&p, U[ui]);
        const double bound_db = 20.0 * log10((1.0 - MIX[mi]) + MIX[mi] / (1.0 - fabs(FB[fi])));
        for (int i = 0; i < 200; i++) {
          const double f = 20.0 * pow(0.45 * sr / 20.0, i / 199.0);
          const double m = mag_db(g_l, len, f, sr);
          const double c = 20.0 * log10(cabs(h_closed(12, fc, FB[fi], MIX[mi], f, sr)) + 1e-300);
          if (c > -40.0) { if (fabs(m - c) > worst) worst = fabs(m - c); }
          else if (m > worst_floor) worst_floor = m;
          if (m - bound_db > worst_excess) worst_excess = m - bound_db;
        }
      }
  ok(worst <= 0.01, "arm C: |H| matches the complex closed form within 0.01 dB", worst, 0.01);
  ok(worst_floor <= -34.0, "arm C: where the closed form is below -40 dB, the measurement is below -34 dB", worst_floor, -34.0);
  ok(worst_excess <= 0.01, "arm G′: the level gain never exceeds (1-mix) + mix/(1-|fb|)", worst_excess, 0.01);
  printf("  %6.0f arm C: worst %.4f dB; arm G′: worst excess over the bound %.4f dB\n", sr, worst, worst_excess);
}

/* ---- arm D: the swept notch trajectory ------------------------------------------------------ */

static void arm_d(double sr) {
  const int n = 4;
  const double f0 = 1000.0, amp = 0.5;
  const struct omx_phaser p = atom(n, 200.0, 4.0, 0.5, 0.0, 0.5, sr);
  const uint32_t len = (uint32_t)(2.2 * 2.0 * sr);
  struct omx_phaser_state s;
  omx_phaser_state_init(&s);
  const double w0 = 2.0 * M_PI * f0 / sr;
  for (uint32_t i = 0; i < len; i++) g_l[i] = g_r[i] = (float)(amp * sin(w0 * i));
  omx_phaser_process(g_l, g_r, len, &p, &s, (float)sr);
  const uint32_t win = (uint32_t)lrint(sr / 200.0), hop = win / 4u, K = omx_phaser_control_interval((float)sr);
  const uint32_t nwin = (len - win) / hop;
  static double env[1 << 16];
  for (uint32_t j = 0; j < nwin; j++) {
    const double complex y = dtft(g_l + j * hop, win, f0, sr);
    env[j] = 20.0 * log10(2.0 * cabs(y) / (win * amp) + 1e-300);
  }
  struct omx_lfo ref = {0.0f, p.lfo_inc, 0.0f};
  double prev = 0.0;
  int crossings = 0, matched = 0;
  double worst_lo = 0.0, worst_hi = 0.0, deepest = -400.0;
  for (uint32_t i = 0; i < len; i++) {
    const double fc = fc_at(&p, ref.phase);
    omx_lfo_advance(&ref);
    for (int k = 0; k < n / 2; k++) {
      const double fs = sr / M_PI * atan(tan(M_PI * f0 / sr) / tan((2 * k + 1) * M_PI / (2.0 * n)));
      const double d = fc - fs;
      const double dp = i ? prev - fs : d;
      if (i && ((dp < 0.0) != (d < 0.0)) && i > 2u * win && i + 2u * win < len) {
        crossings++;
        const double tp = (double)i - 1.0 + dp / (dp - d);
        const double t = tan(M_PI * fs / sr), a = (t - 1.0) / (t + 1.0);
        const double tg = (1.0 - a * a) / (1.0 + 2.0 * a * cos(w0) + a * a);
        const double lo = -(hop / 2.0 + 1.0), hi = hop / 2.0 + K + n * tg + 1.0;
        double best = 1e30, best_db = 0.0;
        for (uint32_t j = 1; j + 1 < nwin; j++) {
          if (!(env[j] < env[j - 1] && env[j] <= env[j + 1])) continue;
          const double den = env[j - 1] - 2.0 * env[j] + env[j + 1];
          const double off = den > 0.0 ? 0.5 * (env[j - 1] - env[j + 1]) / den : 0.0;
          const double tm = (j + off) * hop + win / 2.0;
          const double lag = tm - tp;
          if (fabs(lag) < fabs(best)) { best = lag; best_db = env[j] - 0.25 * (env[j - 1] - env[j + 1]) * off; }
        }
        if (best >= lo && best <= hi) matched++;
        else printf("  %6.0f arm D: crossing at %.0f (k=%d) measured lag %.1f outside [%.1f, %.1f]\n", sr, tp, k, best, lo, hi);
        if (best < worst_lo) worst_lo = best;
        if (best > worst_hi) worst_hi = best;
        if (best_db > deepest) deepest = best_db;
      }
    }
    prev = fc;
  }
  ok(crossings >= 8, "arm D: the sweep crosses both notches at least 8 times in 2.2 periods", crossings, 8);
  ok(matched == crossings, "arm D: every crossing is measured inside its lag budget", matched, crossings);
  ok(deepest <= -40.0, "arm D: every swept minimum is at most -40 dB", deepest, -40.0);
  printf("  %6.0f arm D: %d crossings, lags %.1f … %.1f samples, shallowest minimum %.1f dB\n", sr, crossings,
         worst_lo, worst_hi, deepest);
}

/* ---- arm E: no zipper, with its positive control -------------------------------------------- */

static double max_step(const float *y, uint32_t from, uint32_t to) {
  double m = 0.0;
  for (uint32_t i = from + 1u; i < to; i++) {
    const double e = fabs((double)y[i] - (double)y[i - 1]);
    if (!(e <= m)) m = e; /* not fmax: GCC 12.2 arm64, BUILDING.md */
  }
  return m;
}

static void arm_e(double sr) {
  const double f0 = 1000.0, amp = 0.5, w0 = 2.0 * M_PI * f0 / sr;
  const struct omx_phaser p = atom(12, 50.0, 6.0, 5.0, 0.0, 0.5, sr);
  const uint32_t len = (uint32_t)(0.4 * sr), warm = (uint32_t)(0.1 * sr);
  /* The bound: the output is amp·Re{H(n)e^{jω₀n}}, so a step is at most amp·(|e^{jω₀} − 1| +
   * max|H(n+1) − H(n)|) with |H| ≤ 1 at fb 0, H the closed form at the sweep's own coefficient;
   * plus the recurrence's float32 round-off over N sections. */
  struct omx_lfo ref = {0.0f, p.lfo_inc, 0.0f};
  double dh = 0.0;
  double complex hp = h_closed(12, fc_at(&p, 0.0), 0.0, 0.5, f0, sr);
  for (uint32_t i = 0; i < len; i++) {
    omx_lfo_advance(&ref);
    const double complex h = h_closed(12, fc_at(&p, ref.phase), 0.0, 0.5, f0, sr);
    dh = fmax(dh, cabs(h - hp));
    hp = h;
  }
  const double bound = amp * (2.0 * sin(w0 / 2.0) + dh) + 2.0 * amp * 12.0 * 8.0 * FLT_EPSILON;
  struct omx_phaser_state s;
  omx_phaser_state_init(&s);
  for (uint32_t i = 0; i < len; i++) g_l[i] = g_r[i] = (float)(amp * sin(w0 * i));
  for (uint32_t b = 0; b < len; b += 1024u)
    omx_phaser_process(g_l + b, g_r + b, len - b < 1024u ? len - b : 1024u, &p, &s, (float)sr);
  const double step = max_step(g_l, warm, len);
  ok(step <= bound, "arm E: the ramped coefficient adds no zipper beyond the closed-form bound", step, bound);
  /* Positive control: the SAME kernel, one coefficient per 1024-sample block, no ramp. */
  omx_phaser_state_init(&s);
  struct omx_phaser q = p;
  q.lfo_inc = 0.0f;
  struct omx_lfo lfo = {0.0f, p.lfo_inc, 0.0f};
  for (uint32_t i = 0; i < len; i++) g_l[i] = g_r[i] = (float)(amp * sin(w0 * i));
  for (uint32_t b = 0; b < len; b += 1024u) {
    const uint32_t m = len - b < 1024u ? len - b : 1024u;
    s.lfo.phase = lfo.phase;
    omx_phaser_process(g_l + b, g_r + b, 0u, &q, &s, (float)sr);
    s.a_cur = s.a_tgt = omx_allpass1_coef(omx_phaser_fc(&q, omx_lfo_at(&lfo, 0.0f)), (float)sr);
    s.da = 0.0f;
    s.ctr = UINT32_MAX;
    omx_phaser_process(g_l + b, g_r + b, m, &q, &s, (float)sr);
    for (uint32_t i = 0; i < m; i++) omx_lfo_advance(&lfo);
  }
  const double stale = max_step(g_l, warm, len);
  ok(stale > bound, "arm E positive control: a per-block coefficient breaks the same bound", stale, bound);
  printf("  %6.0f arm E: step %.5f, bound %.5f, per-block control %.5f\n", sr, step, bound, stale);
}

/* ---- arm F: bypass identity ----------------------------------------------------------------- */

static void arm_f(double sr) {
  static float in[512], l[512], r[512];
  for (int i = 0; i < 512; i++) in[i] = (i % 3 == 0) ? 1e-39f * (float)(i + 1) : 0.25f * sinf(0.01f * i);
  struct omx_phaser_state s, before;
  omx_phaser_state_init(&s);
  struct omx_phaser p = atom(6, 200.0, 4.0, 0.5, 0.4, 0.5, sr);
  for (int i = 0; i < 512; i++) l[i] = r[i] = in[i];
  omx_phaser_process(l, r, 512u, &p, &s, (float)sr);
  memcpy(&before, &s, sizeof s);
  for (int pass = 0; pass < 2; pass++) {
    struct omx_phaser q = p;
    if (pass == 0) q.enabled = 0; else q.mix = 0.0f;
    memcpy(l, in, sizeof l);
    memcpy(r, in, sizeof r);
    omx_phaser_process(l, r, 512u, &q, &s, (float)sr);
    ok(memcmp(l, in, sizeof l) == 0 && memcmp(r, in, sizeof r) == 0,
       pass ? "arm F: mix 0 is memcmp-identical, subnormal words included" : "arm F: enabled 0 is memcmp-identical, subnormal words included", 0, 0);
    ok(memcmp(&s, &before, sizeof s) == 0, "arm F: the state and the LFO are untouched", 0, 0);
  }
}

/* ---- arm G: no denormal state, and the tail decays ------------------------------------------ */

static int normal_or_zero(float v) { return v == 0.0f || fabsf(v) >= FLT_MIN; }

static void arm_g(double sr) {
  for (int sign = 0; sign < 2; sign++) {
    const double fb = sign ? -0.9 : 0.9;
    struct omx_phaser p = atom(12, 50.0, 4.0, 0.0, fb, 1.0, sr);
    struct omx_phaser_state s;
    omx_phaser_state_init(&s);
    s.lfo.phase = 0.25f;
    const double t = tan(M_PI * 50.0 / sr), loop = 12.0 / t + 1.0;
    const uint32_t win = (uint32_t)ceil(loop);
    const double ceiling = loop / sr * log(1e-5) / log(0.9);
    const uint32_t len = (uint32_t)ceil(ceiling * sr) + 4u * win;
    memset(g_l, 0, len * sizeof(float));
    memset(g_r, 0, len * sizeof(float));
    for (uint32_t i = 0; i < 4096u; i++) g_l[i] = g_r[i] = (float)(0.5 * pow(0.97, i) * ((i & 1) ? -1.0 : 1.0));
    int clean = 1;
    for (uint32_t b = 0; b < len; b += 4096u) {
      const uint32_t m = len - b < 4096u ? len - b : 4096u;
      omx_phaser_process(g_l + b, g_r + b, m, &p, &s, (float)sr);
      for (uint32_t i = 0; i < m; i++) clean &= normal_or_zero(g_l[b + i]) && normal_or_zero(g_r[b + i]);
      for (int leg = 0; leg < 2; leg++) {
        clean &= normal_or_zero(s.w[leg]);
        for (int k = 0; k < OMX_PHASER_MAX_STAGES; k++) clean &= normal_or_zero(s.sec[leg][k].s);
      }
    }
    ok(clean, "arm G: no subnormal in the output, any section word or the feedback words", sign, 0);
    const uint32_t start = 4096u;
    const uint32_t nw = (len - start) / win;
    double first = 0.0, prev = 0.0, when = -1.0;
    int monotone = 1;
    for (uint32_t j = 0; j < nw; j++) {
      double e = 0.0;
      for (uint32_t i = 0; i < win; i++) e += (double)g_l[start + j * win + i] * g_l[start + j * win + i];
      const double rms = sqrt(e / win);
      if (j == 0) first = rms;
      if (j >= 2 && rms > prev) monotone = 0;
      if (when < 0.0 && rms <= first * 1e-5) when = (double)(j * win) / sr;
      prev = rms;
    }
    ok(when >= 0.0 && when <= ceiling, "arm G: the tail falls 100 dB inside the closed-form ceiling T(1e-5)", when, ceiling);
    if (!sign) ok(monotone, "arm G: at fb +0.9 the loop-window RMS is non-increasing after one loop", monotone, 1);
    printf("  %6.0f arm G: fb %+.1f falls 100 dB in %.2f s (ceiling %.2f s), window %u samples\n", sr, fb, when,
           ceiling, win);
    /* Below the flush floor: a normal input from 1e-30 down to FLT_MIN·10, then silence. The
     * flush zeroes every word it reaches; without it the words decay as (−a)ⁿ into subnormals. */
    omx_phaser_state_init(&s);
    s.lfo.phase = 0.25f;
    const uint32_t quiet = (uint32_t)(2.0 * sr);
    for (uint32_t i = 0; i < quiet; i++) {
      const double v = 1e-30 * pow(0.99, i);
      g_l[i] = g_r[i] = v > 10.0 * FLT_MIN ? (float)((i & 1u) ? -v : v) : 0.0f;
    }
    int floor_clean = 1;
    for (uint32_t b = 0; b < quiet; b += 4096u) {
      const uint32_t m = quiet - b < 4096u ? quiet - b : 4096u;
      omx_phaser_process(g_l + b, g_r + b, m, &p, &s, (float)sr);
      for (uint32_t i = 0; i < m; i++) floor_clean &= normal_or_zero(g_l[b + i]) && normal_or_zero(g_r[b + i]);
      for (int leg = 0; leg < 2; leg++) {
        floor_clean &= normal_or_zero(s.w[leg]);
        for (int k = 0; k < OMX_PHASER_MAX_STAGES; k++) floor_clean &= normal_or_zero(s.sec[leg][k].s);
      }
    }
    ok(floor_clean, "arm G: an input driven below the flush floor leaves no subnormal word", sign, 0);
  }
  /* G′'s printed number: the sample-peak excess of a ±1 square wave, which the law does not bound. */
  const struct omx_phaser p = atom(6, 200.0, 4.0, 0.0, 0.0, 0.5, sr);
  struct omx_phaser_state s;
  omx_phaser_state_init(&s);
  const uint32_t len = (uint32_t)sr, half = (uint32_t)lrint(sr / 200.0);
  for (uint32_t i = 0; i < len; i++) g_l[i] = g_r[i] = ((i / half) & 1u) ? -1.0f : 1.0f;
  omx_phaser_process(g_l, g_r, len, &p, &s, (float)sr);
  float peak = 0.0f;
  for (uint32_t i = len / 2u; i < len; i++) peak = fmaxf(peak, fabsf(g_l[i]));
  printf("  %6.0f arm G′: a ±1 square at 100 Hz peaks at %.3f (%+.2f dB over the input; printed, not a law)\n", sr,
         peak, 20.0 * log10(peak));
}

/* ---- arm H: blocks and legs ----------------------------------------------------------------- */

static void arm_h(double sr) {
  const uint32_t len = (uint32_t)sr;
  static float a[192000], b[192000], ra[192000], rb[192000];
  uint32_t seed = 0x1234567u;
  for (uint32_t i = 0; i < len; i++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    a[i] = b[i] = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / (float)sr) + 1e-3f * (float)(seed >> 8) / 16777216.0f;
    ra[i] = rb[i] = a[i];
  }
  const struct omx_phaser p = atom(8, 200.0, 4.0, 3.0, 0.6, 0.7, sr);
  struct omx_phaser_state s1, s2;
  omx_phaser_state_init(&s1);
  omx_phaser_state_init(&s2);
  omx_phaser_process(a, ra, len, &p, &s1, (float)sr);
  for (uint32_t i = 0; i < len;) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    uint32_t m = 1u + seed % 4096u;
    if (m > len - i) m = len - i;
    omx_phaser_process(b + i, rb + i, m, &p, &s2, (float)sr);
    i += m;
  }
  ok(memcmp(a, b, len * sizeof(float)) == 0 && memcmp(ra, rb, len * sizeof(float)) == 0,
     "arm H: one block and random splits of 1 … 4096 are memcmp-identical", 0, 0);
  ok(memcmp(a, ra, len * sizeof(float)) == 0, "arm H: an identical L/R input gives L == R exactly", 0, 0);
}

/* ---- arm T: N-thread identity (dsp-primitives §4.3 (g)) ------------------------------------ */

#define T_THREADS 8
#define T_FRAMES 48000u
struct t_worker {
  float sr;
  float out[2u * T_FRAMES];
};

static void *t_run(void *arg) {
  struct t_worker *w = arg;
  omx_denormals_off();
  static const uint32_t BLK[] = {64u, 37u, 128u, 1u, 256u, 100u, 64u, 9u};
  const struct omx_phaser p = {1, 12, 50.0f, 6.0f, omx_lfo_inc(5.0f, w->sr), 0.9f, 0.7f};
  struct omx_phaser_state s;
  omx_phaser_state_init(&s);
  uint32_t seed = 0x9e3779b9u;
  float l[256], r[256];
  for (uint32_t at = 0, b = 0; at < T_FRAMES; b++) {
    uint32_t n = BLK[b % 8u];
    if (n > T_FRAMES - at) n = T_FRAMES - at;
    for (uint32_t i = 0; i < n; i++) {
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      l[i] = 0.5f * ((float)(seed >> 8) / 16777216.0f * 2.0f - 1.0f);
      r[i] = 0.75f * l[i];
    }
    omx_phaser_process(l, r, n, &p, &s, w->sr);
    for (uint32_t i = 0; i < n; i++) { w->out[2u * (at + i)] = l[i]; w->out[2u * (at + i) + 1u] = r[i]; }
    at += n;
  }
  return NULL;
}

static void arm_t(double sr) {
  static struct t_worker ref, ws[T_THREADS];
  ref.sr = (float)sr;
  t_run(&ref);
  pthread_t tid[T_THREADS];
  for (int t = 0; t < T_THREADS; t++) {
    ws[t].sr = (float)sr;
    if (pthread_create(&tid[t], NULL, t_run, &ws[t]) != 0) { ok(0, "arm T: a worker starts", t, 0); return; }
  }
  for (int t = 0; t < T_THREADS; t++) pthread_join(tid[t], NULL);
  int same = 0;
  for (int t = 0; t < T_THREADS; t++) same += memcmp(ws[t].out, ref.out, sizeof ref.out) == 0;
  ok(same == T_THREADS, "arm T: 8 threads over their own state are byte-identical to one", same, T_THREADS);
}

int main(int argc, char **argv) {
  omx_fx_require_rate_floor();
  (void)argv;
  omx_contract_reset();
  for (unsigned ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    const double want = (double)sr / (double)OMX_PHASER_CONTROL_HZ;
    const uint32_t got = omx_phaser_control_interval(sr);
    ok(got >= 1u && fabs((double)got - want) <= 0.5,
       "the control interval is the whole number of samples nearest one control period, at every declared rate",
       (double)got, want);
  }
  ok(omx_phaser_control_interval(48000.0f) == 8u, "the control interval is 8 samples at 48 k",
     (double)omx_phaser_control_interval(48000.0f), 8.0);
  ok(omx_phaser_clamp_fb(1.5f) == OMX_PHASER_FB_MAX && omx_phaser_clamp_fb(-1.5f) == -OMX_PHASER_FB_MAX,
     "the feedback clamps to the declared ±0.9", omx_phaser_clamp_fb(1.5f), OMX_PHASER_FB_MAX);
  {
    const struct omx_phaser p = atom(6, 1000.0, 6.0, 0.5, 0.0, 0.5, 48000.0);
    float lo, hi;
    omx_phaser_sweep_hz(&p, &lo, &hi);
    ok(lo == 1000.0f && hi == OMX_PHASER_F_TOP_HZ, "sweepHz reports the F_TOP clamp, never silently", hi, (double)OMX_PHASER_F_TOP_HZ);
    const struct omx_phaser d = atom(6, 200.0, 4.0, 0.5, 0.4, 0.5, 48000.0);
    ok(fabs(20.0 * log10(omx_phaser_level_bound(&d)) - 2.50) < 0.01, "the level bound is +2.50 dB at the defaults",
       20.0 * log10(omx_phaser_level_bound(&d)), 2.50);
  }
  drain("the helpers: no contract violation");
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    arm_b(sr);
    arm_c(sr);
    arm_d(sr);
    arm_e(sr);
    arm_f(sr);
    arm_g(sr);
    arm_h(sr);
    arm_t(sr);
    drain("no contract violation at this rate");
  }
  (void)argc;
  printf("mix_phaser: %d checks, %d failed; %u contracts evaluated\n", g_checks, g_failed, omx_contract_log.checks);
  return g_failed == 0 ? 0 : 1;
}
