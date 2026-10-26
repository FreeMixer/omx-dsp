// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The dynamics kernel's static law in both directions (omx_gaincomp.h, omx_dyn.h) at each of the nine
// oracle rates (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  the gain computer, written out here: ABOVE is `(1/R - 1)(L - T)` above the threshold and
//      UPWARD is `(1 - 1/R)(T - L)` below it, capped at the range, each with the quadratic knee across
//      `T +- W/2` (1e-4 dB over -90 to 0 dB, three thresholds, four ratios, two knees, four ranges);
//      UPWARD at `T + d` is ABOVE at `T - d` negated, bit for bit, where the cap does not reach; a
//      ratio of 1 or a range of 0 is unity, exactly;
//   B  the kernel on a held level, both directions, peak and RMS detectors: the settled gain is the
//      closed form of A plus the make-up (1e-3 dB), on the base path and on the 4x control path, where
//      the boost passes the make-up and must not be clipped to it;
//   C  the ledger is clean.
// ABOVE and BELOW slots are the kernel that shipped: the dynamics, comp and gate goldens hold them.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_dyn.h>

#include "instance_oracle.h"

/* The closed forms, in double. */
static double law_above(double t, double r, double w, double l) {
  const double x = l - t, s = 1.0 / r - 1.0;
  if (w > 0.0 && fabs(x) < 0.5 * w) return s * (x + 0.5 * w) * (x + 0.5 * w) / (2.0 * w);
  return x <= 0.0 ? 0.0 : s * x;
}
static double law_up(double t, double r, double w, double range, double l) {
  const double x = l - t, s = 1.0 - 1.0 / r;
  double g;
  if (w > 0.0 && fabs(x) < 0.5 * w) g = s * (0.5 * w - x) * (0.5 * w - x) / (2.0 * w);
  else g = x >= 0.0 ? 0.0 : -s * x;
  return g > range ? range : g;
}

static struct omx_gaincomp_params gc_of(int mode, float t, float r, float w, float range, float makeup_db) {
  struct omx_gaincomp_params p = {mode, t, r, w, range, omx_db_to_lin(makeup_db)};
  return p;
}

/* ---- A: the gain computer -------------------------------------------------------------------- */
static void arm_law(void) {
  g_arm = "A";
  static const float TS[] = {-40.0f, -24.0f, -12.0f};
  static const float RS[] = {1.0f, 1.5f, 3.0f, 10.0f};
  static const float WS[] = {0.0f, 6.0f};
  static const float RANGES[] = {0.0f, 6.0f, 12.0f, 24.0f};
  double worst = 0.0;
  int mirror = 1, unity = 1;
  for (int ti = 0; ti < 3; ti++)
    for (int ri = 0; ri < 4; ri++)
      for (int wi = 0; wi < 2; wi++)
        for (int gi = 0; gi < 4; gi++) {
          const struct omx_gaincomp_params up = gc_of(OMX_DYN_UPWARD, TS[ti], RS[ri], WS[wi], RANGES[gi], 0.0f);
          const struct omx_gaincomp_params dn = gc_of(OMX_DYN_ABOVE, TS[ti], RS[ri], WS[wi], 0.0f, 0.0f);
          for (float l = -90.0f; l <= 0.0f; l += 0.25f) {
            const double gu = omx_gaincomp_db(&up, l), ga = omx_gaincomp_db(&dn, l);
            const double eu = fabs(gu - law_up(TS[ti], RS[ri], WS[wi], RANGES[gi], l));
            const double ea = fabs(ga - law_above(TS[ti], RS[ri], WS[wi], l));
            worst = fmax(worst, fmax(eu, ea));
            ok(eu < 1e-4, "UPWARD is (1 - 1/R)(T - L) below the threshold, capped at the range (1e-4 dB)", gu,
               law_up(TS[ti], RS[ri], WS[wi], RANGES[gi], l));
            ok(ea < 1e-4, "ABOVE is (1/R - 1)(L - T) above the threshold (1e-4 dB)", ga, law_above(TS[ti], RS[ri], WS[wi], l));
            const float d = l - TS[ti];
            const struct omx_gaincomp_params wide = gc_of(OMX_DYN_UPWARD, TS[ti], RS[ri], WS[wi], 1e9f, 0.0f);
            mirror &= omx_gaincomp_db(&wide, TS[ti] + d) == -omx_gaincomp_db(&dn, TS[ti] - d);
            if (RS[ri] == 1.0f || RANGES[gi] == 0.0f) unity &= gu == 0.0;
          }
        }
  ok(mirror, "UPWARD at T + d is ABOVE at T - d negated, bit for bit, uncapped", mirror, 1);
  ok(unity, "a ratio of 1 or a range of 0 is unity, exactly", unity, 1);
  printf("  dyn_upward: the gain computer against the closed forms, worst %.2e dB\n", worst);
  expect_clean();
}

