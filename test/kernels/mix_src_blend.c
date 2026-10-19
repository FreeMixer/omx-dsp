// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_src_blend.h>

/* ---- the source switch (mix/omx_src_blend.h). At each of the nine oracle rates, a 10 ms block of
 * noise on the live input and the track: settled on the track, the track is left bit for bit;
 * settled on live, the track becomes the live block, or silence with no live input; a switch from
 * live to track on two constants is live + a·(track − live) with `a` ramping linearly from 0, its
 * first sample exactly live; a switch on noise never exceeds the louder input; the cursor ends on
 * the target, 0 for a NaN one; an empty block leaves the track and the cursor alone. A NaN in the
 * track is recorded as `pb/src-blend pre finite-in`. --------------------------------------------- */

static const float MIX_SRC_BLEND_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

static void arm_mix_src_blend(void) {
  g_arm = "mix_src_blend";
  static float live[1920], track[1920], keep[1920];
  uint32_t seed = 0x9b1u;
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const uint32_t n = (uint32_t)(MIX_SRC_BLEND_RATES[ri] / 100.0f);
    for (uint32_t i = 0; i < n; i++) {
      seed = seed * 1664525u + 1013904223u;
      live[i] = (float)(int32_t)(seed >> 8) / 8388608.0f - 1.0f;
      seed = seed * 1664525u + 1013904223u;
      keep[i] = 0.5f * ((float)(int32_t)(seed >> 8) / 8388608.0f - 1.0f);
    }

    /* settled on the track: left as it is */
    memcpy(track, keep, n * sizeof(float));
    float cur = 1.0f;
    omx_src_blend(live, track, n, 1.0f, &cur);
    ok(memcmp(track, keep, n * sizeof(float)) == 0 && cur == 1.0f, "settled on the track, the track is left bit for bit", cur, 1.0);

    /* settled on live: the live block, or silence with no live input */
    cur = 0.0f;
    omx_src_blend(live, track, n, 0.0f, &cur);
    ok(memcmp(track, live, n * sizeof(float)) == 0 && cur == 0.0f, "settled on live, the track becomes the live block", cur, 0.0);
    memcpy(track, keep, n * sizeof(float));
    omx_src_blend(NULL, track, n, 0.0f, &cur);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) if (track[i] != 0.0f) bad++;
    ok(bad == 0, "settled on live with no live input, the track is silence", bad, 0);

    /* live to track on two constants: a linear crossfade from exactly live */
    static float lc[1920];
    for (uint32_t i = 0; i < n; i++) { lc[i] = 0.2f; track[i] = 0.8f; }
    cur = 0.0f;
    omx_src_blend(lc, track, n, 1.0f, &cur);
    double worst = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const double want = 0.2 + 0.6 * (double)i / (double)n;
      if (fabs(track[i] - want) > worst) worst = fabs(track[i] - want);
    }
    ok(track[0] == 0.2f, "the switch's first sample is exactly live", track[0], 0.2);
    ok(worst < 1e-6, "the switch ramps linearly from live to track", worst, 1e-6);
    ok(cur == 1.0f, "the cursor ends on the target", cur, 1.0);

    /* on noise, each way: never louder than the louder input */
    for (int way = 0; way < 2; way++) {
      memcpy(track, keep, n * sizeof(float));
      cur = way ? 1.0f : 0.0f;
      omx_src_blend(way ? NULL : live, track, n, way ? 0.0f : 1.0f, &cur);
      bad = 0;
      for (uint32_t i = 0; i < n; i++) {
        const float bound = fmaxf(way ? 0.0f : fabsf(live[i]), fabsf(keep[i]));
        if (fabsf(track[i]) > bound * (1.0f + 1e-6f)) bad++;
      }
      ok(bad == 0, "a switch never exceeds the louder input", bad, 0);
      ok(cur == (way ? 0.0f : 1.0f), "and its cursor ends on the target", cur, way ? 0.0 : 1.0);
    }

    /* a NaN target: taken as 0 */
    memcpy(track, keep, n * sizeof(float));
    cur = 1.0f;
    omx_src_blend(live, track, n, NAN, &cur);
    ok(cur == 0.0f, "a NaN target ends the cursor on 0", cur, 0.0);

    /* an empty block */
    memcpy(track, keep, n * sizeof(float));
    cur = 0.3f;
    omx_src_blend(live, track, 0, 1.0f, &cur);
    ok(cur == 0.3f && memcmp(track, keep, n * sizeof(float)) == 0, "an empty block is a no-op", cur, 0.3);
    expect_clean();

    /* the contract */
    memcpy(track, keep, n * sizeof(float));
    track[n / 2u] = NAN;
    cur = 0.0f;
    omx_src_blend(live, track, n, 1.0f, &cur);
    ok(omx_contract_log.count >= 1u && omx_contract_record_ready(&omx_contract_log.rec[0]) &&
           strcmp(omx_contract_log.rec[0].stage, "pb/src-blend") == 0 &&
           strcmp(omx_contract_log.rec[0].kind, "pre") == 0 && strcmp(omx_contract_log.rec[0].token, "finite-in") == 0,
       "a NaN in the track is recorded as pb/src-blend pre finite-in", omx_contract_log.count, 1);
    omx_contract_reset();
  }
}
