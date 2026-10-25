// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The one-knob leveler's instance oracle (omx_leveler_instance.h) at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  an amount of 0 is the input, bit for bit, every direction and speed; so are a fresh instance and
//      a bypassed one;
//   B  the table: at each point of LEVELER_AMOUNT_POINTS the slots take the declared row, written out
//      here, and between two points each column is linear (1e-6); both slots run RMS detectors with
//      LEVELER_KNEE_DB of knee;
//   C  the static law on a held level, the three directions: down is `(1/R - 1)(L - T)` over T plus the
//      make-up, up is `(1 - 1/R)(T - L)` under T capped at the boost, both is down then up at the level
//      down left (1e-3 dB), over -70 to -3 dBFS at three amounts;
//   D  each speed's attack and release are its declared milliseconds, bit for bit;
//   E  hostile words land on the travel's ends and the sets' defaults, the ledger is clean, nothing
//      allocates, a rate outside the oracle set is refused;
//   F  one block or random blocks of 1 to 4096 frames are the same bits.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_leveler_instance.h>

#include "instance_oracle.h"

#include <stdlib.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
static int g_counting = 0;
static long g_allocs = 0;
void *__wrap_malloc(size_t n) {
  if (g_counting) g_allocs++;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t sz) {
  if (g_counting) g_allocs++;
  return __real_calloc(n, sz);
}
void *__wrap_realloc(void *p, size_t n) {
  if (g_counting) g_allocs++;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (g_counting) g_allocs++;
  __real_free(p);
}

/* The declarations, stated here so a moved one turns its arm red (leveler_instance-perturb.sh). */
static const double POINTS[5] = {0, 25, 50, 75, 100};
static const double DOWN_T[5] = {0, -10, -16, -22, -28};
static const double DOWN_R[5] = {1, 1.5, 2, 3, 4};
static const double DOWN_M[5] = {0, 1, 2.5, 4, 6};
static const double UP_T[5] = {-40, -42, -45, -48, -50};
static const double UP_R[5] = {1, 1.5, 2, 2.5, 3};
static const double UP_RANGE[5] = {0, 3, 6, 9, 12};
static const float ATTACK[3] = {30, 10, 3};
static const float RELEASE[3] = {800, 300, 100};
#define KNEE 6.0

#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxLevelerInstance g_v;

/* The ledger is drained after every resolve and every block: at 32, 64 and 128 kHz each pole made
 * records an explained `rate-is-declared`, and the ledger keeps only OMX_CONTRACT_MAX of them. */
static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static void setup(float amount, int dir, int speed) {
  omx_leveler_instance_init(&g_v, g_sr);
  drain();
  omx_leveler_instance_resolve(&g_v, 0, amount, dir, speed);
  drain();
}
static void run(OmxLevelerInstance *v, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) {
    omx_leveler_instance_run(v, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
    drain();
  }
}

static double lerp(const double *t, double a) {
  int k = 0;
  while (k < 3 && a > POINTS[k + 1]) k++;
  return t[k] + (a - POINTS[k]) / (POINTS[k + 1] - POINTS[k]) * (t[k + 1] - t[k]);
}
static double law_above(double t, double r, double l) {
  const double x = l - t, s = 1.0 / r - 1.0, w = KNEE;
  if (fabs(x) < 0.5 * w) return s * (x + 0.5 * w) * (x + 0.5 * w) / (2.0 * w);
  return x <= 0.0 ? 0.0 : s * x;
}
static double law_up(double t, double r, double range, double l) {
  const double x = l - t, s = 1.0 - 1.0 / r, w = KNEE;
  double g;
  if (fabs(x) < 0.5 * w) g = s * (0.5 * w - x) * (0.5 * w - x) / (2.0 * w);
  else g = x >= 0.0 ? 0.0 : -s * x;
  return g > range ? range : g;
}

