/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * live_shells.test.c — the two correctors' live-shell cores (omx_fbs_lv2.h, omx_hrp_lv2.h) by value, through the
 * calls a plugin face drives them with (spec 7.7 and 7.16):
 *
 *   no-worker   a host with no worker: the audio bit-exact, `analysing` 0, every report slot free (both shells)
 *   mode        the `mode` word resolved as declared: cut by default, report at one (both shells)
 *   report      `report` keeps the audio bit-exact while `cut` runs the bands (both shells); for omx-fbs the
 *               report timeline under `report` is the timeline under `cut`, pass for pass
 *   fade        omx-fbs over a growing 1 kHz ring then silence, at the nine oracle rates and the quanta 32 and
 *               1024: the band is planted, its cut measured on the audio at the reported depth within 0.5 dB, and
 *               its release, 90 s, is the staged fade's closed form pass for pass; the first 20 s are the same
 *               timeline bit for bit at the quantum 32, and at all six from 32 to 1024 at 48 kHz
 *   slots       a release frees its own slot and no other slot's words move (both shells); the HRP set carries
 *               a Q per band
 *
 * test/analysis/live-shells-sabotage.sh breaks each in a copy of the header and requires its arm to fail.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/analysis/omx_fbs_lv2.h>
#include <omxdsp/analysis/omx_hrp_lv2.h>

static int checks, failures;
static void check(int ok, const char *arm, const char *what, double got) {
  checks++;
  if (ok) return;
  failures++;
  printf("FAIL %s: %s (read %.9g)\n", arm, what, got);
}

static uint32_t lcg_state = 12345u;
static float noise(void) {
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return (float)((double)(lcg_state >> 8) / 16777216.0 - 0.5);
}

static int report_free(OmxBandReport r) {
  const OmxBandReport f = omx_band_report_free();
  return memcmp(&r, &f, sizeof(r)) == 0;
}

/* ---- a host with no worker ------------------------------------------------------------------------------ */

static void no_worker(void) {
  const uint32_t rate = 48000u, q = 256u;
  OmxFbsLv2 *f = calloc(1, sizeof(*f));
  void *block = malloc(omx_fbs_lv2_block_bytes(rate));
  check(omx_fbs_lv2_init_block(f, rate, block), "no-worker", "omx-fbs inits", 0);
  OmxHrpLv2 *h = calloc(1, sizeof(*h));
  float *hblock = malloc(omx_hrp_lv2_block_floats(rate) * sizeof(float));
  check(omx_hrp_lv2_init_block(h, rate, hblock), "no-worker", "omx-hrp inits", 0);
  float in[256], out[256];
  int same_f = 1, same_h = 1;
  for (uint32_t b = 0; b < rate * 3u / q; b++) {
    for (uint32_t i = 0; i < q; i++)
      in[i] = 0.25f * sinf(2.0f * 3.14159265f * 1000.0f * (float)(b * q + i) / (float)rate) + 0.01f * noise();
    omx_fbs_lv2_run(f, in, out, q);
    same_f &= memcmp(in, out, sizeof(in)) == 0;
    omx_hrp_lv2_run(h, in, out, q);
    same_h &= memcmp(in, out, sizeof(in)) == 0;
  }
  check(same_f, "no-worker", "omx-fbs passes 3 s of audio bit-exact with no worker", 0);
  check(same_h, "no-worker", "omx-hrp passes 3 s of audio bit-exact with no worker", 0);
  check(omx_fbs_lv2_analysing(f) == 0, "no-worker", "omx-fbs reports analysing 0", omx_fbs_lv2_analysing(f));
  check(omx_hrp_lv2_analysing(h) == 0, "no-worker", "omx-hrp reports analysing 0", omx_hrp_lv2_analysing(h));
  int all_free = 1;
  for (uint32_t k = 0; k < OMX_FBS_LV2_SLOTS; k++) all_free &= report_free(omx_fbs_lv2_report(f, k));
  for (uint32_t k = 0; k < OMX_HRP_LV2_MAX_BANDS; k++) all_free &= report_free(omx_hrp_lv2_report(h, k));
  check(all_free, "no-worker", "every report slot of both shells is free", 0);
  check(omx_fbs_lv2_due(f), "no-worker", "omx-fbs did ask for a pass (the host never ran it)", 0);
  free(block);
  free(f);
  free(hblock);
  free(h);
}

