// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The transient designer's readouts (omx_transient_instance.h, FX-16) at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz), read one sample at a time:
//   A  on a held level both contrasts settle to nil, the followers within float rounding of the level:
//      each readout within its knob's share of the envelope word's rounding bound, |knob|/REF times
//      20 log10(1 + N 2^-24/(1 - q)), q the fast follower's slower per-stage pole (omx_envdiff_step's
//      `ordering-within-rounding`), and the gain within their sum of outputDb;
//   B  a level stepping up out of silence drives attackGainDb to attackDb exactly (the ramp saturates)
//      while sustainGainDb stays inside its bound; a step down to silence drives sustainGainDb to
//      sustainDb exactly while attackGainDb stays inside its bound; both signs of each knob;
//   C  every sample, attackGainDb + sustainGainDb + outputDb is the gain the stage applied, bit for
//      bit, and each readout lies inside its declared range (TRANSIENT_LIMITS' attackDb and sustainDb);
//   D  a refused instance reads 0; the ledger is clean.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_transient_instance.h>

#include "instance_oracle.h"

#define MAXN 200000u
static float g_l[MAXN], g_ol[MAXN], g_or[MAXN];
static float g_ra[MAXN], g_rs[MAXN], g_g[MAXN];
static OmxTransientInstance g_t;

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

/* A readout's bound where its contrast is nil: the knob's share of the envelope word's rounding bound. */
static double nil_bound(float knob) {
  const double q = fmax(g_t.atom.on.fast_attack, g_t.atom.on.fast_release);
  return fabs(knob) / OMX_TRANSIENT_REF_DB * 20.0 * log10(1.0 + OMX_DYN_ENV_STAGES * 0x1p-24 / (1.0 - q));
}

static void run_read(float att, float sus, float out, uint32_t n) {
  omx_transient_instance_init(&g_t, g_sr);
  omx_transient_instance_resolve(&g_t, 0, att, sus, 10.0f, 150.0f, out);
  drain();
  for (uint32_t i = 0; i < n; i++) {
    omx_transient_instance_run(&g_t, g_l + i, g_l + i, g_ol + i, g_or + i, 1u);
    g_ra[i] = omx_transient_instance_attack_gain_db(&g_t);
    g_rs[i] = omx_transient_instance_sustain_gain_db(&g_t);
    g_g[i] = g_t.state.gain_db;
    if ((i & 31u) == 31u) drain();
  }
  drain();
}

/* ---- A: a held level ------------------------------------------------------------------------- */
static void arm_held(void) {
  g_arm = "A";
  const uint32_t n = (uint32_t)(1.0f * g_sr);
  for (uint32_t i = 0; i < n; i++) g_l[i] = 0.25f;
  run_read(9.0f, -7.0f, 2.0f, n);
  int nil = 1;
  const double ba = nil_bound(9.0f), bs = nil_bound(-7.0f);
  for (uint32_t i = n / 2; i < n; i++) nil &= fabs(g_ra[i]) <= ba && fabs(g_rs[i]) <= bs && fabs(g_g[i] - 2.0) <= ba + bs;
  ok(nil, "on a held level both readouts are 0 and the gain is outputDb, within the rounding bound", nil, 1);
  clean();
}

/* ---- B: an onset and a cut-off --------------------------------------------------------------- */
static void arm_edges(void) {
  g_arm = "B";
  const uint32_t n = (uint32_t)(0.8f * g_sr), on = (uint32_t)(0.1f * g_sr), off = (uint32_t)(0.5f * g_sr);
  for (uint32_t i = 0; i < n; i++) g_l[i] = i >= on && i < off ? 0.5f : 0.0f;
  static const float KN[2][2] = {{9.0f, -7.0f}, {-12.0f, 5.0f}};
  for (int k = 0; k < 2; k++) {
    run_read(KN[k][0], KN[k][1], 0.0f, n);
    float ra_ext = 0.0f, rs_on = 0.0f, rs_ext = 0.0f, ra_off = 0.0f;
    for (uint32_t i = on; i < on + (uint32_t)(0.02f * g_sr); i++) {
      if (fabsf(g_ra[i]) > fabsf(ra_ext)) ra_ext = g_ra[i];
      rs_on = fmaxf(rs_on, fabsf(g_rs[i]));
    }
    for (uint32_t i = off; i < off + (uint32_t)(0.1f * g_sr); i++) {
      if (fabsf(g_rs[i]) > fabsf(rs_ext)) rs_ext = g_rs[i];
      ra_off = fmaxf(ra_off, fabsf(g_ra[i]));
    }
    ok(ra_ext == KN[k][0], "an onset drives attackGainDb to attackDb exactly", ra_ext, KN[k][0]);
    ok(rs_on <= nil_bound(KN[k][1]), "an onset leaves sustainGainDb at 0, within the rounding bound", rs_on, nil_bound(KN[k][1]));
    ok(rs_ext == KN[k][1], "a cut-off drives sustainGainDb to sustainDb exactly", rs_ext, KN[k][1]);
    ok(ra_off <= nil_bound(KN[k][0]), "a cut-off leaves attackGainDb at 0, within the rounding bound", ra_off, nil_bound(KN[k][0]));
  }
  clean();
}

/* ---- C: the sum and the ranges --------------------------------------------------------------- */
static void arm_sum(void) {
  g_arm = "C";
  const uint32_t n = (uint32_t)(0.6f * g_sr);
  stimulus(g_l, g_or, n, 0x7a7au, 1500u);
  run_read(24.0f, -24.0f, -3.0f, n);
  int sum = 1, inside = 1;
  for (uint32_t i = 0; i < n; i++) {
    sum &= g_ra[i] + g_rs[i] + -3.0f == g_g[i];
    inside &= g_ra[i] >= OMX_TRANSIENT_READOUT_ATTACK_GAIN_DB_MIN && g_ra[i] <= OMX_TRANSIENT_READOUT_ATTACK_GAIN_DB_MAX &&
              g_rs[i] >= OMX_TRANSIENT_READOUT_SUSTAIN_GAIN_DB_MIN && g_rs[i] <= OMX_TRANSIENT_READOUT_SUSTAIN_GAIN_DB_MAX;
  }
  ok(sum, "attackGainDb + sustainGainDb + outputDb is the applied gain, bit for bit", sum, 1);
  ok(inside, "each readout lies inside its declared range", inside, 1);
  ok(OMX_TRANSIENT_READOUT_ATTACK_GAIN_DB_MIN == -24.0f && OMX_TRANSIENT_READOUT_ATTACK_GAIN_DB_MAX == 24.0f &&
         OMX_TRANSIENT_READOUT_SUSTAIN_GAIN_DB_MIN == -24.0f && OMX_TRANSIENT_READOUT_SUSTAIN_GAIN_DB_MAX == 24.0f,
     "the readouts' ranges are the knobs' travels, -24 to +24 dB", 0, 0);
  clean();
  g_arm = "D";
  OmxTransientInstance bad;
  omx_transient_instance_init(&bad, 22050.0f);
  ok(omx_transient_instance_attack_gain_db(&bad) == 0.0f && omx_transient_instance_sustain_gain_db(&bad) == 0.0f,
     "a refused instance reads 0", 0, 0);
  clean();
}

int main(void) {
  (void)sine;
  (void)rms;
  (void)expect_clean;
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  for (unsigned ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_held();
    arm_edges();
    arm_sum();
  }
  return instance_oracle_end("transient_readouts");
}
