// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The oracle of omx_fastmath.h: each of tanhf, expf, log10f and powf, called from a loop the way the
 * kernels call it (so a libmvec build runs the vector symbols), against the double-precision libm over the
 * range the library uses it on. Three arms:
 *   ERROR    the largest error in float ULP of the correctly rounded value, held by OMX_FM_<F>_MAX_ULP;
 *   SPECIALS NaN gives NaN, tanhf(+-inf) is +-1, expf(-inf) is 0, expf overflows to +inf, a result below the
 *            smallest normal is finite, and every finite argument inside the range gives a finite result;
 *   TAIL     a length that is not a multiple of the vector width (the scalar epilogue) holds the same bound.
 * It prints the measured maxima. The build derives which functions run through libmvec; without any the
 * loops are the libm's and the bounds hold a fortiori.
 *   -DOMX_FM_ORACLE_SABOTAGE=1  moves each result by 5 ULP, and the ERROR arm must go red (make fastmath-sabotage).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <omxdsp/omx_fastmath.h>

#define N 4096
static int g_fail = 0, g_checks = 0;

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) { g_fail++; printf("FAIL %s: measured %.9g, limit %.9g\n", what, measured, limit); }
}

/* error of y against the exact value r, in ULP of the float nearest r */
static double ulp_err(float y, double r) {
  float rf = (float)r;
  if (rf == 0.0f || fabsf(rf) < 1.17549435e-38f) return fabs((double)y - r) / 1.40129846e-45 / 8388608.0; /* denormal results: against the normal spacing */
  float up = nextafterf(fabsf(rf), INFINITY);
  double u = (double)up - (double)fabsf(rf);
  return fabs((double)y - r) / u;
}

#ifdef OMX_FM_ORACLE_SABOTAGE
static float bump(float y) { return nextafterf(nextafterf(nextafterf(nextafterf(nextafterf(y, INFINITY), INFINITY), INFINITY), INFINITY), INFINITY); }
#else
static float bump(float y) { return y; }
#endif

static uint32_t g_seed = 0x2545F491u;
static float urand(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; return (float)(g_seed >> 8) / 16777216.0f; }

static float xs[N], ys[N], zs[N];

/* a sweep of a function over [lo, hi]: uniform and log-spaced, `reps` blocks of N, length n each time */
typedef void (*fill_fn)(float *x, int n, float lo, float hi, int blocks);
static void fill(float *x, int n, float lo, float hi, int uniform) {
  for (int i = 0; i < n; i++) {
    if (uniform) x[i] = lo + (hi - lo) * urand();
    else { float a = fabsf(lo) > fabsf(hi) ? fabsf(lo) : fabsf(hi); x[i] = (urand() < 0.5f ? -1.0f : 1.0f) * a * powf(10.0f, -9.0f * urand()); if (x[i] < lo || x[i] > hi) x[i] = lo + (hi - lo) * urand(); }
  }
}

static double worst_tanh(int n, float lo, float hi) {
  double w = 0;
  for (int b = 0; b < 800; b++) {
    fill(xs, n, lo, hi, b & 1);
    OMX_FM_SIMD
    for (int i = 0; i < n; i++) ys[i] = bump(tanhf(xs[i]));
    for (int i = 0; i < n; i++) { double e = ulp_err(ys[i], tanh((double)xs[i])); if (e > w) w = e; }
  }
  return w;
}
static double worst_exp(int n, float lo, float hi) {
  double w = 0;
  for (int b = 0; b < 800; b++) {
    fill(xs, n, lo, hi, b & 1);
    OMX_FM_SIMD
    for (int i = 0; i < n; i++) ys[i] = bump(expf(xs[i]));
    for (int i = 0; i < n; i++) { double e = ulp_err(ys[i], exp((double)xs[i])); if (e > w) w = e; }
  }
  return w;
}
static double worst_log10(int n, float lo, float hi) {
  double w = 0;
  for (int b = 0; b < 800; b++) {
    for (int i = 0; i < n; i++) xs[i] = (b & 1) ? lo + (hi - lo) * urand() : lo * powf(hi / lo, urand());
    OMX_FM_SIMD
    for (int i = 0; i < n; i++) ys[i] = bump(log10f(xs[i]));
    for (int i = 0; i < n; i++) { double e = ulp_err(ys[i], log10((double)xs[i])); if (e > w) w = e; }
  }
  return w;
}
static double worst_pow(int n, float lo, float hi) {
  double w = 0;
  for (int b = 0; b < 800; b++) {
    fill(xs, n, lo, hi, b & 1);
    OMX_FM_SIMD
    for (int i = 0; i < n; i++) ys[i] = bump(powf(10.0f, xs[i]));
    for (int i = 0; i < n; i++) { double e = ulp_err(ys[i], pow(10.0, (double)xs[i])); if (e > w) w = e; }
  }
  return w;
}

