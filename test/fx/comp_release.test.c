// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The compressor's release modes and effective attack (omx_comp_instance.h, omx_dyn.h) at each of the
// nine oracle rates (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  opto and auto are the program release run on the gain computer's targets: a fixed compressor
//      whose release is its attack (5 ms) gives the targets on held levels, omx_env_program_release run by
//      hand over them gives the gain, and the face's gain is that (1e-3 dB);
//   B  the release's closed form end to end: a level 20 dB over the threshold held 0.2 s and 3 s, then
//      removed, recovers half its reduction at the t solving (1 - s) e^(-t/fast) + s e^(-t/slow(m)) =
//      1/2, with the declared profile's fastMs, share and slowMs(m) = slowMinMs + m (slowMaxMs -
//      slowMinMs), m the memory the hold charged, and 99 % at the t solving the same for 1/100 after
//      the short hold (10 %: the detector sits in series), opto and auto, on the base path and the 4x
//      control path;
//   C  in opto and auto releaseMs is inert, bit for bit; the modes differ from fixed;
//   D  effectiveAttackMs is attackMs feed-forward, and in feedback the time constant of the loop's floor
//      (R - 1)/R, -1000/(rate ln((R - 1)/R)), where it is slower; the bus compressor design's table at
//      the four floor rates (1e-4 ms);
//   E  no gain is added: the applied gain never passes the make-up;
//   F  a release mode outside the set is fixed, bit for bit; the ledger is clean.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_comp_instance.h>

#include "instance_oracle.h"

/* The declarations, stated here so a moved one turns its arm red (comp_release-perturb.sh). */
static const double PROFILE[2][4] = {{70, 110, 1100, 0.5}, {100, 300, 1200, 0.5}}; /* opto, auto */
#define CHARGE_MS 1000.0

#define KEY_IDLE                                                                                       \
  (int)OMX_COMP_KEY_SOURCES_DEFAULT, (float)OMX_COMP_KEY_HP_HZ_RANGE_DEFAULT,                          \
      (float)OMX_COMP_KEY_LP_HZ_RANGE_DEFAULT, (float)OMX_COMP_KEY_TILT_DB_RANGE_DEFAULT,              \
      (int)OMX_COMP_KEY_LISTENS_DEFAULT, (float)OMX_COMP_HOLD_MS_RANGE_DEFAULT,                        \
      (int)OMX_COMP_DETECT_LINKS_DEFAULT, (int)OMX_COMP_DUCKS_DEFAULT, (float)OMX_COMP_RANGE_DB_RANGE_DEFAULT

#define MAXN 800000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxCompInstance g_c, g_f;

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static void comp(OmxCompInstance *c, float thr, float ratio, float att, float rel, float makeup, int ovs, int topo, int mode) {
  omx_comp_instance_init(c, g_sr);
  drain();
  omx_comp_instance_resolve(c, 0, thr, ratio, 0.0f, att, rel, makeup, 100.0f, (int)OMX_COMP_KINDS_LIMITER, ovs, topo, KEY_IDLE,
                            mode);
  drain();
}
/* Blocks of 8 frames: at 32, 64 and 128 kHz the program release makes its slow pole every sample, four
 * a frame on the 4x control path, and records an explained `rate-is-declared` each time; the ledger
 * keeps OMX_CONTRACT_MAX (64). */
static void run(OmxCompInstance *c, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 8u) {
    omx_comp_instance_run(c, il + o, ir + o, ol + o, orr + o, n - o < 8u ? n - o : 8u);
    drain();
  }
}

/* Held levels, dBFS, each for its seconds, on both legs. */
static uint32_t levels(const float *db, const float *secs, int k) {
  uint32_t n = 0;
  for (int j = 0; j < k; j++) {
    const float x = omx_db_to_lin(db[j]);
    const uint32_t m = (uint32_t)(secs[j] * g_sr);
    for (uint32_t i = 0; i < m && n < MAXN; i++, n++) g_l[n] = g_r[n] = x;
  }
  return n;
}

