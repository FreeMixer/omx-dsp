// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The sub-octaver's instance oracle (omx_suboct_instance.h, the FX-17 design's oracle table) at each of
// the nine oracle rates (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  each range, a sine at its middle fundamental: the first octave's lines are the divider's series
//      times the designed sections, A_t 8/(3 pi) |LP24(f/2)| at f/2 and A_t 8/(5 pi) |LP24(3f/2)| at
//      3f/2, A_t the sine through the tracking band (0.05 and 0.1 dB, a Hann-windowed DTFT over the
//      last second), and the line at f stays 60 dB under the sine;
//   B  the second octave: its line at f/4 leads every other line and is the series applied to the first
//      octave's f/2 line through post2's low-pass within 1 dB (the first octave's own 3f/2 line moves
//      the second divider's crossings; the error is printed);
//   C  110 Hz with uniform noise 20 dB under its peak: one toggle per input period over 1.8 s, and
//      trackedHz's period is the tone's within 2/44100 s and a sample (noise moves each toggle's sample,
//      never its count, L3; the spec's arm D holds a toggle's time to a sample of 44.1 kHz);
//   D  the same second of tone toggles the same number of times at every rate;
//   E  560 Hz at 0 dBFS through the high range: no output step passes twice the input's largest (L4);
//   F  the level is bounded: over band-limited noise, RMS(y) <= (dry + oct1 + oct2)/100 RMS(x)
//      (+0.05 dB) at the corners of the three levels, each range;
//   G  a note decaying through trackGateDb: the octaves fade out within SUBOCT_GATE_RAMP_MS and one
//      sample of the envelope crossing it, the output is then the dry alone, and trackedHz reads 0;
//   H  oct1 = oct2 = 0 and dry = 100, or bypassed, is the input bit for bit; random blocks are one
//      block; identical legs stay identical; the latency is 0;
//   I  the declarations read back: the range table's corners, the envelope's times, the gate's ramp,
//      the arming level SUBOCT_HYSTERESIS e, the travels' ends and defaults; nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_suboct_instance.h>

#include "instance_oracle.h"

#include <complex.h>
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

/* The declarations, stated here so a moved one turns its arm red (suboct_instance-perturb.sh). */
static const double LO[3] = {35, 70, 140}, HI[3] = {140, 280, 560}, POST1[3] = {90, 180, 360}, POST2[3] = {45, 90, 180};
#define HYST 0.25
#define ENV_ATTACK_MS 1.0f
#define ENV_RELEASE_MS 100.0f
#define RAMP_MS 5.0
static const float DEFAULTS[5] = {0, 100, 0, 100, -50};

#define MAXN 800000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxSuboctInstance g_s;
static uint32_t g_toggles[OMX_FX_ORACLE_RATE_COUNT];
static unsigned g_ri;

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static void setup(int range, float o1, float o2, float dry, float gate) {
  omx_suboct_instance_init(&g_s, g_sr);
  drain();
  omx_suboct_instance_resolve(&g_s, 0, range, o1, o2, dry, gate);
  drain();
}
static void run(OmxSuboctInstance *s, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) {
    omx_suboct_instance_run(s, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
    drain();
  }
}
static void tone(float *x, uint32_t n, double f, double a) {
  for (uint32_t i = 0; i < n; i++) x[i] = (float)(a * sin(2.0 * M_PI * f * i / g_sr));
}
/* A section's |H| at f, from its coefficients. */
static double mag(const double c[5], double f) {
  const double complex z = cexp(-I * 2.0 * M_PI * f / g_sr);
  return cabs((c[0] + c[1] * z + c[2] * z * z) / (1.0 + c[3] * z + c[4] * z * z));
}
/* A line's amplitude at f over x[from, from + len), Hann-windowed. */
static double line(const float *x, uint32_t from, uint32_t len, double f) {
  double complex h = 0.0;
  double wsum = 0.0;
  for (uint32_t i = 0; i < len; i++) {
    const double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / len);
    h += w * x[from + i] * cexp(-I * 2.0 * M_PI * f * (from + i) / g_sr);
    wsum += w;
  }
  return 2.0 * cabs(h) / wsum;
}
static double db(double x) { return 20.0 * log10(x); }

