// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Standalone unit test for the HRP per-note baseline (hrp_baseline.h):
 *   cc -Wall -Wextra -O2 -o /tmp/hrp_baseline_test src/hrp_baseline.test.c -lm && /tmp/hrp_baseline_test
 * (also driven from `pnpm test` via the test:dsp script).
 *
 * Fed numbers by hand, never the analyzer chain: a learning bug and a detection bug look
 * identical through the chain, and these must fail for learning reasons only. Covers §20
 * (note-relative rows), §22 (robust, bounded-step), ripeness-as-absence, and §51 reset.
 */
#include <math.h>
#include <stdio.h>

#include <omxdsp/analysis/omx_hrp_baseline.h>

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

static const int A4 = 69;
static const int C5 = 72;

/* Feed n accepted measurements of one (note, harmonic) at one level. */
static void feed(OmxHrpBaseline *b, int note, uint32_t k, float db, int n,
                 const OmxHrpBaselineConfig *cfg) {
  for (int i = 0; i < n; i++) omx_hrp_baseline_learn(b, note, k, db, cfg);
}

int main(void) {
  const OmxHrpBaselineConfig cfg = omx_hrp_baseline_config_default();
  OmxHrpBaseline b;
  float out = 0;

  /* ---- unripe is ABSENT, not zero ------------------------------------------------ */
  omx_hrp_baseline_init(&b);
  check(!omx_hrp_baseline_read(&b, A4, 4, &cfg, &out), "an empty model answers nothing");
  feed(&b, A4, 4, -12.0f, cfg.ripe_updates - 1, &cfg);
  check(!omx_hrp_baseline_read(&b, A4, 4, &cfg, &out),
        "one update short of ripe still answers nothing");
  omx_hrp_baseline_learn(&b, A4, 4, -12.0f, &cfg);
  check(omx_hrp_baseline_read(&b, A4, 4, &cfg, &out), "the ripe row answers");
  check(fabsf(out - (-12.0f)) < 1e-4f, "a steady level is learned exactly");

  /* ---- the estimator is ROBUST: one outlier moves it by at most step_db ----------- */
  omx_hrp_baseline_learn(&b, A4, 4, +8.0f, &cfg); /* a 20 dB outlier */
  check(omx_hrp_baseline_read(&b, A4, 4, &cfg, &out) && fabsf(out - (-11.5f)) < 1e-4f,
        "a 20 dB outlier moves the baseline by exactly one bounded step");

  /* ---- and it CONVERGES: a real change walks the baseline there ------------------- */
  omx_hrp_baseline_init(&b);
  feed(&b, A4, 4, -12.0f, cfg.ripe_updates, &cfg);
  feed(&b, A4, 4, -6.0f, 40, &cfg); /* the instrument genuinely changed: 6 dB up */
  check(omx_hrp_baseline_read(&b, A4, 4, &cfg, &out) && fabsf(out - (-6.0f)) < 1e-3f,
        "a sustained change converges (40 steps of 0.5 dB cover 6 dB with room)");

  /* ---- rows are per NOTE and per HARMONIC (§20) ----------------------------------- */
  omx_hrp_baseline_init(&b);
  feed(&b, A4, 4, -12.0f, cfg.ripe_updates, &cfg);
  check(!omx_hrp_baseline_read(&b, C5, 4, &cfg, &out),
        "learning A4 teaches nothing about C5 — the model is note-relative");
  check(!omx_hrp_baseline_read(&b, A4, 5, &cfg, &out),
        "learning H4 teaches nothing about H5");
  check(omx_hrp_baseline_ripe_rows(&b, &cfg) == 1, "exactly one row is ripe");

  /* ---- the first accepted measurement SEEDS (no walk up from zero) ---------------- */
  omx_hrp_baseline_init(&b);
  feed(&b, C5, 2, -30.0f, cfg.ripe_updates, &cfg);
  check(omx_hrp_baseline_read(&b, C5, 2, &cfg, &out) && fabsf(out - (-30.0f)) < 1e-4f,
        "the first measurement seeds the row outright — no ramp from 0 dB");

  /* ---- out-of-model input is IGNORED, never clamped ------------------------------- */
  omx_hrp_baseline_init(&b);
  feed(&b, 200, 4, -12.0f, cfg.ripe_updates, &cfg); /* not a MIDI note */
  feed(&b, A4, 0, -12.0f, cfg.ripe_updates, &cfg);  /* harmonic is 1-based */
  feed(&b, A4, OMX_HRP_HARMONICS + 1, -12.0f, cfg.ripe_updates, &cfg);
  check(omx_hrp_baseline_ripe_rows(&b, &cfg) == 0,
        "notes and harmonics outside the model teach nothing anywhere");

  /* ---- §51 reset: everything unlearned, nothing answers --------------------------- */
  omx_hrp_baseline_init(&b);
  feed(&b, A4, 4, -12.0f, cfg.ripe_updates, &cfg);
  omx_hrp_baseline_init(&b);
  check(!omx_hrp_baseline_read(&b, A4, 4, &cfg, &out), "reset clears the learned model");
  check(omx_hrp_baseline_ripe_rows(&b, &cfg) == 0, "reset leaves no ripe rows");

  if (g_fail) {
    fprintf(stderr, "hrp_baseline: %d/%d checks failed\n", g_fail, g_checks);
    return 1;
  }
  printf("hrp_baseline: %d checks passed\n", g_checks);
  return 0;
}
