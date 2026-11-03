// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The gain computer's branch-free lanes raise no floating-point exception the per-sample functions do not. A lane
// evaluates every arm for every sample, also where the per-sample function never would, and selects the arm it
// needs. Two ways that went wrong: at a knee shift of exactly +knee/2 (a threshold below the knee range, moved whole
// onto it by omx_gate_knee) the off-centre knee's denominator is 0 below the threshold; and a compiler that
// vectorises `disc > 0 ? disc : 0` followed by sqrtf as a select after the square root takes the square root of a
// negative number outside the knee (GCC 13). The values are selected away; the sticky flags are not, and a host
// running with FE_INVALID or FE_DIVBYZERO trapping would stop in the gate. Three arms, release flags (the lanes
// are the kernels' loops when the build has libmvec):
//   LANE  omx_gaincomp_gain_shifted_lane, one call at a time, over a level sweep at shift -half, 0 < shift < half
//         and +half: FE_INVALID and FE_DIVBYZERO stay clear, and every value is omx_gaincomp_gain_shifted's, bit
//         for bit;
//   LOOP  the same sweep through vectorised loops (OMX_FM_SIMD) of the shifted lane at those shifts and of the
//         centred lane (omx_gaincomp_gain_lane) in ABOVE, BELOW and UPWARD at a hard and at soft knees: the flags
//         stay clear;
//   GATE  the gate instance with its threshold below, above and inside the knee range, centred in it, and with a
//         hard knee, over noise that crosses the range, at every oracle rate: the flags stay clear.
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/fx/omx_gate_instance.h>

#include "fx_rates.h"

#define FE_BAD (FE_INVALID | FE_DIVBYZERO)

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
}

static const char *flags_name(int f) {
  if ((f & FE_INVALID) && (f & FE_DIVBYZERO)) return "FE_INVALID|FE_DIVBYZERO";
  if (f & FE_INVALID) return "FE_INVALID";
  if (f & FE_DIVBYZERO) return "FE_DIVBYZERO";
  return "none";
}

/* One call each, out of line: the comparison is of the two functions, not of what a vectoriser makes of a loop. */
__attribute__((noinline)) static float lane(const struct omx_gaincomp_params *p, float shift, float level) {
  return omx_gaincomp_gain_shifted_lane(p, shift, level);
}
__attribute__((noinline)) static float scalar(const struct omx_gaincomp_params *p, float shift, float level) {
  return omx_gaincomp_gain_shifted(p, shift, level);
}

static void arm_lane(void) {
  g_arm = "LANE";
  static const float knees[] = {6.0f, 16.0f, 0.5f};
  static const float where[] = {-0.5f, 0.25f, 0.5f}; /* the shift, in knee widths: -half, inside, +half */
  for (unsigned k = 0; k < sizeof knees / sizeof knees[0]; k++)
    for (unsigned w = 0; w < sizeof where / sizeof where[0]; w++) {
      const struct omx_gaincomp_params p = {OMX_DYN_BELOW, -40.0f, 4.0f, knees[k], -80.0f, 1.0f};
      const float shift = where[w] * knees[k];
      int mismatches = 0, flags = 0;
      for (int i = 0; i <= 4000; i++) {
        const float level = powf(10.0f, (-200.0f + 0.05f * (float)i) / 20.0f); /* -200 .. 0 dBFS */
        feclearexcept(FE_ALL_EXCEPT);
        const float a = lane(&p, shift, level);
        flags |= fetestexcept(FE_BAD);
        const float b = scalar(&p, shift, level);
        if (memcmp(&a, &b, sizeof a) != 0) mismatches++;
      }
      char what[160];
      snprintf(what, sizeof what, "knee %g dB, shift %g dB: the lane raises %s", (double)knees[k], (double)shift,
               flags_name(flags));
      ok(flags == 0, what, (double)flags, 0.0);
      snprintf(what, sizeof what, "knee %g dB, shift %g dB: the lane's gain is the per-sample function's",
               (double)knees[k], (double)shift);
      ok(mismatches == 0, what, (double)mismatches, 0.0);
    }
}

#define NL 4001

/* visible outside the unit, so the compiler keeps the stores of a loop whose result nothing here reads */
float g_lev[NL], g_out[NL];

/* One block through a lane, the loop the kernels run; out of line so the flags read are this loop's. */
__attribute__((noinline)) static void loop_shifted(const struct omx_gaincomp_params *p, float shift) {
  const struct omx_gaincomp_params gc = *p;
  OMX_FM_SIMD
  for (int i = 0; i < NL; i++) g_out[i] = omx_gaincomp_gain_shifted_lane(&gc, shift, g_lev[i]);
}
__attribute__((noinline)) static void loop_centred(const struct omx_gaincomp_params *p) {
  const struct omx_gaincomp_params gc = *p;
  OMX_FM_SIMD
  for (int i = 0; i < NL; i++) g_out[i] = omx_gaincomp_gain_lane(&gc, g_lev[i]);
}