/* ---- A, B: the lines --------------------------------------------------------------------------- */
static void arm_lines(void) {
  static const double F[3] = {70.0, 140.0, 280.0};
  const uint32_t n = (uint32_t)(2.0 * g_sr), from = (uint32_t)g_sr, len = (uint32_t)g_sr;
  for (int k = 0; k < 3; k++) {
    g_arm = "A";
    const double f = F[k], a = 0.5;
    tone(g_l, n, f, a);
    memcpy(g_r, g_l, n * sizeof(float));
    setup(k, 100.0f, 0.0f, 0.0f, -80.0f);
    run(&g_s, g_l, g_r, g_ol, g_or, n);
    const double at = a * mag(g_s.hp, f) * mag(g_s.lp, f);
    const double l1 = line(g_ol, from, len, 0.5 * f), w1 = at * 8.0 / (3.0 * M_PI) * mag(g_s.p1[0], 0.5 * f) * mag(g_s.p1[1], 0.5 * f);
    const double l3 = line(g_ol, from, len, 1.5 * f), w3 = at * 8.0 / (5.0 * M_PI) * mag(g_s.p1[0], 1.5 * f) * mag(g_s.p1[1], 1.5 * f);
    const double lf = line(g_ol, from, len, f);
    ok(fabs(db(l1 / w1)) < 0.05, "the f/2 line is A_t 8/(3 pi) |LP24| (0.05 dB)", db(l1 / w1), 0.0);
    ok(fabs(db(l3 / w3)) < 0.1, "the 3f/2 line is A_t 8/(5 pi) |LP24| (0.1 dB)", db(l3 / w3), 0.0);
    ok(db(lf / a) < -60.0, "the line at f stays 60 dB under the sine", db(lf / a), -60.0);
    printf("  suboct @ %.0f Hz range %d: f/2 %+.4f dB, 3f/2 %+.4f dB, f %.1f dB\n", (double)g_sr, k, db(l1 / w1), db(l3 / w3), db(lf / a));
    g_arm = "B";
    setup(k, 0.0f, 100.0f, 0.0f, -80.0f);
    run(&g_s, g_l, g_r, g_ol, g_or, n);
    const double l4 = line(g_ol, from, len, 0.25 * f);
    const double w4 = l1 * 8.0 / (3.0 * M_PI) * mag(g_s.p2[0], 0.25 * f) * mag(g_s.p2[1], 0.25 * f);
    int leads = 1;
    for (int j = 1; j < 8; j++) leads &= line(g_ol, from, len, (2 * j + 1) * 0.25 * f) < l4;
    ok(leads, "the second octave's f/4 line leads every other line", leads, 1);
    ok(fabs(db(l4 / w4)) < 1.0, "the f/4 line is the series on the first octave's f/2 line through post2 (1 dB)", db(l4 / w4), 0.0);
    printf("  suboct @ %.0f Hz range %d: f/4 %+.3f dB against the closed form\n", (double)g_sr, k, db(l4 / w4));
  }
  clean();
}

/* ---- C, D: the toggles ------------------------------------------------------------------------- */
static void arm_toggles(void) {
  g_arm = "C";
  const double f = 110.0;
  const uint32_t n = (uint32_t)(2.0 * g_sr), from = (uint32_t)(0.2 * g_sr);
  tone(g_l, n, f, 0.5);
  uint32_t lcg = 0x5eedu;
  for (uint32_t i = 0; i < n; i++) {
    lcg = lcg * 1664525u + 1013904223u;
    g_l[i] += (float)(0.05 * ((double)(lcg >> 8) / 8388608.0 - 1.0)); /* uniform, 20 dB under the peak */
  }
  memcpy(g_r, g_l, n * sizeof(float));
  setup(1, 100.0f, 0.0f, 0.0f, -80.0f);
  run(&g_s, g_l, g_r, g_ol, g_or, from);
  const uint32_t t0 = g_s.d1.toggles;
  run(&g_s, g_l + from, g_r + from, g_ol + from, g_or + from, n - from);
  const uint32_t count = g_s.d1.toggles - t0, want = (uint32_t)lround(f * (n - from) / g_sr);
  ok(count + 1u >= want && count <= want + 1u, "one toggle per input period under noise 20 dB down", count, want);
  const double th = omx_suboct_instance_tracked_hz(&g_s);
  ok(fabs(1.0 / th - 1.0 / f) <= 2.0 / 44100.0 + 1.0 / g_sr, "trackedHz's period is the tone's within 2/44100 s and a sample",
     th, f);
  clean();

  g_arm = "D";
  tone(g_l, (uint32_t)g_sr, f, 0.5);
  memcpy(g_r, g_l, (uint32_t)g_sr * sizeof(float));
  setup(1, 100.0f, 0.0f, 0.0f, -80.0f);
  run(&g_s, g_l, g_r, g_ol, g_or, (uint32_t)g_sr);
  g_toggles[g_ri] = g_s.d1.toggles;
  ok(g_s.d1.toggles == g_toggles[0], "the same second of tone toggles as often at every rate", g_s.d1.toggles, g_toggles[0]);
  clean();
}