/* ---- B, C: the kernel on a held level -------------------------------------------------------- */
#define HOLD_MS 300.0f
#define MAXN 60000u
static float g_l[MAXN], g_r[MAXN];

static struct omx_dyn atom(int mode, float t, float r, float w, float range, float makeup_db, int detect, float att_ms) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc = gc_of(mode, t, r, w, range, makeup_db);
  d.detect = detect;
  d.ovs_mode = OMX_DYN_OVS_AUTO;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, g_sr);
  d.release_coeff = omx_pole_from_time_ms(20.0f, g_sr);
  return d;
}

/* The settled gain, dB, of a slot holding the level `l_db` on both legs. */
static double held(const struct omx_dyn *p, float l_db) {
  static struct omx_dyn_state st;
  omx_dyn_state_init(&st, 1u);
  const float x = omx_db_to_lin(l_db);
  const uint32_t n = (uint32_t)(HOLD_MS * 0.001f * g_sr), blk = 256u;
  float last = 0.0f;
  for (uint32_t o = 0; o < n; o += blk) {
    const uint32_t m = n - o < blk ? n - o : blk;
    for (uint32_t i = 0; i < m; i++) g_l[i] = g_r[i] = x;
    omx_dynamics(g_l, g_r, m, p, &st);
    last = g_l[m - 1u];
  }
  return 20.0 * log10((double)last / (double)x);
}

static void arm_kernel(void) {
  g_arm = "B";
  static const float LS[] = {-70.0f, -52.0f, -43.0f, -40.0f, -37.0f, -30.0f, -12.0f, -3.0f};
  double worst = 0.0;
  for (int det = 0; det < 2; det++)
    for (int path = 0; path < 2; path++) {
      const int detect = det ? OMX_DETECT_RMS : OMX_DETECT_PEAK;
      const float att = path ? 0.2f : 2.0f; /* below OMX_DYN_OVS_AUTO_MS the 4x control path runs */
      const struct omx_dyn up = atom(OMX_DYN_UPWARD, -40.0f, 3.0f, 6.0f, 12.0f, 2.0f, detect, att);
      const struct omx_dyn dn = atom(OMX_DYN_ABOVE, -40.0f, 3.0f, 6.0f, 0.0f, 2.0f, detect, att);
      for (int k = 0; k < 8; k++) {
        const double gu = held(&up, LS[k]), ga = held(&dn, LS[k]);
        const double wu = law_up(-40.0, 3.0, 6.0, 12.0, LS[k]) + 2.0, wa = law_above(-40.0, 3.0, 6.0, LS[k]) + 2.0;
        worst = fmax(worst, fmax(fabs(gu - wu), fabs(ga - wa)));
        ok(fabs(gu - wu) < 1e-3, path ? "UPWARD on a held level, 4x path: the closed form plus make-up (1e-3 dB)"
                                       : "UPWARD on a held level: the closed form plus make-up (1e-3 dB)",
           gu, wu);
        ok(fabs(ga - wa) < 1e-3, path ? "ABOVE on a held level, 4x path: the closed form plus make-up (1e-3 dB)"
                                       : "ABOVE on a held level: the closed form plus make-up (1e-3 dB)",
           ga, wa);
      }
    }
  printf("  dyn_upward @ %.0f Hz: the kernel on held levels, worst %.2e dB\n", (double)g_sr, worst);
  expect_clean();
}

int main(void) {
  (void)stimulus;
  (void)sine;
  (void)rms;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  g_sr = 48000.0f;
  arm_law();
  for (unsigned ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_kernel();
  }
  return instance_oracle_end("dyn_upward");
}