/* ---- A: the word over the computer's targets ------------------------------------------------- */
static void arm_word(void) {
  g_arm = "A";
  static const float DB[] = {-6.0f, -40.0f, -10.0f, -50.0f, -20.0f};
  static const float SECS[] = {0.3f, 0.4f, 0.2f, 0.6f, 0.3f};
  const uint32_t n = levels(DB, SECS, 5);
  double worst = 0.0;
  for (int mode = 1; mode <= 2; mode++) {
    /* The reference's release is its attack, 5 ms, the release's travel floor. */
    comp(&g_f, -30.0f, 4.0f, 5.0f, 5.0f, 0.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, (int)OMX_COMP_RELEASE_MODES_FIXED);
    run(&g_f, g_l, g_r, g_xl, g_xr, n);
    comp(&g_c, -30.0f, 4.0f, 5.0f, 500.0f, 0.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, mode);
    run(&g_c, g_l, g_r, g_ol, g_or, n);
    struct omx_env_program_release prs = {0.0f, 0.0f, 0.0f};
    struct omx_env_program_release_params prp;
    if (mode == 1) omx_env_program_release_opto(&prp, g_sr, 1u);
    else omx_env_program_release_auto(&prp, g_sr, 1u);
    drain();
    double e = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      float t = omx_lin_to_db(g_xl[i] / g_l[i]);
      t = t < 0.0f ? t : 0.0f;
      const double want = omx_env_program_release(&prs, &prp, t);
      const double got = 20.0 * log10((double)g_ol[i] / (double)g_l[i]);
      e = fmax(e, fabs(got - want));
      drain();
    }
    drain();
    worst = fmax(worst, e);
    ok(e < 1e-3, mode == 1 ? "opto is the program release over the computer's targets (1e-3 dB)"
                           : "auto is the program release over the computer's targets (1e-3 dB)",
       e, 1e-3);
  }
  printf("  comp_release @ %.0f Hz: the word over the targets, worst %.2e dB\n", (double)g_sr, worst);
  clean();
}