/* ---- the mode word -------------------------------------------------------------------------------------- */

static void mode_words(void) {
  static OmxFbsLv2 f;
  static OmxHrpLv2 h;
  const float words[] = {0.0f, 1.0f, 0.4f, 0.6f, NAN, 7.0f, -3.0f};
  const int want[] = {OMX_FBS_LV2_CUT, OMX_FBS_LV2_REPORT, OMX_FBS_LV2_CUT, OMX_FBS_LV2_REPORT,
                      OMX_FBS_LV2_CUT, OMX_FBS_LV2_REPORT, OMX_FBS_LV2_CUT};
  omx_fbs_lv2_init_block(&f, 0u, NULL);
  omx_hrp_lv2_init_block(&h, 0u, NULL);
  check(omx_fbs_lv2_mode(&f) == OMX_FBS_LV2_CUT, "mode", "omx-fbs comes up at cut", omx_fbs_lv2_mode(&f));
  check(omx_hrp_lv2_mode(&h) == OMX_HRP_LV2_CUT, "mode", "omx-hrp comes up at cut", omx_hrp_lv2_mode(&h));
  for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    omx_fbs_lv2_set_mode(&f, words[i]);
    omx_hrp_lv2_set_mode(&h, words[i]);
    check(omx_fbs_lv2_mode(&f) == want[i], "mode", "omx-fbs reads the mode word back as declared", words[i]);
    check(omx_hrp_lv2_mode(&h) == want[i], "mode", "omx-hrp reads the mode word back as declared", words[i]);
  }
}

/* ---- omx-fbs over a ring, at rates and quanta ------------------------------------------------------------- */

#define F0 1000.0
#define F1 2500.0
#define MAX_PASSES 1000

typedef struct {
  uint32_t passes;
  double t_ms[MAX_PASSES];
  OmxBandReport slot[MAX_PASSES][2];
  double last_ring_ms; /* slot 0's, the last pass it was held */
  double planted_db;   /* slot 0's report depth on the pass it was planted */
  double cut_measured_db, cut_reported_db;
  int bit_exact;
} Timeline;

/* The input: noise at -80 dBFS throughout; a ring at F0 growing 3 dB per 100 ms from -60 dBFS over 0.5..2.0 s,
 * held at -15 dBFS to 3.0 s, then gone; with `two`, a second ring at F1 0.3 s behind it that never stops. */
static float signal_at(uint64_t i, uint32_t rate, int two) {
  const double t = (double)i / rate;
  double x = 1e-4 * noise();
  if (t >= 0.5 && t < 3.0) {
    const double db = t < 2.0 ? -60.0 + 30.0 * (t - 0.5) : -15.0;
    x += pow(10.0, db / 20.0) * sin(2.0 * M_PI * F0 * t);
  }
  if (two && t >= 0.8) {
    const double db = t < 2.3 ? -60.0 + 30.0 * (t - 0.8) : -15.0;
    x += pow(10.0, db / 20.0) * sin(2.0 * M_PI * F1 * t);
  }
  return (float)x;
}

