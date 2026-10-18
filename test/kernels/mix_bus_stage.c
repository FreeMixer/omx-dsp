// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_bus_stage.h>

/* ---- the bus stages (mix/omx_bus_stage.h, FreeMixer/omx-dsp#185). At each of the nine RME rates,
 * a 10 ms block. omx_mix_strip: a settled source is bus + in × gain on each leg exactly; a 0 → 1
 * move ramps linearly across the block and the cursors end on the targets; the sum is linear in
 * its inputs; a mono source passed twice lands the same on both legs; a NaN target mutes the
 * contribution, leaves a finite cursor and is recorded as `bus/mix-strip pre finite-coef`; a
 * poisoned cursor ramps from 0 and heals; an empty block leaves everything alone.
 * omx_bus_out_gain: the legs and cursors of omx_bus_out_run() bit for bit over a balance sweep with
 * the fader at +10 dB, settled legs in × gain exactly, a balance past the range recorded as
 * `bus/output-gain pre balance-within-declared-range`, a gain below 0 (a negative fader) as
 * `bus/output-gain post fold-coefficient-in-range`. omx_bus_out_stage: the legs and cursors of
 * omx_bus_out_run(), each slot holding the loudest peak since the reader's clear. ------------- */

static const float MIX_BUS_STAGE_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;

/* Exactly one violation is on the ledger, with this stage, kind and token; otherwise each record
 * is printed (test/mix/sabotage.sh reads them). The ledger is then cleared. */
static int mix_bus_stage_recorded(const char *stage, const char *kind, const char *token) {
  const int one = omx_contract_log.count == 1u && omx_contract_record_ready(&omx_contract_log.rec[0]) &&
                  strcmp(omx_contract_log.rec[0].stage, stage) == 0 &&
                  strcmp(omx_contract_log.rec[0].kind, kind) == 0 &&
                  strcmp(omx_contract_log.rec[0].token, token) == 0;
  if (!one) {
    const uint32_t kept = omx_contract_log.count < OMX_CONTRACT_MAX ? omx_contract_log.count : OMX_CONTRACT_MAX;
    for (uint32_t i = 0; i < kept; i++)
      if (omx_contract_record_ready(&omx_contract_log.rec[i]))
        printf("  got [%s] %s %s\n", omx_contract_log.rec[i].stage, omx_contract_log.rec[i].kind,
               omx_contract_log.rec[i].token);
  }
  omx_contract_reset();
  return one;
}