/* ---- B: the closed form end to end ----------------------------------------------------------- */
/* The t, ms, at which (1 - s) e^(-t/f) + s e^(-t/w) falls to `frac`, by bisection. */
static double solve(double f, double w, double s, double frac) {
  double lo = 0.0, hi = 60000.0;
  for (int k = 0; k < 200; k++) {
    const double mid = 0.5 * (lo + hi);
    if ((1.0 - s) * exp(-mid / f) + s * exp(-mid / w) > frac) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

static void recovery(int mode, int ovs, float hold_s, int full) {
  const float DB[] = {-10.0f, -90.0f};
  const float SECS[] = {hold_s, full ? 1.4f : 2.6f};
  const uint32_t n = levels(DB, SECS, 2), at = (uint32_t)(hold_s * g_sr);
  comp(&g_c, -30.0f, 4.0f, ovs ? 0.2f : 2.0f, 100.0f, 0.0f, ovs ? OMX_DYN_OVS_X4 : OMX_DYN_OVS_OFF,
       OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, mode);
  run(&g_c, g_l, g_r, g_ol, g_or, n);
  const uint32_t lat = ovs ? OMX_OVS_LATENCY_4X : 0u;
  const double g0 = 20.0 * log10((double)g_ol[at - 1u] / (double)g_l[at - 1u]);
  const double *pr = PROFILE[mode - 1];
  const double m = 1.0 - exp(-(double)hold_s * 1000.0 / CHARGE_MS);
  const double slow = pr[1] + m * (pr[2] - pr[1]);
  double t50 = -1.0, t99 = -1.0;
  for (uint32_t i = at + lat; i < n; i++) {
    const double r = 20.0 * log10((double)g_ol[i] / (double)g_l[i]);
    if (t50 < 0.0 && r >= 0.5 * g0) t50 = (double)(i - at - lat) * 1000.0 / g_sr;
    if (t99 < 0.0 && r >= 0.01 * g0) t99 = (double)(i - at - lat) * 1000.0 / g_sr;
  }
  const double w50 = solve(pr[0], slow, pr[3], 0.5), w99 = solve(pr[0], slow, pr[3], 0.01);
  char what[160];
  snprintf(what, sizeof what, "%s, %s, a %.1f s hold: half the reduction back at the closed form's t (10 %%)",
           mode == 1 ? "opto" : "auto", ovs ? "4x" : "base", (double)hold_s);
  ok(fabs(t50 / w50 - 1.0) < 0.1, what, t50, w50);
  if (!full) {
    snprintf(what, sizeof what, "%s, %s, a %.1f s hold: 99 %% back at the closed form's t (10 %%)", mode == 1 ? "opto" : "auto",
             ovs ? "4x" : "base", (double)hold_s);
    ok(fabs(t99 / w99 - 1.0) < 0.1, what, t99, w99);
  }
  ok(g0 < -14.0 && g0 > -16.0, "the held reduction is the law's, (1/4 - 1) 20 dB", g0, -15.0);
  printf("  comp_release @ %.0f Hz: %s %s hold %.1f s: 50 %% at %.1f ms (closed form %.1f), 99 %% at %.1f ms (%.1f)\n",
         (double)g_sr, mode == 1 ? "opto" : "auto", ovs ? "4x" : "base", (double)hold_s, t50, w50, t99, w99);
}
static void arm_closed(void) {
  g_arm = "B";
  for (int mode = 1; mode <= 2; mode++)
    for (int ovs = 0; ovs < 2; ovs++) {
      recovery(mode, ovs, 0.2f, 0);
      recovery(mode, ovs, 3.0f, 1);
    }
  clean();
}

/* ---- C: releaseMs inert; E: no gain added; F: a mode outside the set ------------------------- */
static void arm_inert(void) {
  g_arm = "C";
  static const float DB[] = {-6.0f, -45.0f, -12.0f, -60.0f};
  static const float SECS[] = {0.2f, 0.3f, 0.2f, 0.3f};
  const uint32_t n = levels(DB, SECS, 4);
  for (int mode = 1; mode <= 2; mode++) {
    comp(&g_c, -30.0f, 4.0f, 2.0f, 30.0f, 3.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, mode);
    run(&g_c, g_l, g_r, g_ol, g_or, n);
    comp(&g_c, -30.0f, 4.0f, 2.0f, 900.0f, 3.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, mode);
    run(&g_c, g_l, g_r, g_xl, g_xr, n);
    ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0, "releaseMs is inert in opto and auto, bit for bit", 0, 0);
    g_arm = "E";
    int under = 1;
    for (uint32_t i = 0; i < n; i++) under &= g_ol[i] <= g_l[i] * omx_db_to_lin(3.0f);
    ok(under, "no gain is added: the applied gain never passes the make-up", under, 1);
    g_arm = "C";
    comp(&g_c, -30.0f, 4.0f, 2.0f, 30.0f, 3.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, (int)OMX_COMP_RELEASE_MODES_FIXED);
    run(&g_c, g_l, g_r, g_xl, g_xr, n);
    ok(memcmp(g_ol, g_xl, n * sizeof(float)) != 0, "a program release is not the fixed one", 0, 0);
  }
  clean();
  g_arm = "F";
  comp(&g_c, -30.0f, 4.0f, 2.0f, 30.0f, 3.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, (int)OMX_COMP_RELEASE_MODES_FIXED);
  run(&g_c, g_l, g_r, g_ol, g_or, n);
  int same = 1;
  for (int bad = -1; bad <= 7; bad += 8) {
    comp(&g_c, -30.0f, 4.0f, 2.0f, 30.0f, 3.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, bad);
    run(&g_c, g_l, g_r, g_xl, g_xr, n);
    same &= memcmp(g_ol, g_xl, n * sizeof(float)) == 0;
  }
  ok(same, "a release mode outside the set is fixed, bit for bit", same, 1);
  clean();
}

/* ---- D: the effective attack ----------------------------------------------------------------- */
static void arm_attack(void) {
  g_arm = "D";
  static const float RS[] = {2.0f, 4.0f, 10.0f, 20.0f};
  /* The bus compressor design's table of the floor, ms, at the four floor rates (44.1, 48, 96, 192 kHz). */
  static const float TAB_RATES[OMX_ORACLE_FLOOR_RATES_COUNT] = OMX_ORACLE_FLOOR_RATES_INIT;
  _Static_assert(OMX_ORACLE_FLOOR_RATES_COUNT == 4u, "the table's four columns");
  static const double TAB[4][4] = {{0.0327, 0.0301, 0.0150, 0.0075},
                                   {0.0788, 0.0724, 0.0362, 0.0181},
                                   {0.2152, 0.1977, 0.0989, 0.0494},
                                   {0.4421, 0.4062, 0.2031, 0.1015}};
  for (int k = 0; k < 4; k++) {
    comp(&g_c, -30.0f, RS[k], 0.1f, 100.0f, 0.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, 0);
    ok(omx_comp_instance_effective_attack_ms(&g_c) == g_c.atom.attack_ms, "feed-forward: effectiveAttackMs is attackMs",
       omx_comp_instance_effective_attack_ms(&g_c), g_c.atom.attack_ms);
    comp(&g_c, -30.0f, RS[k], 0.1f, 100.0f, 0.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDBACK, 0);
    const double floor_ms = -1000.0 / ((double)g_sr * log((RS[k] - 1.0) / RS[k]));
    const double want = floor_ms > 0.1 ? floor_ms : 0.1;
    const double got = omx_comp_instance_effective_attack_ms(&g_c);
    ok(fabs(got - want) < 1e-4 * want + 1e-6, "feedback: effectiveAttackMs is the floor's time constant where it is slower",
       got, want);
    for (int ti = 0; ti < 4; ti++)
      if (TAB_RATES[ti] == g_sr && TAB[k][ti] > 0.1)
        ok(fabs(got - TAB[k][ti]) < 1e-4, "the design's table of the floor", got, TAB[k][ti]);
    /* a slow attack never reaches the floor */
    comp(&g_c, -30.0f, RS[k], 20.0f, 100.0f, 0.0f, OMX_DYN_OVS_OFF, OMX_DETECTOR_TOPOLOGY_FEEDBACK, 0);
    ok(omx_comp_instance_effective_attack_ms(&g_c) == 20.0f, "feedback above the floor: effectiveAttackMs is attackMs",
       omx_comp_instance_effective_attack_ms(&g_c), 20.0);
  }
  clean();
}

int main(void) {
  (void)stimulus;
  (void)sine;
  (void)rms;
  (void)expect_clean;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (unsigned ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_word();
    arm_closed();
    arm_inert();
    arm_attack();
  }
  return instance_oracle_end("comp_release");
}