static void run_fbs(uint32_t rate, uint32_t quantum, int mode, double seconds, int two, Timeline *tl) {
  OmxFbsLv2 *s = calloc(1, sizeof(*s));
  void *block = malloc(omx_fbs_lv2_block_bytes(rate));
  OmxFbsLv2Set *set = calloc(1, sizeof(*set));
  float *in = malloc(quantum * sizeof(float)), *out = malloc(quantum * sizeof(float));
  memset(tl, 0, sizeof(*tl));
  tl->bit_exact = 1;
  tl->last_ring_ms = -1.0;
  tl->planted_db = NAN;
  lcg_state = 12345u;
  omx_fbs_lv2_init_block(s, rate, block);
  omx_fbs_lv2_set_mode(s, (float)mode);
  const OmxFbsLv2Ask ask = omx_fbs_lv2_ask_of(1.0f, 1.0f, 6.0f, 116.0f, 100.0f, 600.0f);
  const uint64_t total = (uint64_t)(seconds * rate);
  double e_in = 0.0, e_out = 0.0;
  for (uint64_t at = 0; at < total; at += quantum) {
    for (uint32_t i = 0; i < quantum; i++) in[i] = signal_at(at + i, rate, two);
    omx_fbs_lv2_run(s, in, out, quantum);
    if (memcmp(in, out, quantum * sizeof(float)) != 0) tl->bit_exact = 0;
    const double t = (double)at / rate;
    if (t >= 2.5 && t < 3.0)
      for (uint32_t i = 0; i < quantum; i++) {
        e_in += (double)in[i] * in[i];
        e_out += (double)out[i] * out[i];
      }
    while (omx_fbs_lv2_due(s) && tl->passes < MAX_PASSES) {
      omx_fbs_lv2_answer(s, &ask, set);
      omx_fbs_lv2_adopt(s, set);
      const uint32_t p = tl->passes++;
      tl->t_ms[p] = s->place.now_ms;
      tl->slot[p][0] = omx_fbs_lv2_report(s, 0);
      tl->slot[p][1] = omx_fbs_lv2_report(s, 1);
      if (s->place.slot[0].origin == OMX_CURVE_ORIGINS_FBS_LIVE) {
        if (isnan(tl->planted_db)) tl->planted_db = s->place.slot[0].planted_db;
        tl->last_ring_ms = s->place.slot[0].last_ring_ms;
      }
      if (fabs(tl->t_ms[p] - 2900.0) < 1e-9) tl->cut_reported_db = tl->slot[p][0].depth_db;
    }
  }
  tl->cut_measured_db = 10.0 * log10(e_out / e_in);
  free(in);
  free(out);
  free(set);
  free(block);
  free(s);
}

/* The staged fade's closed form for a band planted at `p_db`, last ringing at `last`, hold 1 s: the depth the
 * report carries at `t`, or NAN once the band is lifted. */
static double fade_at(double p_db, double last, double t) {
  const double ramp = (double)OMX_FBS_LIFT_RAMP_MS, step = ramp / OMX_FBS_LIFT_RAMP_STEPS;
  double begin = last + 1000.0, depth = p_db;
  for (uint32_t k = 1;; k++) {
    if (t < begin) return depth;
    const double to = p_db * pow(OMX_FBS_RELEASE_RATIO_DOUBLE, (double)k);
    if (t < begin + ramp) {
      const double n = floor((t - begin) / step);
      return depth + (to - depth) * (n / OMX_FBS_LIFT_RAMP_STEPS);
    }
    if (fabs(to) < OMX_FBS_RELEASE_DONE_DB) return NAN;
    depth = to;
    begin = begin + ramp + OMX_FBS_RELEASE_DWELL_MS;
  }
}

/* `b` (the shorter run) is `a` pass for pass, bit for bit */
static int same_timeline(const Timeline *a, const Timeline *b) {
  return b->passes > 0 && b->passes <= a->passes && memcmp(a->t_ms, b->t_ms, b->passes * sizeof(double)) == 0 &&
         memcmp(a->slot, b->slot, b->passes * sizeof(a->slot[0])) == 0;
}