/* ---- E: no step; F: the level -------------------------------------------------------------- */
static void arm_bounds(void) {
  g_arm = "E";
  const uint32_t n = (uint32_t)(0.5 * g_sr);
  tone(g_l, n, 560.0, 1.0);
  memcpy(g_r, g_l, n * sizeof(float));
  setup(2, 100.0f, 0.0f, 0.0f, -80.0f);
  run(&g_s, g_l, g_r, g_ol, g_or, n);
  double din = 0.0, dout = 0.0;
  for (uint32_t i = 1; i < n; i++) din = fmax(din, fabs(g_l[i] - g_l[i - 1])), dout = fmax(dout, fabs(g_ol[i] - g_ol[i - 1]));
  ok(dout <= 2.0 * din, "no output step passes twice the input's largest", dout, 2.0 * din);
  clean();

  g_arm = "F";
  const uint32_t m = (uint32_t)(1.0 * g_sr);
  for (int k = 0; k < 3; k++) {
    double c[5], st[4] = {0, 0, 0, 0};
    omx_eq_design(OMX_EQ_BANDPASS, sqrt(LO[k] * HI[k]), 0.7, 0.0, g_sr, c);
    uint32_t lcg = 77u + (uint32_t)k;
    for (uint32_t i = 0; i < m; i++) {
      lcg = lcg * 1664525u + 1013904223u;
      const double v = (double)(lcg >> 8) / 8388608.0 - 1.0;
      const double y = c[0] * v + c[1] * st[0] + c[2] * st[1] - c[3] * st[2] - c[4] * st[3];
      st[1] = st[0], st[0] = v, st[3] = st[2], st[2] = y;
      g_l[i] = g_r[i] = (float)(0.5 * y);
    }
    const double rin = rms(g_l, 0, m);
    int under = 1;
    double worst = -1e9;
    for (int corner = 1; corner < 8; corner++) {
      const float o1 = corner & 1 ? 100.0f : 0.0f, o2 = corner & 2 ? 100.0f : 0.0f, dr = corner & 4 ? 100.0f : 0.0f;
      setup(k, o1, o2, dr, -80.0f);
      run(&g_s, g_l, g_r, g_ol, g_or, m);
      const double bound = (o1 + o2 + dr) / 100.0 * rin, e = db(rms(g_ol, 0, m) / bound);
      worst = fmax(worst, e);
      under &= e <= 0.05;
    }
    ok(under, "RMS(y) <= (dry + oct1 + oct2)/100 RMS(x) (+0.05 dB)", worst, 0.05);
  }
  clean();
}

/* ---- G: the gate ------------------------------------------------------------------------------- */
static void arm_gate(void) {
  g_arm = "G";
  const uint32_t n = (uint32_t)(2.0 * g_sr);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(0.03 * exp(-(double)i / (0.25 * g_sr)) * sin(2.0 * M_PI * 70.0 * i / g_sr));
  setup(0, 100.0f, 50.0f, 60.0f, -50.0f);
  const float gl = omx_db_to_lin(-50.0f);
  int64_t cross = -1, gone = -1;
  int dry = 1;
  for (uint32_t i = 0; i < n; i++) {
    omx_suboct_instance_run(&g_s, g_l + i, g_r + i, g_ol + i, g_or + i, 1u);
    if ((i & 31u) == 31u) drain();
    const float e = omx_env_level(&g_s.e0, &g_s.ep);
    if (cross < 0 && i > g_sr * 0.1 && e < gl) cross = i;
    if (cross >= 0 && gone < 0 && g_s.g == 0.0f) gone = i;
    if (gone >= 0) dry &= g_ol[i] == g_s.dr * g_l[i] && omx_suboct_instance_tracked_hz(&g_s) == 0.0f;
  }
  ok(cross > 0 && gone >= cross && (double)(gone - cross) <= RAMP_MS * 0.001 * g_sr + 1.0,
     "the octaves fade out within SUBOCT_GATE_RAMP_MS and a sample of the crossing", (double)(gone - cross), RAMP_MS * 0.001 * g_sr);
  ok(dry, "once gated the output is the dry alone and trackedHz reads 0", dry, 1);
  clean();
}