static void arm_loop(void) {
  g_arm = "LOOP";
  for (int i = 0; i < NL; i++) g_lev[i] = powf(10.0f, (-200.0f + 0.05f * (float)i) / 20.0f); /* -200 .. 0 dBFS */
  static const float knees[] = {6.0f, 16.0f, 0.5f};
  static const float where[] = {-0.5f, 0.25f, 0.5f};
  char what[160];
  for (unsigned k = 0; k < sizeof knees / sizeof knees[0]; k++)
    for (unsigned w = 0; w < sizeof where / sizeof where[0]; w++) {
      const struct omx_gaincomp_params p = {OMX_DYN_BELOW, -40.0f, 4.0f, knees[k], -80.0f, 1.0f};
      const float shift = where[w] * knees[k];
      feclearexcept(FE_ALL_EXCEPT);
      loop_shifted(&p, shift);
      const int flags = fetestexcept(FE_BAD);
      snprintf(what, sizeof what, "shifted lane, knee %g dB, shift %g dB: the loop raises %s", (double)knees[k],
               (double)shift, flags_name(flags));
      ok(flags == 0, what, (double)flags, 0.0);
    }
  static const int modes[] = {OMX_DYN_ABOVE, OMX_DYN_BELOW, OMX_DYN_UPWARD};
  static const float cknees[] = {0.0f, 0.5f, 6.0f, 16.0f};
  for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++)
    for (unsigned k = 0; k < sizeof cknees / sizeof cknees[0]; k++) {
      const float range = modes[m] == OMX_DYN_BELOW ? -80.0f : modes[m] == OMX_DYN_UPWARD ? 12.0f : 0.0f;
      const struct omx_gaincomp_params p = {modes[m], -40.0f, 4.0f, cknees[k], range, 1.0f};
      feclearexcept(FE_ALL_EXCEPT);
      loop_centred(&p);
      const int flags = fetestexcept(FE_BAD);
      snprintf(what, sizeof what, "centred lane, mode %d, knee %g dB: the loop raises %s", modes[m], (double)cknees[k],
               flags_name(flags));
      ok(flags == 0, what, (double)flags, 0.0);
    }
}

#define N 4096u

static uint32_t g_seed;
static float rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

static void arm_gate(float sr) {
  g_arm = "GATE";
  /* threshold, knee start, knee end: below the range (shift +half), above it (shift -half), inside it, centred
   * on it, and a hard knee (start == end) */
  static const float cases[][3] = {{-60.0f, -43.0f, -37.0f}, {-30.0f, -43.0f, -37.0f}, {-40.0f, -46.0f, -38.0f},
                                   {-40.0f, -44.0f, -36.0f}, {-40.0f, -40.0f, -40.0f}};
  static OmxGateInstance g;
  static float l[N], r[N], ol[N], or_[N];
  for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    if (!omx_gate_instance_init(&g, sr)) {
      ok(0, "the instance initialises", 0.0, 1.0);
      return;
    }
    omx_gate_instance_resolve(&g, 0, 0, cases[c][0], -80.0f, cases[c][1], cases[c][2], 1.0f, 0.0f, 50.0f, 0.0f, 4.0f,
                              0.0f, 0.0f, (int)OMX_GATE_KEY_LISTENS_DEFAULT,
                              (float)OMX_GATE_KEY_TILT_DB_RANGE_DEFAULT);
    g_seed = 2027u;
    for (uint32_t i = 0; i < N; i++) {
      /* a level sweep from about -100 dBFS to -6 dBFS and back: it crosses the knee range both ways */
      const float t = (float)i / (float)N, env = powf(10.0f, (-100.0f + 94.0f * (t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t)) / 20.0f);
      l[i] = env * rnd();
      r[i] = env * rnd();
    }
    int flags = 0;
    for (int pass = 0; pass < 4; pass++) {
      feclearexcept(FE_ALL_EXCEPT);
      omx_gate_instance_run(&g, NULL, l, r, ol, or_, N);
      flags |= fetestexcept(FE_BAD);
    }
    char what[160];
    snprintf(what, sizeof what, "%g Hz, threshold %g dB, knee %g..%g dB (shift %g dB): the gate raises %s",
             (double)sr, (double)cases[c][0], (double)cases[c][1], (double)cases[c][2], (double)g.atom.knee_shift_db,
             flags_name(flags));
    ok(flags == 0, what, (double)flags, 0.0);
  }
}

int main(void) {
  arm_lane();
  arm_loop();
  for (unsigned i = 0; i < OMX_FX_ORACLE_RATE_COUNT; i++) arm_gate(OMX_FX_ORACLE_RATES[i]);
  printf("gaincomp_fpflags: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