static void arm_mix_bus_stage(void) {
  g_arm = "mix_bus_stage";
  static float a[1920], b[1920], bl[1920], br[1920], l1[1920], r1[1920], l2[1920], r2[1920];
  uint32_t seed = 0x0b5u;
  for (uint32_t ri = 0; ri < OMX_RME_RATES_COUNT; ri++) {
    const uint32_t n = (uint32_t)(MIX_BUS_STAGE_RATES[ri] / 100.0f);
    for (uint32_t i = 0; i < n; i++) {
      seed = seed * 1664525u + 1013904223u;
      a[i] = (float)(int32_t)(seed >> 8) / 8388608.0f - 1.0f;
      seed = seed * 1664525u + 1013904223u;
      b[i] = 0.5f * ((float)(int32_t)(seed >> 8) / 8388608.0f - 1.0f);
    }

    /* ---- omx_mix_strip ---- */
    /* settled: bus + in × gain on each leg, exactly */
    for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.125f;
    float cl = 0.5f, cr = 0.25f;
    omx_mix_strip(a, b, bl, br, n, 0.5f, 0.25f, &cl, &cr);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.125f + a[i] * 0.5f || br[i] != 0.125f + b[i] * 0.25f) bad++;
    ok(bad == 0, "a settled source is bus + in × gain on each leg exactly", bad, 0);
    ok(cl == 0.5f && cr == 0.25f, "settled cursors stay on the targets", cl, 0.5);

    /* a 0 → 1 move on DC is a linear ramp across the block */
    for (uint32_t i = 0; i < n; i++) { l1[i] = 0.8f; bl[i] = br[i] = 0.0f; }
    cl = cr = 0.0f;
    omx_mix_strip(l1, l1, bl, br, n, 1.0f, 1.0f, &cl, &cr);
    double worst = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const double want = 0.8 * (double)i / (double)n;
      if (fabs(bl[i] - want) > worst) worst = fabs(bl[i] - want);
      if (fabs(br[i] - want) > worst) worst = fabs(br[i] - want);
    }
    ok(worst < 1e-6, "a 0 → 1 move ramps linearly across the block", worst, 1e-6);
    ok(cl == 1.0f && cr == 1.0f, "the cursors end on the targets", cl, 1.0);

    /* linear in the inputs: a then b into one bus is a + b into another */
    for (uint32_t i = 0; i < n; i++) { bl[i] = br[i] = l2[i] = r2[i] = 0.0f; l1[i] = a[i] + b[i]; }
    float c1 = 0.3f, c2 = 0.3f, c3 = 0.3f, c4 = 0.3f, c5 = 0.3f, c6 = 0.3f;
    omx_mix_strip(a, a, bl, br, n, 0.7f, 0.7f, &c1, &c2);
    omx_mix_strip(b, b, bl, br, n, 0.7f, 0.7f, &c3, &c4);
    omx_mix_strip(l1, l1, l2, r2, n, 0.7f, 0.7f, &c5, &c6);
    worst = 0.0;
    for (uint32_t i = 0; i < n; i++) if (fabs(bl[i] - l2[i]) > worst) worst = fabs(bl[i] - l2[i]);
    ok(worst < 1e-6, "the sum is linear in its inputs", worst, 1e-6);

    /* a mono source passed twice: the same on both legs */
    bad = 0;
    for (uint32_t i = 0; i < n; i++) if (bl[i] != br[i]) bad++;
    ok(bad == 0, "a mono source passed twice lands the same on both legs", bad, 0);

    expect_clean();

    /* a NaN target: recorded, the contribution mutes, the cursor ends finite */
    for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.125f;
    cl = 0.0f;
    cr = 1.0f;
    omx_mix_strip(a, b, bl, br, n, NAN, 1.0f, &cl, &cr);
    ok(mix_bus_stage_recorded("bus/mix-strip", "pre", "finite-coef"),
       "a NaN coefficient is recorded as bus/mix-strip pre finite-coef", 0, 0);
    bad = 0;
    for (uint32_t i = 0; i < n; i++) if (bl[i] != 0.125f || br[i] != 0.125f + b[i]) bad++;
    ok(bad == 0, "a NaN target mutes its leg and leaves the other mixing", bad, 0);
    ok(cl == 0.0f && cr == 1.0f, "the ramp self-heals: the cursors end finite", cl, 0.0);

    /* a poisoned cursor: the ramp starts from 0 and the cursor heals onto the target */
    for (uint32_t i = 0; i < n; i++) bl[i] = br[i] = 0.0f;
    cl = NAN;
    cr = INFINITY;
    omx_mix_strip(a, a, bl, br, n, 1.0f, 1.0f, &cl, &cr);
    bad = 0;
    for (uint32_t i = 0; i < n; i++) if (bl[i] != a[i] * ((1.0f / (float)n) * (float)i) || br[i] != bl[i]) bad++;
    ok(bad == 0, "a poisoned cursor ramps from 0", bad, 0);
    ok(cl == 1.0f && cr == 1.0f, "and heals onto the target", cl, 1.0);

    /* an empty block leaves the bus and the cursors alone */
    cl = 0.3f;
    cr = 0.6f;
    omx_mix_strip(a, b, bl, br, 0, 1.0f, 1.0f, &cl, &cr);
    ok(cl == 0.3f && cr == 0.6f, "an empty block is a no-op", cl, 0.3);
    expect_clean();

    /* ---- omx_bus_out_gain ---- */
    uint32_t diff = 0, settled = 0;
    for (uint32_t s = 0; s <= 40; s++) {
      const float bal = -1.0f + (float)s / 20.0f;
      for (int ch = 1; ch <= 2; ch++) {
        for (uint32_t i = 0; i < n; i++) { l1[i] = l2[i] = a[i]; r1[i] = r2[i] = b[i]; }
        float g1l = 0.2f, g1r = 0.9f, g2l = 0.2f, g2r = 0.9f, pl, pr;
        omx_bus_out_gain(l1, r1, n, ch, 3.1623f, 0, bal, &g1l, &g1r);
        omx_bus_out_run(l2, r2, n, ch, 3.1623f, 0, bal, &g2l, &g2r, &pl, &pr);
        if (memcmp(l1, l2, n * sizeof(float)) != 0 || memcmp(r1, r2, n * sizeof(float)) != 0 || g1l != g2l ||
            g1r != g2r)
          diff++;
        /* settled at the targets: each leg is in × gain exactly */
        float tl, tr;
        omx_bus_out_gains(ch, 0.6f, 0, bal, &tl, &tr);
        for (uint32_t i = 0; i < n; i++) { l1[i] = a[i]; r1[i] = b[i]; }
        g1l = tl;
        g1r = tr;
        omx_bus_out_gain(l1, r1, n, ch, 0.6f, 0, bal, &g1l, &g1r);
        for (uint32_t i = 0; i < n; i++) if (l1[i] != a[i] * tl || r1[i] != b[i] * tr) settled++;
      }
    }
    ok(diff == 0, "the out gain is omx_bus_out_run's legs and cursors bit for bit, fader at +10 dB", diff, 0);
    ok(settled == 0, "settled, each leg is in × gain exactly", settled, 0);
    expect_clean();

    float g1l = 1.0f, g1r = 1.0f;
    for (uint32_t i = 0; i < n; i++) { l1[i] = a[i]; r1[i] = b[i]; }
    omx_bus_out_gain(l1, r1, n, 2, 1.0f, 0, 1.5f, &g1l, &g1r);
    ok(mix_bus_stage_recorded("bus/output-gain", "pre", "balance-within-declared-range"),
       "a balance past +1 is recorded as bus/output-gain pre balance-within-declared-range", 0, 0);
    omx_bus_out_gain(l1, r1, n, 2, 1.0f, 0, -1.5f, &g1l, &g1r);
    ok(mix_bus_stage_recorded("bus/output-gain", "pre", "balance-within-declared-range"),
       "a balance past −1 is recorded too", 0, 0);
    g1l = g1r = 0.0f;
    omx_bus_out_gain(l1, r1, n, 2, -0.5f, 0, 0.0f, &g1l, &g1r);
    ok(mix_bus_stage_recorded("bus/output-gain", "post", "fold-coefficient-in-range"),
       "a gain below 0 is recorded as bus/output-gain post fold-coefficient-in-range", 0, 0);

    /* ---- omx_bus_out_stage ---- */
    _Atomic float sl = 0.0f, sr = 0.0f;
    float k1l = 1.0f, k1r = 1.0f, k2l = 1.0f, k2r = 1.0f, pl, pr;
    diff = 0;
    float first_l = 0.0f, first_r = 0.0f;
    static const float faders[4] = {1.0f, 0.5f, 0.5f, 0.25f};
    for (uint32_t blk = 0; blk < 4u; blk++) {
      for (uint32_t i = 0; i < n; i++) { l1[i] = l2[i] = a[i]; r1[i] = r2[i] = b[i]; }
      if (blk == 2u) { /* the reader's clear */
        atomic_store(&sl, 0.0f);
        atomic_store(&sr, 0.0f);
      }
      omx_bus_out_stage(l1, r1, n, 2, faders[blk], 0, -0.25f, &k1l, &k1r, &sl, &sr);
      omx_bus_out_run(l2, r2, n, 2, faders[blk], 0, -0.25f, &k2l, &k2r, &pl, &pr);
      if (memcmp(l1, l2, n * sizeof(float)) != 0 || memcmp(r1, r2, n * sizeof(float)) != 0 || k1l != k2l ||
          k1r != k2r)
        diff++;
      if (blk == 0u) {
        first_l = pl;
        first_r = pr;
        ok(atomic_load(&sl) == pl && atomic_load(&sr) == pr, "an empty slot takes the block's peak", sl, pl);
      } else if (blk == 1u) {
        ok(atomic_load(&sl) == first_l && atomic_load(&sr) == first_r && pl <= first_l && pr <= first_r,
           "a quieter block leaves the louder held peak", sl, first_l);
      } else if (blk == 2u) {
        first_l = pl;
        first_r = pr;
        ok(atomic_load(&sl) == pl && atomic_load(&sr) == pr, "after the reader's clear the slot takes the new peak", sl, pl);
      } else {
        ok(atomic_load(&sl) == first_l && atomic_load(&sr) == first_r && pl <= first_l && pr <= first_r,
           "the hold outlasts a quieter block again", sl, first_l);
      }
    }
    ok(diff == 0, "the out stage is omx_bus_out_run's legs and cursors bit for bit", diff, 0);
    expect_clean();
  }
}
