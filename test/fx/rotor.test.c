// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The rotor word's oracle (omx_rotor.h, formerly openmixer's mix_rotor.test.c),
 * docs/design/specs/2026-09-26-rotary-speaker.md §5 arms B, C, D, E, at the nine RME rates (32 to
 * 192 kHz), closed forms in double.
 *
 *   B  Doppler: a 1 kHz tone through a rotor settled at the horn's fast speed, 2 s; every sample
 *      against a(u)·Lagrange₃(sin; d(u)) in double at the rotor's own phase word u, and the peak
 *      instantaneous frequency (interpolated zero crossings) against the same estimator over the
 *      ideal closed-form signal; f₀·(1 ± 8·D·rate) is printed beside it.
 *   C  AM: DC through the same rotor; every sample against a(u) = 1 − m·(1 − s(u + ¼))/2.
 *   D  spin-up: slow → fast from a settled slow rotor, 8 s, horn and drum constants: each
 *      sample's phase increment against §2's rate closed form inside the envelope of the float
 *      pole's 2⁻²⁴ and the phase word's 2⁻²⁴ rounding; the time to 95 % of the target inside the
 *      same envelope.
 *   E  stop: fast → stop; the rate reaches exactly 0.0f, no later than the closed form's
 *      τ·ln(rate₀/ε) (+3 % for the float pole) + 1 sample, and the output then holds bit-identical.
 *
 * Pure C, `-lm`, no PipeWire. Built with -DOMX_CONTRACTS; every arm drains the ledger and leaves no
 * violation the rate does not explain (fx_rates.h).
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/fx/omx_rotor.h>

#include "fx_rates.h"

static int g_checks = 0, g_failed = 0;
static void ok(int cond, const char *what, double sr, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s @ %.0f — measured %.9g, limit %.9g\n", what, sr, measured, limit);
  }
}

static void drain_violations(const char *where, double sr) {
  const uint32_t unexplained = omx_fx_drain_ledger((float)sr);
  ok(unexplained == 0u, where, sr, (double)unexplained, 0.0);
}

/* The spec's §3 default speeds, Hz — the row lane declares them as ROTARY_*_RANGE defaults. */
#define HORN_SLOW 0.8f
#define HORN_FAST 6.8f
#define DRUM_SLOW 0.7f
#define DRUM_FAST 5.9f

static float g_rl[OMX_ROTARY_RING_FLOATS], g_rr[OMX_ROTARY_RING_FLOATS];
static struct omx_fdelay g_ll, g_lr;

static void arm_lines(void) {
  memset(g_rl, 0, sizeof g_rl);
  memset(g_rr, 0, sizeof g_rr);
  if (omx_fdelay_init(&g_ll, g_rl, OMX_ROTARY_RING_FLOATS, OMX_ROTOR_ORDER) != OMX_FDELAY_OK ||
      omx_fdelay_init(&g_lr, g_rr, OMX_ROTARY_RING_FLOATS, OMX_ROTOR_ORDER) != OMX_FDELAY_OK)
    ok(0, "the ring the header declares arms a line", 0.0, 0.0, 0.0);
}

/* ---- the closed forms, in double --------------------------------------------------------- */

static double shape(double u) { /* the LFO word's parabola, from its formula */
  u -= floor(u);
  const double t = 2.0 * u - 1.0;
  return 4.0 * t * (1.0 - fabs(t));
}
static double gain_at(double u, double m) { return 1.0 - m * 0.5 * (1.0 - shape(u + 0.25)); }
static double delay_at(double u, double base, double depth) {
  return base + depth * 0.5 * (1.0 + shape(u));
}
/* The order-3 Lagrange read of A·sin(ω·m) at delay d behind sample n: nodes at I−1 … I+2. */
static double lagrange_sine(double A, double w, long n, double d) {
  const double I = floor(d);
  double y = 0.0;
  for (int j = 0; j < 4; j++) {
    const double qj = I - 1.0 + j;
    double L = 1.0;
    for (int k = 0; k < 4; k++)
      if (k != j) L *= (d - (I - 1.0 + k)) / (qj - (I - 1.0 + k));
    y += L * A * sin(w * ((double)n - qj));
  }
  return y;
}
/* omx_rotor_settle() sets the phase, the increment and the rate, not the phase word's Kahan carry:
 * a state starts zeroed, as omx_rotary_init() leaves its rotors, and is settled from there. */