/* ---- A: an amount of 0 ----------------------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "A";
  const uint32_t n = (uint32_t)(g_sr * 0.2f);
  stimulus(g_l, g_r, n, 0x1e7eu, 3000u);
  int same = 1;
  for (int d = 0; d < 3; d++)
    for (int sp = 0; sp < 3; sp++) {
      setup(0.0f, d, sp);
      run(&g_v, g_l, g_r, g_ol, g_or, n);
      same &= memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0;
    }
  ok(same, "an amount of 0 is the input, bit for bit, every direction and speed", same, 1);
  omx_leveler_instance_init(&g_v, g_sr);
  drain();
  run(&g_v, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a fresh instance is the input", 0, 0);
  omx_leveler_instance_init(&g_v, g_sr);
  omx_leveler_instance_resolve(&g_v, 1, 80.0f, 2, 2);
  drain();
  run(&g_v, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a bypassed instance is the input", 0, 0);
  clean();
}

/* ---- B: the table ---------------------------------------------------------------------------- */
static void arm_table(void) {
  g_arm = "B";
  static const float AMTS[] = {0, 10, 25, 37.5f, 50, 62, 75, 90, 100};
  for (int k = 0; k < 9; k++) {
    setup(AMTS[k], 2, 1);
    const double a = AMTS[k];
    const double want[6] = {lerp(DOWN_T, a), lerp(DOWN_R, a), lerp(DOWN_M, a), lerp(UP_T, a), lerp(UP_R, a), lerp(UP_RANGE, a)};
    const double got[6] = {g_v.down.gc.thresh_db, g_v.down.gc.ratio, 20.0 * log10(g_v.down.gc.makeup_lin),
                           g_v.up.gc.thresh_db, g_v.up.gc.ratio, g_v.up.gc.range_db};
    for (int c = 0; c < 6; c++) ok(fabs(got[c] - want[c]) < 1e-5, "the table's row, linear between its points", got[c], want[c]);
    ok(g_v.down.gc.mode == OMX_DYN_ABOVE && g_v.up.gc.mode == OMX_DYN_UPWARD && g_v.down.detect == OMX_DETECT_RMS &&
           g_v.up.detect == OMX_DETECT_RMS && g_v.down.gc.knee_db == (float)KNEE && g_v.up.gc.knee_db == (float)KNEE &&
           g_v.up.gc.makeup_lin == 1.0f,
       "down above, up upward, RMS, the declared knee, no make-up on the lift", 0, 0);
  }
  clean();
}

/* ---- C: the static law on a held level ------------------------------------------------------- */
static double held(float amount, int dir, float l_db) {
  setup(amount, dir, 2);
  const float x = omx_db_to_lin(l_db);
  const uint32_t n = (uint32_t)(1.2f * g_sr);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = x;
  run(&g_v, g_l, g_r, g_ol, g_or, n);
  return 20.0 * log10((double)g_ol[n - 1u] / (double)x);
}
static void arm_law(void) {
  g_arm = "C";
  static const float AMTS[] = {30, 60, 100};
  double worst = 0.0;
  for (int k = 0; k < 3; k++) {
    const double a = AMTS[k];
    for (float l = -70.0f; l <= -3.0f; l += 6.7f) {
      const double gd = law_above(lerp(DOWN_T, a), lerp(DOWN_R, a), l) + lerp(DOWN_M, a);
      const double gu = law_up(lerp(UP_T, a), lerp(UP_R, a), lerp(UP_RANGE, a), l);
      const double gb = gd + law_up(lerp(UP_T, a), lerp(UP_R, a), lerp(UP_RANGE, a), l + gd);
      const double md = held(AMTS[k], 0, l), mu = held(AMTS[k], 1, l), mb = held(AMTS[k], 2, l);
      worst = fmax(worst, fmax(fabs(md - gd), fmax(fabs(mu - gu), fabs(mb - gb))));
      ok(fabs(md - gd) < 1e-3, "down: the compressor's law plus the make-up (1e-3 dB)", md, gd);
      ok(fabs(mu - gu) < 1e-3, "up: the upward law, capped at the boost (1e-3 dB)", mu, gu);
      ok(fabs(mb - gb) < 1e-3, "both: down, then up at the level down left (1e-3 dB)", mb, gb);
    }
  }
  printf("  leveler @ %.0f Hz: the static law, worst %.2e dB\n", (double)g_sr, worst);
  clean();
}