/* ---- H: identities; I: readback ------------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "H";
  const uint32_t n = (uint32_t)(0.3 * g_sr);
  stimulus(g_l, g_r, n, 0x3c3cu, 1200u);
  setup(1, 0.0f, 0.0f, 100.0f, -80.0f);
  run(&g_s, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0,
     "both octaves 0 and dry 100 are the input bit for bit", 0, 0);
  ok(g_s.d1.toggles > 0u, "with the divider still tracking", g_s.d1.toggles, 1);
  omx_suboct_instance_init(&g_s, g_sr);
  omx_suboct_instance_resolve(&g_s, 1, 0, 100.0f, 100.0f, 50.0f, -80.0f);
  drain();
  run(&g_s, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0, "bypassed is the input bit for bit", 0, 0);
  setup(1, 80.0f, 40.0f, 70.0f, -80.0f);
  omx_suboct_instance_run(&g_s, g_l, g_l, g_ol, g_or, n);
  drain();
  ok(memcmp(g_ol, g_or, n * sizeof(float)) == 0, "identical legs stay identical", 0, 0);
  setup(1, 80.0f, 40.0f, 70.0f, -80.0f);
  uint32_t lcg = 9u;
  for (uint32_t o = 0; o < n;) {
    lcg = lcg * 1664525u + 1013904223u;
    uint32_t b = 1u + (lcg >> 8) % 4096u;
    if (b > n - o) b = n - o;
    omx_suboct_instance_run(&g_s, g_l + o, g_l + o, g_xl + o, g_xr + o, b);
    drain();
    o += b;
  }
  ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0, "random blocks of 1 to 4096 frames are one block", 0, 0);
  ok(omx_suboct_instance_latency(&g_s) == 0u, "no latency", 0, 0);
  clean();

  g_arm = "I";
  g_counting = 1;
  g_allocs = 0;
  int table = 1;
  for (int k = 0; k < 3; k++) {
    setup(k, 100.0f, 0.0f, 100.0f, -50.0f);
    double hp[5], lp[5], p1[2][5], p2[2][5];
    omx_eq_design(OMX_EQ_HIGHPASS, 0.5 * LO[k], 0.70710678118654752, 0.0, g_sr, hp);
    omx_eq_design(OMX_EQ_LOWPASS, HI[k], 0.70710678118654752, 0.0, g_sr, lp);
    omx_suboct_lp24(p1, POST1[k], g_sr);
    omx_suboct_lp24(p2, POST2[k], g_sr);
    table &= memcmp(hp, g_s.hp, sizeof hp) == 0 && memcmp(lp, g_s.lp, sizeof lp) == 0 && memcmp(p1, g_s.p1, sizeof p1) == 0 &&
             memcmp(p2, g_s.p2, sizeof p2) == 0;
  }
  ok(table, "each range's corners are the declared table's", table, 1);
  ok(g_s.ep.attack_pole == omx_pole_from_time_ms(ENV_ATTACK_MS, g_sr) && g_s.ep.release_pole == omx_pole_from_time_ms(ENV_RELEASE_MS, g_sr),
     "the envelope's times are 1 and 100 ms", g_s.ep.attack_pole, 0.0);
  ok(fabs(g_s.ramp_step * RAMP_MS * 0.001 * g_sr - 1.0) < 1e-6, "the gate's ramp is 5 ms", g_s.ramp_step, 0.0);
  ok(omx_suboct_arm_level(1.0f) == (float)HYST, "the arming level is SUBOCT_HYSTERESIS of the peak", omx_suboct_arm_level(1.0f), HYST);
  setup(9, NAN, NAN, NAN, NAN);
  int ends = g_s.rng == (int)DEFAULTS[0] && g_s.o1 == DEFAULTS[1] * 0.01f && g_s.o2 == DEFAULTS[2] * 0.01f &&
             g_s.dr == DEFAULTS[3] * 0.01f && g_s.gate_db == DEFAULTS[4];
  setup(0, 1e9f, 1e9f, 1e9f, 1e9f);
  ends &= g_s.o1 == 1.0f && g_s.o2 == 1.0f && g_s.dr == 1.0f && g_s.gate_db == -20.0f;
  setup(0, -1e9f, -1e9f, -1e9f, -1e9f);
  ends &= g_s.o1 == 0.0f && g_s.o2 == 0.0f && g_s.dr == 0.0f && g_s.gate_db == -80.0f;
  ok(ends, "words outside land on the declared ends and defaults", ends, 1);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0.0);
  OmxSuboctInstance bad;
  ok(omx_suboct_instance_init(&bad, 22050.0f) == 0, "a rate outside the oracle set is refused", 0, 0);
  clean();
}

int main(void) {
  (void)sine;
  (void)expect_clean;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (g_ri = 0; g_ri < OMX_FX_ORACLE_RATE_COUNT; g_ri++) {
    g_sr = OMX_FX_ORACLE_RATES[g_ri];
    arm_lines();
    arm_toggles();
    arm_bounds();
    arm_gate();
    arm_identity();
  }
  return instance_oracle_end("suboct_instance");
}
