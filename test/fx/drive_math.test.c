// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The drive against its declared contracts, the drive arms of the engine's rt_contracts.test.c
 * moved with the kernel: every declared rate, every input shape, factors 1/2/4, random drive,
 * character and mix — an engaged stage is finite, bypass is the identity bit for bit, and a
 * NaN/Inf block does not poison the auto-gain ramp (D7). Then the coefficient twin, moved from
 * the engine's LV2 shell test with omx_drive_design.h: every row of omx_drive_design_corpus.h
 * (designed by the console's TypeScript) is reproduced by the C to float32. The positive control:
 * the poisoning block's own `finite-in` precondition IS recorded.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/fx/omx_drive_design.h>
#include <omxdsp/fx/omx_drive_design_corpus.h>

#include "contracts_battery.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** The LV2 shell test's check(), on the battery's ledger. */
static void check(int cond, const char *what) { ok(cond, what, (double)cond, 1.0); }

/* The positive control: a NaN/Inf block into an engaged stage IS recorded under `finite-in`, so
 * the empty ledgers below are a ledger that could have spoken. */
static void test_drive_positive_control(void) {
  g_stage = "drive-positive-control";
  struct omx_drive p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.curve = OMX_DRIVE_SOFT;
  p.band = OMX_DRIVE_BAND_FULL;
  p.drive_lin = 1.0f;
  p.mix = 1.0f;
  p.trim_lin = 1.0f;
  p.os_factor = 1;
  p.band_c[0] = p.tilt_c[0] = p.tilt_inv_c[0] = p.hf_c[0] = 1.0f;
  omx_drive_time_constants(&p, 48000.0f);
  struct omx_drive_state st;
  omx_drive_state_init(&st, omx_drive_factor_of(1));
  float l[N], r[N];
  for (uint32_t i = 0; i < N; i++) l[i] = r[i] = (i & 1u) ? (float)NAN : (float)INFINITY;
  omx_contract_log.count = 0u;
  omx_drive_process(l, r, N, &p, &st);
  int found = 0;
  for (uint32_t i = 0; i < omx_contract_log.count && i < OMX_CONTRACT_MAX; i++)
    if (strcmp(omx_contract_log.rec[i].token, "finite-in") == 0) found = 1;
  ok(found, "a NaN/Inf block IS recorded as finite-in", (double)found, 1.0);
  omx_contract_log.count = 0u;
}

static void test_drive(void) {
  g_stage = "drive";
  for (int ri = 0; ri < RATES; ri++) {
    for (int fi = 0; fi < 3; fi++) {
      const int factor = fi == 0 ? 1 : (fi == 1 ? 2 : 4);
      for (int sh = 0; sh < SH_COUNT; sh++) {
        float in[N], l[N], r[N];
        fill(in, N, (enum shape)sh, RATE[ri]);
        memcpy(l, in, sizeof l);
        memcpy(r, in, sizeof r);
        struct omx_drive p;
        memset(&p, 0, sizeof p);
        p.enabled = 1;
        p.curve = OMX_DRIVE_SOFT;
        p.band = OMX_DRIVE_BAND_FULL;
        p.drive_lin = powf(10.0f, rnd(0.0f, 18.0f) / 20.0f);
        p.even_w = rnd(0.0f, 1.0f);
        p.mix = rnd(0.0f, 1.0f);
        p.trim_lin = 1.0f;
        p.os_factor = factor;
        p.band_c[0] = p.tilt_c[0] = p.tilt_inv_c[0] = p.hf_c[0] = 1.0f;
        p.dc_coeff = 1.0f - 2.0f * (float)M_PI * 5.0f / RATE[ri];
        p.ag_coeff = expf(-1.0f / (0.05f * RATE[ri]));
        struct omx_drive_state st;
        omx_drive_state_init(&st, omx_drive_factor_of(factor));
        omx_drive_process(l, r, N, &p, &st);
        ok(omx_block_finite(l, N) && omx_block_finite(r, N),
           "an engaged stage is finite at any drive",
           (double)(omx_block_finite(l, N) && omx_block_finite(r, N)), 1.0);
        drain_violations(in, N);

        /* BYPASS IS THE IDENTITY — not "close to": a disengaged stage touches no sample. */
        memcpy(l, in, sizeof l);
        memcpy(r, in, sizeof r);
        p.enabled = 0;
        omx_drive_process(l, r, N, &p, &st);
        ok(memcmp(l, in, sizeof l) == 0 && memcmp(r, in, sizeof r) == 0,
           "bypass is the IDENTITY", 0.0, 0.0);
        drain_violations(in, N);
      }
    }
  }
}

/* ---- 6b. drive — a NaN/Inf block self-heals (D7) ---------------------------------------------- */

/*
 * A NaN/Inf sample entering the drive stage is a declared PRE violation (`finite-in`) — the
 * contract records it, it does not refuse to run. What the stage must not do is let that one bad
 * block poison the auto-gain ramp FOREVER, the way `mix_drive.h:492`'s hand-rolled `c += step`
 * did before it ran `omx_dsp.h`'s `omx_ramp` (2026-08-07's `isfinite` self-heal). Isolated to the
 * path the ramp actually owns — band FULL, no tilt, no HF roll-off, no DC blocker (`even_w = 0`
 * skips it), factor 1 (no oversampler filter memory) — so a failure here is the ramp, not one of
 * the tier's other known-permanent recursive-state pokes (`mix_dsp.h:906-910`'s own "a single NaN
 * in state is permanent" — a separate, undeclared risk this fold does not touch).
 */
static void test_drive_nan_self_heals(void) {
  g_stage = "drive-nan";
  for (int ri = 0; ri < RATES; ri++) {
    struct omx_drive p;
    memset(&p, 0, sizeof p);
    p.enabled = 1;
    p.curve = OMX_DRIVE_SOFT;
    p.band = OMX_DRIVE_BAND_FULL;
    p.drive_lin = 1.0f;
    p.even_w = 0.0f;
    p.mix = 1.0f;
    p.trim_lin = 1.0f;
    p.auto_gain = 1;
    p.os_factor = 1;
    p.band_c[0] = p.tilt_c[0] = p.tilt_inv_c[0] = p.hf_c[0] = 1.0f;
    p.dc_coeff = 1.0f - 2.0f * (float)M_PI * 5.0f / RATE[ri];
    p.ag_coeff = expf(-1.0f / (0.05f * RATE[ri]));
    struct omx_drive_state st;
    omx_drive_state_init(&st, omx_drive_factor_of(1));

    /* the poisoning block: half NaN, half Inf. The PRE fires — that IS the point, not the
     * failure — so it is drained without asserting the ledger is empty. */
    float l[N], r[N];
    for (uint32_t i = 0; i < N; i++) l[i] = r[i] = (i & 1u) ? (float)NAN : (float)INFINITY;
    omx_drive_process(l, r, N, &p, &st);
    omx_contract_log.count = 0u;

    /* finite blocks after: the ramp must have self-healed, not carried the poison forward. */
    for (int blk = 0; blk < 4; blk++) {
      float in[N];
      fill(in, N, SH_NOISE, RATE[ri]);
      memcpy(l, in, sizeof l);
      memcpy(r, in, sizeof r);
      omx_drive_process(l, r, N, &p, &st);
      ok(omx_block_finite(l, N) && omx_block_finite(r, N),
         "a NaN/Inf block does not poison the ramp — later finite blocks stay finite",
         (double)(omx_block_finite(l, N) && omx_block_finite(r, N)), 1.0);
      drain_violations(in, N);
    }
  }
}

static void test_design_corpus(void) {
  check(OMX_DRIVE_DESIGN_CORPUS_ROWS == sizeof(OMX_DRIVE_DESIGN_CORPUS) / sizeof(OMX_DRIVE_DESIGN_CORPUS[0]),
        "corpus: the declared row count is the array's");
  check(OMX_DRIVE_DESIGN_CORPUS_ROWS >= 40u, "corpus: positive control — enough rows to mean anything");
  int worst_row = -1;
  double worst = 0.0;
  for (uint32_t r = 0; r < OMX_DRIVE_DESIGN_CORPUS_ROWS; r++) {
    const OmxDriveDesignRow *row = &OMX_DRIVE_DESIGN_CORPUS[r];
    float c[5];
    if (row->kind == 0) omx_drive_design_lowpass((float)row->freq_hz, row->rate, c);
    else omx_drive_design_highshelf((float)row->freq_hz, (float)row->gain_db, row->rate, c);
    for (int i = 0; i < 5; i++) {
      /* float32's tolerance on a coefficient of order 1: 1e-6 absolute, and relative for the
       * tiny b-terms of a low corner at a high rate (b0 ~ 1e-8 at 20 Hz / 192 kHz). */
      const double want = row->coeffs[i];
      const double err = fabs((double)c[i] - want);
      const double tol = 1e-6 * (fabs(want) > 1.0 ? fabs(want) : 1.0);
      if (fabs(want) < 1e-3 ? err > 1e-6 * fabs(want) + 1e-12 : err > tol) {
        if (err > worst) { worst = err; worst_row = (int)r; }
      }
    }
  }
  if (worst_row >= 0)
    fprintf(stderr, "  corpus row %d (%s @ %g Hz / %u): worst |err| %.3g\n", worst_row,
            OMX_DRIVE_DESIGN_CORPUS[worst_row].why, OMX_DRIVE_DESIGN_CORPUS[worst_row].freq_hz,
            OMX_DRIVE_DESIGN_CORPUS[worst_row].rate, worst);
  check(worst_row < 0, "corpus: every row of the TypeScript design is reproduced by the C, to float32");
  /* The tilt CONSTANT is pinned too: a row designed at the console's DRIVE_TILT_SHELF_DB must
   * exist, so the bank omx_drive_design_bank builds is the one the corpus holds equal. */
  int tilt_rows = 0;
  for (uint32_t r = 0; r < OMX_DRIVE_DESIGN_CORPUS_ROWS; r++)
    if (OMX_DRIVE_DESIGN_CORPUS[r].kind == 1 && OMX_DRIVE_DESIGN_CORPUS[r].gain_db == OMX_DRIVE_TILT_SHELF_DB) tilt_rows++;
  check(tilt_rows > 0, "corpus: the tilt shelf constant is one the TypeScript designed rows at");
}

int main(void) {
  omx_fx_require_rate_floor();
  test_drive_positive_control();
  test_drive();
  test_drive_nan_self_heals();
  test_design_corpus();
  return omx_fx_battery_report("drive");
}