static void settle(struct omx_rotor_state *s, float rate) {
  memset(s, 0, sizeof *s);
  omx_rotor_settle(s, rate, 0.0f);
}

/* ---- arm B + C: a settled fast horn ------------------------------------------------------ */

/* The peak and trough of the zero-crossing frequency of y[skip..n). */
static void crossing_range(const float *y, long n, long skip, double sr, double *fmax, double *fmin) {
  double last = -1.0;
  *fmax = 0.0;
  *fmin = 1e30;
  for (long k = skip + 1; k < n; k++) {
    if (!(y[k - 1] < 0.0f && y[k] >= 0.0f)) continue;
    const double t = (double)(k - 1) + (double)y[k - 1] / ((double)y[k - 1] - (double)y[k]);
    if (last >= 0.0) {
      const double f = sr / (t - last);
      if (f > *fmax) *fmax = f;
      if (f < *fmin) *fmin = f;
    }
    last = t;
  }
}

#define MAX_FRAMES (2u * 192000u)
static float g_y[MAX_FRAMES], g_ideal[MAX_FRAMES];

static void arm_bc(double sr) {
  const double A = 0.5, f0 = 1000.0, w = 2.0 * M_PI * f0 / sr;
  const long N = (long)(2.0 * sr), skip = OMX_ROTARY_RING_FLOATS;
  struct omx_rotor p;
  struct omx_rotor_state s;
  settle(&s, HORN_FAST);
  omx_rotor_resolve(&p, &s, HORN_FAST, OMX_ROTARY_HORN_ACCEL_MS, OMX_ROTARY_HORN_DECEL_MS, 1.0f,
                    OMX_ROTARY_HORN_DOPPLER_MS, OMX_ROTARY_HORN_AM, (float)sr);
  const double base = OMX_ROTARY_BASE_MS * 1e-3 * sr, depth = 2.0 * OMX_ROTARY_HORN_DOPPLER_MS * 1e-3 * sr;
  arm_lines();
  double worst = 0.0;
  for (long n = 0; n < N; n++) {
    float yl = 0.0f, yr = 0.0f;
    omx_rotor_tick(&p, &s, &g_ll, &g_lr, (float)(A * sin(w * (double)n)), (float)(A * sin(w * (double)n)), &yl, &yr);
    const double u = (double)s.lfo.phase;
    const double ud = (double)HORN_FAST * (double)(n + 1) / sr;
    g_y[n] = yl;
    g_ideal[n] = (float)(A * gain_at(ud, OMX_ROTARY_HORN_AM) * sin(w * ((double)n - delay_at(ud, base, depth))));
    if (n >= skip) {
      const double e = fabs((double)yl - gain_at(u, OMX_ROTARY_HORN_AM) * lagrange_sine(A, w, n, delay_at(u, base, depth)));
      if (e > worst) worst = e;
    }
    if (yl != yr) ok(0, "arm B: identical legs read identically", sr, yl - yr, 0.0);
  }
  double fmax, fmin, imax, imin;
  crossing_range(g_y, N, skip, sr, &fmax, &fmin);
  crossing_range(g_ideal, N, skip, sr, &imax, &imin);
  const double dev = 8.0 * OMX_ROTARY_HORN_DOPPLER_MS * 1e-3 * (double)HORN_FAST;
  const double up = 1200.0 * log2(fmax / f0), dn = 1200.0 * log2(fmin / f0);
  const double iup = 1200.0 * log2(imax / f0), idn = 1200.0 * log2(imin / f0);
  printf("arm B @ %6.0f: worst |y − closed form| %.3g FS; peak shift %+.3f / %+.3f cents (ideal %+.3f / %+.3f, f0·(1 ± 8DT) %+.3f / %+.3f)\n",
         sr, worst, up, dn, iup, idn, 1200.0 * log2(1.0 + dev), 1200.0 * log2(1.0 - dev));
  ok(worst <= 1e-4, "arm B: every sample on the closed-form Doppler read", sr, worst, 1e-4);
  ok(fabs(up - iup) <= 0.5, "arm B: peak upward shift in cents", sr, up, iup);
  ok(fabs(dn - idn) <= 0.5, "arm B: peak downward shift in cents", sr, dn, idn);
  drain_violations("arm B: no contract violation", sr);

  /* C: DC in, so the output IS the gain; a quarter-turn ahead of the Doppler read. */
  settle(&s, HORN_FAST);
  arm_lines();
  double cw = 0.0;
  for (long n = 0; n < (long)sr; n++) {
    float yl = 0.0f, yr = 0.0f;
    omx_rotor_tick(&p, &s, &g_ll, &g_lr, 1.0f, 1.0f, &yl, &yr);
    if (n < skip) continue;
    const double e = fabs((double)yl - gain_at((double)s.lfo.phase, OMX_ROTARY_HORN_AM));
    if (e > cw) cw = e;
  }
  printf("arm C @ %6.0f: worst |gain − closed form| %.3g\n", sr, cw);
  ok(cw <= 1e-5, "arm C: the gain follows the quarter-turn-ahead read", sr, cw, 1e-5);
  drain_violations("arm C: no contract violation", sr);
}