/* ---- D: the speeds --------------------------------------------------------------------------- */
static void arm_speed(void) {
  g_arm = "D";
  for (int sp = 0; sp < 3; sp++) {
    setup(50.0f, 2, sp);
    const float a = omx_pole_from_time_ms(ATTACK[sp], g_sr), r = omx_pole_from_time_ms(RELEASE[sp], g_sr);
    ok(g_v.down.attack_coeff == a && g_v.up.attack_coeff == a && g_v.down.release_coeff == r && g_v.up.release_coeff == r,
       "each speed's attack and release are its declared milliseconds", g_v.down.attack_coeff, a);
  }
  clean();
}

/* ---- E: hostile words ------------------------------------------------------------------------ */
static int same_as(float a0, int d0, int s0, float a1, int d1, int s1, uint32_t n) {
  setup(a0, d0, s0);
  run(&g_v, g_l, g_r, g_ol, g_or, n);
  setup(a1, d1, s1);
  run(&g_v, g_l, g_r, g_xl, g_xr, n);
  return memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0;
}
static void arm_hostile(void) {
  g_arm = "E";
  const uint32_t n = (uint32_t)(g_sr * 0.1f);
  stimulus(g_l, g_r, n, 0xbad1u, 2000u);
  g_counting = 1;
  g_allocs = 0;
  ok(same_as(1e9f, 2, 1, 100.0f, 2, 1, n), "an amount past the travel is 100 %", 0, 0);
  ok(same_as(-5.0f, 2, 1, 0.0f, 2, 1, n), "an amount under the travel is 0", 0, 0);
  ok(same_as(NAN, 2, 1, 0.0f, 2, 1, n), "a non-finite amount is the default, 0", 0, 0);
  ok(same_as(60.0f, -1, 1, 60.0f, 2, 1, n) && same_as(60.0f, 7, 1, 60.0f, 2, 1, n), "a direction outside the set is both", 0, 0);
  ok(same_as(60.0f, 2, -3, 60.0f, 2, 1, n) && same_as(60.0f, 2, 9, 60.0f, 2, 1, n), "a speed outside the set is medium", 0, 0);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0.0);
  OmxLevelerInstance bad;
  ok(omx_leveler_instance_init(&bad, 22050.0f) == 0, "a rate outside the oracle set is refused", 0, 0);
  drain();
  run(&bad, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "a refused instance is the input", 0, 0);
  ok(omx_leveler_instance_latency(&g_v) == 0u, "no latency", 0, 0);
  clean();
}

/* ---- F: blocks ------------------------------------------------------------------------------- */
static void arm_blocks(void) {
  g_arm = "F";
  const uint32_t n = (uint32_t)(g_sr * 0.3f);
  stimulus(g_l, g_r, n, 0xb10cu, 2500u);
  setup(70.0f, 2, 2);
  omx_leveler_instance_run(&g_v, g_l, g_r, g_ol, g_or, n);
  drain();
  setup(70.0f, 2, 2);
  uint32_t lcg = 99u;
  for (uint32_t o = 0; o < n;) {
    lcg = lcg * 1664525u + 1013904223u;
    uint32_t b = 1u + (lcg >> 8) % 4096u;
    if (b > n - o) b = n - o;
    omx_leveler_instance_run(&g_v, g_l + o, g_r + o, g_xl + o, g_xr + o, b);
    drain();
    o += b;
  }
  ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0,
     "random blocks of 1 to 4096 frames are one block, bit for bit", 0, 0);
  clean();
}

int main(void) {
  (void)sine;
  (void)expect_clean;
  (void)rms;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (unsigned ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_identity();
    arm_table();
    arm_law();
    arm_speed();
    arm_hostile();
    arm_blocks();
  }
  return instance_oracle_end("leveler_instance");
}