static void fade(void) {
  static const uint32_t rates[] = OMX_ORACLE_FULL_RATES_INIT;
  static Timeline base, other;
  static const uint32_t quanta[] = {32u, 64u, 128u, 256u, 512u, 1024u};
  for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
    const uint32_t rate = rates[r];
    run_fbs(rate, 1024u, OMX_FBS_LV2_CUT, 90.0, 0, &base);
    check(!isnan(base.planted_db) && base.planted_db <= -6.0, "fade", "a growing ring plants a live cut", rate);
    check(base.slot[base.passes > 0 ? base.passes - 1 : 0][0].on == 0.0f, "fade", "the band is lifted by the end",
          rate);
    check(fabs(base.cut_measured_db - base.cut_reported_db) <= 0.5, "fade",
          "the cut measured on the audio at 1 kHz is the reported depth within 0.5 dB",
          base.cut_measured_db - base.cut_reported_db);
    check(base.last_ring_ms >= 3000.0 && base.last_ring_ms <= 3300.0, "fade",
          "the ring stops being heard within three passes of its end", base.last_ring_ms);
    int closed = 1, stages = 0;
    double worst = 0.0;
    for (uint32_t p = 0; p < base.passes; p++) {
      if (base.t_ms[p] <= base.last_ring_ms) continue;
      const double want = fade_at(base.planted_db, base.last_ring_ms, base.t_ms[p]);
      const OmxBandReport got = base.slot[p][0];
      if (isnan(want)) {
        closed &= report_free(got);
        continue;
      }
      const double d = fabs((double)got.depth_db - want);
      if (d > worst) worst = d;
      closed &= got.on == 1.0f && d <= 1e-4;
      if (p > 0 && got.depth_db != base.slot[p - 1][0].depth_db) stages++;
    }
    check(closed, "fade", "the release is the staged fade's closed form, pass for pass", worst);
    check(stages >= 30, "fade", "the release moved through its ramps", stages);
    const unsigned nq = rate == 48000u ? 6u : 1u;
    for (unsigned qi = 0; qi < nq; qi++) {
      run_fbs(rate, rate == 48000u ? quanta[qi] : 32u, OMX_FBS_LV2_CUT, 20.0, 0, &other);
      check(same_timeline(&base, &other), "fade", "the timeline is the same bit for bit at every quantum",
            rate == 48000u ? quanta[qi] : 32u);
    }
  }
  /* report: the audio bit-exact and the same timeline as cut */
  run_fbs(48000u, 128u, OMX_FBS_LV2_CUT, 20.0, 0, &base);
  run_fbs(48000u, 128u, OMX_FBS_LV2_REPORT, 20.0, 0, &other);
  check(!base.bit_exact, "report", "omx-fbs in cut changes the audio", 0);
  check(other.bit_exact, "report", "omx-fbs in report passes the audio bit-exact for the whole run", 0);
  check(same_timeline(&base, &other), "report", "omx-fbs in report reports what cut plants, pass for pass", 0);
}

/* ---- slots keep their finds ------------------------------------------------------------------------------ */

static void fbs_slots(void) {
  static Timeline tl;
  run_fbs(48000u, 256u, OMX_FBS_LV2_CUT, 90.0, 1, &tl);
  int f0_at_0 = 0, f1_at_1 = 0, moved = 0, lifted = 0;
  for (uint32_t p = 0; p < tl.passes; p++) {
    const OmxBandReport a = tl.slot[p][0], b = tl.slot[p][1];
    if (a.on == 1.0f && fabs(a.freq_hz - F0) < 10.0) f0_at_0 = 1;
    if (b.on == 1.0f && fabs(b.freq_hz - F1) < 10.0) f1_at_1 = 1;
    if (f1_at_1 && !(b.on == 1.0f && fabs(b.freq_hz - F1) < 10.0)) moved = 1;
    if (f0_at_0 && report_free(a)) lifted = 1;
    if (a.on == 1.0f && fabs(a.freq_hz - F1) < 10.0) moved = 1;
  }
  check(f0_at_0 && f1_at_1, "slots", "omx-fbs plants the first ring in slot 0 and the second in slot 1", 0);
  check(lifted, "slots", "omx-fbs lifts the first ring and frees slot 0", 0);
  check(!moved, "slots", "omx-fbs keeps the second ring in slot 1 through the first one's release", 0);
}

