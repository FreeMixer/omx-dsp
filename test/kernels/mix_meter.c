// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_meter.h>
#include <omxdsp/mix/omx_mix_coef.h>

/* ---- the meters' measure half (mix/omx_meter.h, FreeMixer/omx-dsp#98). At each of the nine RME
 * rates, one second of a 0.7 sine at 1 kHz with one placed sample of −0.95: the block peak is
 * exactly 0.95 on either leg and with a NULL or aliased second leg; the per-leg peaks are each
 * leg's own peak times its own coefficient; a run of two full-scale samples is no clip, a run of
 * three is, on either leg, and a scale that lifts the tone to full scale for three samples
 * running trips it. The peak hold, fed the tone's 10 ms block peaks, ends on the whole second's
 * peak; a smaller, an equal (+0 against −0) or a NaN peak leaves the slot as it is. The strip's
 * post-fader reading is the block peak times its level, bit for bit, and follows gain, fader and
 * mute. ---------------------------------------------------------------------------------------- */

static const float MIX_METER_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

static void arm_mix_meter(void) {
  g_arm = "mix_meter";
  static float a[192000], b[192000];
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const float sr = MIX_METER_RATES[ri];
    const uint32_t n = (uint32_t)sr;
    for (uint32_t i = 0; i < n; i++) {
      a[i] = 0.7f * sinf((float)(2.0 * M_PI * 1000.0 * (double)i / sr));
      b[i] = 0.5f * a[i];
    }
    a[n / 3] = -0.95f;
    ok(omx_meter_block_peak(a, NULL, n) == 0.95f, "the block peak of one leg", omx_meter_block_peak(a, NULL, n), 0.95);
    ok(omx_meter_block_peak(a, a, n) == 0.95f, "an aliased leg is read once", omx_meter_block_peak(a, a, n), 0.95);
    ok(omx_meter_block_peak(b, a, n) == 0.95f, "the peak across two legs", omx_meter_block_peak(b, a, n), 0.95);
    float pl, pr;
    omx_meter_leg_peaks(a, b, n, 0.5f, 2.0f, &pl, &pr);
    const float pb = omx_meter_block_peak(b, NULL, n);
    ok(pl == 0.95f * 0.5f && pr == pb * 2.0f, "each leg's peak times its own coefficient", pl, 0.475);

    ok(!omx_meter_clip_run(a, b, n, 1.0f, OMX_METER_CLIP_LINEAR, OMX_METER_CLIP_RUN),
       "a tone under full scale never clips", 0, 0);
    b[n / 2] = 1.0f;
    b[n / 2 + 1] = -1.0f;
    ok(!omx_meter_clip_run(a, b, n, 1.0f, OMX_METER_CLIP_LINEAR, OMX_METER_CLIP_RUN),
       "two full-scale samples are no clip", 0, 0);
    b[n / 2 + 2] = 1.0f;
    ok(omx_meter_clip_run(a, b, n, 1.0f, OMX_METER_CLIP_LINEAR, OMX_METER_CLIP_RUN),
       "three full-scale samples running are a clip, on the second leg", 0, 0);
    ok(omx_meter_clip_run(b, NULL, n, 1.0f, OMX_METER_CLIP_LINEAR, OMX_METER_CLIP_RUN),
       "and on a single leg", 0, 0);
    a[n / 3] = 0.0f;
    /* scaled by 2 the 0.7 tone crests at 1.4, over full scale for several samples running */
    ok(omx_meter_clip_run(a, NULL, n, 2.0f, OMX_METER_CLIP_LINEAR, OMX_METER_CLIP_RUN),
       "a scale that lifts the tone over full scale trips the run", 0, 0);

    /* the peak hold over the second's 10 ms blocks ends on the second's own peak */
    a[n / 3] = -0.95f;
    const uint32_t blk = n / 100u;
    float held = 0.0f;
    for (uint32_t i = 0; i + blk <= n; i += blk) held = omx_meter_hold_peak(held, omx_meter_block_peak(a + i, NULL, blk));
    ok(held == omx_meter_block_peak(a, NULL, n), "the hold ends on the loudest block's peak", held, 0.95);
    ok(omx_meter_hold_peak(0.5f, 0.25f) == 0.5f, "a smaller peak leaves the slot as it is",
       omx_meter_hold_peak(0.5f, 0.25f), 0.5);
    ok(omx_meter_hold_peak(0.25f, 0.5f) == 0.5f, "a greater peak replaces it", omx_meter_hold_peak(0.25f, 0.5f), 0.5);
    ok(!signbit(omx_meter_hold_peak(0.0f, -0.0f)), "an equal peak is not stored (+0 stays +0)", 0, 0);
    ok(omx_meter_hold_peak(0.5f, NAN) == 0.5f, "a NaN peak never lands in the slot", omx_meter_hold_peak(0.5f, NAN), 0.5);

    /* the strip's post-fader reading: block peak × level */
    static const float levels[] = {1.0f, 2.0f, 0.5f, 0.70794578f, 0.0f};
    uint32_t strip_bits = 0;
    for (uint32_t k = 0; k < 5u; k++) {
      if (omx_meter_strip_peak(a, b, n, levels[k]) != omx_meter_block_peak(a, b, n) * levels[k]) strip_bits++;
      if (omx_meter_strip_peak(a, NULL, n, levels[k]) != 0.95f * levels[k]) strip_bits++;
    }
    ok(strip_bits == 0, "the strip reading is the block peak times its level, bit for bit", strip_bits, 0);
  }
  {
    float in[4] = {1.0f, -1.0f, 0.5f, -0.25f}, quiet[3] = {0.1f, -0.3f, 0.2f}, sl[2] = {0.2f, 0.2f}, sr2[2] = {0.2f, 0.8f};
    ok(omx_meter_strip_peak(in, in, 4, omx_mix_strip_level(2.0f, 1.0f, 0)) == 2.0f, "strip reading follows the gain up", 0, 0);
    ok(omx_meter_strip_peak(in, in, 4, omx_mix_strip_level(1.0f, 0.5f, 0)) == 0.5f, "strip reading follows the fader", 0, 0);
    ok(omx_meter_strip_peak(in, in, 4, omx_mix_strip_level(1.0f, 1.0f, 1)) == 0.0f, "strip reading follows the mute to 0", 0, 0);
    ok(omx_meter_strip_peak(quiet, quiet, 3, 2.0f) == 0.3f * 2.0f, "strip reading is the max |sample| times the level", 0, 0);
    ok(omx_meter_strip_peak(sl, sr2, 2, 1.0f) == 0.8f, "strip reading takes the louder leg", 0, 0);
  }
  expect_clean();
}