/* ---- arm D: spin-up; arm E: stop ---------------------------------------------------------- */

static double rate_after(long n, double r0, double T, double p) { return T + (r0 - T) * pow(p, (double)n); }
static double t95_of(double p, double sr) { return log(20.0) / (-log(p) * sr); }

static void arm_d(double sr, const char *name, float slow, float fast, float accel_ms,
                  float decel_ms, float doppler_ms, float am) {
  const long N = (long)(8.0 * sr), skip = OMX_ROTARY_RING_FLOATS;
  struct omx_rotor p;
  struct omx_rotor_state s;
  settle(&s, slow);
  omx_rotor_resolve(&p, &s, fast, accel_ms, decel_ms, 1.0f, doppler_ms, am, (float)sr);
  const double pole = exp(-1.0 / ((double)accel_ms * 1e-3 * sr)), ulp = ldexp(1.0, -24);
  arm_lines();
  double worst = 0.0, gw = 0.0, t95 = -1.0;
  for (long n = 0; n < N; n++) {
    float yl = 0.0f, yr = 0.0f;
    const double u0 = (double)s.lfo.phase, c0 = (double)s.lfo.carry;
    omx_rotor_tick(&p, &s, &g_ll, &g_lr, 1.0f, 1.0f, &yl, &yr);
    double du = (double)s.lfo.phase - u0;
    if (du < 0.0) du += 1.0;
    /* omx_lfo_advance is Kahan-compensated: the step it takes is inc − carry(n−1) + carry(n), so
     * the increment it was handed is the measured step with both carries put back. */
    du += c0 - (double)s.lfo.carry;
    if (t95 < 0.0 && omx_rotor_rate(&s) >= slow + 0.95f * (fast - slow)) t95 = (double)(n + 1) / sr;
    const double lo = rate_after(n + 1, slow, fast, pole - ulp), hi = rate_after(n + 1, slow, fast, pole + ulp);
    /* The phase sum's half-ulp (2⁻²⁴ where the sum lands in [1, 2) at the wrap), the rate's own
     * half-ulp, and the distance's relative rounding accumulated over n steps — each a float
     * bound, none a fit. */
    const double dev_ref = fabs(rate_after(n + 1, slow, fast, pole) - fast);
    const double slack = ldexp(1.0, -24) * sr + ldexp(1.0, -21) + dev_ref * (double)(n + 1) * ldexp(1.0, -23);
    const double miss = du * sr < lo - slack ? lo - slack - du * sr : du * sr > hi + slack ? du * sr - hi - slack : 0.0;
    if (miss > worst) worst = miss;
    if (n >= skip) {
      const double e = fabs((double)yl - gain_at((double)s.lfo.phase, am));
      if (e > gw) gw = e;
    }
  }
  const double t_lo = t95_of(pole - ulp, sr), t_hi = t95_of(pole + ulp, sr);
  printf("arm D @ %6.0f %s: worst rate miss %.3g Hz outside the pole envelope; gain %.3g; 95 %% at %.4f s (closed form %.4f … %.4f s)\n",
         sr, name, worst, gw, t95, t_lo, t_hi);
  ok(worst == 0.0, "arm D: each phase increment follows the closed-form spin-up", sr, worst, 0.0);
  ok(gw <= 1e-5, "arm D: the gain follows the phase through the spin-up", sr, gw, 1e-5);
  ok(t95 >= t_lo - 1.0 / sr && t95 <= t_hi + 1.0 / sr, "arm D: 95 % of target inside the closed-form envelope", sr, t95, t_hi);
  drain_violations("arm D: no contract violation", sr);

  /* E: from the settled fast rotor, stop. */
  settle(&s, fast);
  omx_rotor_resolve(&p, &s, 0.0f, accel_ms, decel_ms, 1.0f, doppler_ms, am, (float)sr);
  const long limit = (long)ceil((double)decel_ms * 1e-3 * sr * log((double)fast / OMX_ROTARY_STOP_EPS) * 1.03) + 1;
  long zero_at = -1;
  float held = 0.0f;
  int moved = 0;
  for (long n = 0; n < limit + 1000; n++) {
    float yl = 0.0f, yr = 0.0f;
    omx_rotor_tick(&p, &s, &g_ll, &g_lr, 1.0f, 1.0f, &yl, &yr);
    if (zero_at < 0 && omx_rotor_rate(&s) == 0.0f) { zero_at = n; held = yl; continue; }
    if (zero_at >= 0 && (omx_rotor_rate(&s) != 0.0f || yl != held)) moved = 1;
  }
  printf("arm E @ %6.0f %s: rate exactly 0 after %.3f s (bound %.3f s)\n", sr, name,
         (double)zero_at / sr, (double)limit / sr);
  ok(zero_at >= 0 && zero_at <= limit, "arm E: a stopped rotor reaches exactly 0.0f", sr, (double)zero_at, (double)limit);
  ok(!moved, "arm E: a stopped rotor holds its phase and its output", sr, moved, 0.0);
  drain_violations("arm E: no contract violation", sr);
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t k = 0; k < OMX_FX_RME_RATE_COUNT; k++) {
    const double sr = OMX_FX_RME_RATES[k];
    arm_bc(sr);
    arm_d(sr, "horn", HORN_SLOW, HORN_FAST, OMX_ROTARY_HORN_ACCEL_MS, OMX_ROTARY_HORN_DECEL_MS,
          OMX_ROTARY_HORN_DOPPLER_MS, OMX_ROTARY_HORN_AM);
    arm_d(sr, "drum", DRUM_SLOW, DRUM_FAST, OMX_ROTARY_DRUM_ACCEL_MS, OMX_ROTARY_DRUM_DECEL_MS,
          OMX_ROTARY_DRUM_DOPPLER_MS, OMX_ROTARY_DRUM_AM);
  }
  printf("fx/rotor: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_FX_RME_RATE_COUNT);
  return g_failed ? 1 : 0;
}