static void hrp_slots_and_report(void) {
  const uint32_t rate = 48000u;
  OmxHrpLv2 *h = calloc(1, sizeof(*h));
  float *block = malloc(omx_hrp_lv2_block_floats(rate) * sizeof(float));
  omx_hrp_lv2_init_block(h, rate, block);
  /* two bands in flight on the worker: slot 0 one step from free, slot 1 deep */
  h->slot[0].voice_id = 1;
  h->slot[0].harmonic = 2;
  h->slot[0].freq_hz = 440.0f;
  h->slot[0].applied_db = OMX_HRP_GAIN_STEP_DB;
  h->slot[1].voice_id = 2;
  h->slot[1].harmonic = 3;
  h->slot[1].freq_hz = 990.0f;
  h->slot[1].applied_db = 6.0f;
  float in[512], out[512];
  for (uint32_t i = 0; i < 512u; i++) in[i] = 0.0f;
  while (!omx_hrp_lv2_due(h, 0)) omx_hrp_lv2_run(h, in, out, 512u);
  static OmxHrpLv2Set set;
  omx_hrp_lv2_analyse(h, 1, &set); /* bypassed: both walk out one step */
  check(set.count == 1u, "slots", "omx-hrp's walked-out band leaves one in the cascade", set.count);
  check(report_free(set.report[0]), "slots", "omx-hrp frees slot 0 in the report", set.report[0].freq_hz);
  check(set.report[1].freq_hz == 990.0f && set.report[1].on == 1.0f &&
            set.report[1].depth_db == -(6.0f - OMX_HRP_GAIN_STEP_DB) && set.report[1].q == (float)OMX_HRP_BAND_Q &&
            (int)set.report[1].origin == OMX_CURVE_ORIGINS_HRP && (int)set.report[1].shape == OMX_CURVE_SHAPES_BELL,
        "slots", "omx-hrp keeps slot 1's six words in slot 1", set.report[1].freq_hz);
  check(set.q[0] == (float)OMX_HRP_BAND_Q, "slots", "omx-hrp's set carries the band's Q", set.q[0]);
  check(omx_hrp_lv2_analysing(h) == 0, "report", "omx-hrp is not analysing before an adopt", 1);
  omx_hrp_lv2_adopt(h, &set);
  check(omx_hrp_lv2_analysing(h) == 1, "report", "omx-hrp reports analysing once a pass is adopted", 0);
  const OmxBandReport r1 = omx_hrp_lv2_report(h, 1);
  check(memcmp(&r1, &set.report[1], sizeof(r1)) == 0, "report", "omx-hrp's report reads slot 1 by value", r1.freq_hz);
  /* report: a band adopted, a sine at it; cut changes the audio, report does not */
  double e_in = 0.0, e_cut = 0.0;
  int exact = 1;
  for (int pass = 0; pass < 2; pass++) {
    omx_hrp_lv2_set_mode(h, pass == 0 ? 0.0f : 1.0f);
    for (uint32_t b = 0; b < 200u; b++) {
      for (uint32_t i = 0; i < 512u; i++) in[i] = 0.5f * sinf(2.0f * 3.14159265f * 990.0f * (float)(b * 512u + i) / rate);
      omx_hrp_lv2_run(h, in, out, 512u);
      if (pass == 1) exact &= memcmp(in, out, sizeof(in)) == 0;
      if (pass == 0 && b >= 100u)
        for (uint32_t i = 0; i < 512u; i++) {
          e_in += (double)in[i] * in[i];
          e_cut += (double)out[i] * out[i];
        }
    }
  }
  const double cut_db = 10.0 * log10(e_cut / e_in);
  check(fabs(cut_db - set.report[1].depth_db) <= 0.5, "report",
        "omx-hrp in cut cuts the sine at the reported depth within 0.5 dB", cut_db);
  check(exact, "report", "omx-hrp in report passes the audio bit-exact", 0);
  free(block);
  free(h);
}

int main(void) {
  no_worker();
  mode_words();
  fade();
  fbs_slots();
  hrp_slots_and_report();
  printf("live_shells: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