int main(void) {
  printf("fx/fastmath: libmvec functions in this build: tanhf=%d expf=%d log10f=%d powf=%d\n", !!(omx_fastmath_enabled() & 1),
         !!(omx_fastmath_enabled() & 2), !!(omx_fastmath_enabled() & 4), !!(omx_fastmath_enabled() & 8));
  /* ERROR: the full block, and the tail of a block of 4093 */
  static const int lens[2] = {N, N - 3};
  for (int k = 0; k < 2; k++) {
    int n = lens[k];
    double t = worst_tanh(n, -30.0f, 30.0f), t2 = worst_tanh(n, -1.0e4f, 1.0e4f);
    double e = worst_exp(n, -87.0f, 88.5f), l = worst_log10(n, 1e-9f, 1e6f), p = worst_pow(n, -12.0f, 4.0f);
    printf("fx/fastmath: n=%d max ULP: tanhf %.2f (|x|<=30) %.2f (|x|<=1e4), expf %.2f, log10f %.2f, powf(10,x) %.2f\n", n, t, t2, e, l, p);
    ok(t <= OMX_FM_TANHF_MAX_ULP && t2 <= OMX_FM_TANHF_MAX_ULP, "tanhf error", t > t2 ? t : t2, OMX_FM_TANHF_MAX_ULP);
    ok(e <= OMX_FM_EXPF_MAX_ULP, "expf error", e, OMX_FM_EXPF_MAX_ULP);
    ok(l <= OMX_FM_LOG10F_MAX_ULP, "log10f error", l, OMX_FM_LOG10F_MAX_ULP);
    ok(p <= OMX_FM_POWF_MAX_ULP, "powf error", p, OMX_FM_POWF_MAX_ULP);
  }
  /* SPECIALS, in a vector-width-aligned block so the vector symbol, not the epilogue, sees them */
  float in[16] = {NAN, INFINITY, -INFINITY, 0.0f, -0.0f, 1e-40f, -1e-40f, 100.0f, -100.0f, 1e30f, -1e30f, 0.5f, -0.5f, 88.0f, -103.9f, 1e-45f};
  float out[16];
  OMX_FM_SIMD
  for (int i = 0; i < 16; i++) out[i] = tanhf(in[i]);
  ok(out[0] != out[0], "tanhf(NaN) is NaN", out[0], 0);
  ok(out[1] == 1.0f && out[2] == -1.0f, "tanhf(+-inf) is +-1", out[1], 1);
  ok(out[3] == 0.0f && out[4] == 0.0f && signbit(out[4]), "tanhf(+-0) is +-0", out[4], 0);
  ok(out[7] == 1.0f && out[8] == -1.0f && out[9] == 1.0f && out[10] == -1.0f, "tanhf saturates to +-1", out[9], 1);
  ok(out[5] - out[5] == 0.0f && out[15] - out[15] == 0.0f, "tanhf of a denormal is finite", out[15], 0);
  OMX_FM_SIMD
  for (int i = 0; i < 16; i++) out[i] = expf(in[i]);
  ok(out[0] != out[0], "expf(NaN) is NaN", out[0], 0);
  ok(out[1] == INFINITY && out[2] == 0.0f, "expf(+inf) is +inf, expf(-inf) is 0", out[2], 0);
  ok(out[7] == INFINITY && out[8] - out[8] == 0.0f && out[8] >= 0.0f, "expf overflows to +inf and underflows to a finite zero", out[8], 0);
  ok(out[14] - out[14] == 0.0f, "expf(-103.9) is finite", out[14], 0);
  OMX_FM_SIMD
  for (int i = 0; i < 16; i++) out[i] = log10f(in[i] < 0.0f ? -in[i] : in[i]);
  ok(out[0] != out[0], "log10f(NaN) is NaN", out[0], 0);
  ok(out[1] == INFINITY && out[3] == -INFINITY, "log10f(+inf) is +inf, log10f(0) is -inf", out[3], 0);
  ok(out[5] - out[5] == 0.0f, "log10f of a denormal is finite", out[5], 0);
  OMX_FM_SIMD
  for (int i = 0; i < 16; i++) out[i] = powf(10.0f, in[i]);
  ok(out[0] != out[0], "powf(10, NaN) is NaN", out[0], 0);
  ok(out[1] == INFINITY && out[2] == 0.0f, "powf(10, +-inf) is +inf, 0", out[2], 0);
  ok(out[3] == 1.0f && out[4] == 1.0f, "powf(10, +-0) is 1", out[3], 1);
  /* FINITE: every finite argument of the range gives a finite result */
  int nonfinite = 0;
  for (int b = 0; b < 200; b++) {
    for (int i = 0; i < N; i++) xs[i] = -12.0f + 16.0f * urand();
    OMX_FM_SIMD
    for (int i = 0; i < N; i++) ys[i] = powf(10.0f, xs[i]);
    for (int i = 0; i < N; i++) for (int j = 0; j < 1; j++) nonfinite += !(ys[i] - ys[i] == 0.0f);
    for (int i = 0; i < N; i++) xs[i] = 1e-9f * powf(1e15f, urand());
    OMX_FM_SIMD
    for (int i = 0; i < N; i++) zs[i] = log10f(xs[i]);
    for (int i = 0; i < N; i++) nonfinite += !(zs[i] - zs[i] == 0.0f);
  }
  ok(nonfinite == 0, "finite arguments in the library's range give finite results", nonfinite, 0);
  printf("fx/fastmath: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
