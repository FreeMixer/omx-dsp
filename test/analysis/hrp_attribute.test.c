// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Standalone unit test for HRP spectral attribution (hrp_attribute.h):
 *   cc -Wall -Wextra -O2 -o /tmp/hrp_attr_test src/hrp_attribute.test.c -lm && /tmp/hrp_attr_test
 * (also driven from `pnpm test` via the test:dsp script).
 *
 * Covers §40's list — isolated, overlapping, nearly coincident and ambiguous components — and the
 * rule that governs all of them: ambiguous attribution must REDUCE confidence rather than pick a
 * winner (§11, §54).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_attribute.h>

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

#define NBINS 2048
static float g_mag[NBINS];
static const float BIN_HZ = 48000.0f / 4096.0f;

static void spectrum_clear(void) { memset(g_mag, 0, sizeof(g_mag)); }

static void place(float hz, float amp) {
  int b = (int)(hz / BIN_HZ + 0.5f);
  if (b < 0 || b >= NBINS) return;
  if (amp > g_mag[b]) g_mag[b] = amp;
}

static void place_series(float f0, uint32_t harmonics, float amp) {
  for (uint32_t n = 1; n <= harmonics; n++) place((float)n * f0, amp / (float)n);
}

static OmxHrpVoiceSet voices_of(const float *hz, uint32_t n) {
  OmxHrpVoiceSet v;
  memset(&v, 0, sizeof(v));
  for (uint32_t i = 0; i < n && i < OMX_HRP_MAX_VOICES; i++) {
    v.voices[i].f0_hz = hz[i];
    v.voices[i].confidence = 0.9f;
    v.voices[i].valid = 1;
    v.voice_count++;
  }
  return v;
}

static const float A4 = 440.0f;
static const float E5 = 659.255f;
static const float Cs5 = 554.365f; /* C#5 — no low-order harmonic relation to A4 */

/* ---- §9/§10: the grid and its measurement ------------------------------------------------- */

static void test_grid_is_n_times_f0(void) {
  spectrum_clear();
  place_series(A4, 8, 1.0f);
  OmxHrpVoiceSet v = voices_of(&A4, 1);
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &v, &c, g);

  check(g[0].count > 0, "a voice's grid is measured");
  const OmxHrpPartial *h1 = omx_hrp_partial_of(&g[0], 1);
  const OmxHrpPartial *h4 = omx_hrp_partial_of(&g[0], 4);
  check(h1 && fabsf(h1->freq_hz - A4) < 0.01f, "harmonic 1 IS the fundamental, not 2*f0");
  check(h4 && fabsf(h4->freq_hz - 4.0f * A4) < 0.01f, "harmonic 4 sits at 4*f0 = 1760 Hz");
  check(h1 && h1->present, "a sounding fundamental is present");
  check(h1 && h4 && h1->level_db > h4->level_db, "a 1/n series falls off — H1 is above H4");
}

static void test_absent_harmonics_are_absent_not_quiet(void) {
  /* "Nothing at H7" and "H7 is very quiet" are different facts. Only the first should stop a
     correction, so they must not be reported the same way. */
  spectrum_clear();
  place(A4, 1.0f);
  place(2.0f * A4, 0.5f);
  place(3.0f * A4, 0.3f);
  OmxHrpVoiceSet v = voices_of(&A4, 1);
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &v, &c, g);

  const OmxHrpPartial *h2 = omx_hrp_partial_of(&g[0], 2);
  const OmxHrpPartial *h7 = omx_hrp_partial_of(&g[0], 7);
  check(h2 && h2->present, "a harmonic that sounds is present");
  check(h7 && !h7->present, "a harmonic that does not sound is ABSENT");
  check(h7 && h7->confidence == 0.0f, "an absent partial carries no confidence — there is nothing to be sure of");
  check(h7 && h7->level_db < -100.0f, "…and reads as silence, not as a small number");
}

/* ---- §11: collisions --------------------------------------------------------------------- */

static void test_isolated_partials_are_unambiguous(void) {
  /* A4 and C#5 share no low-order harmonics, so every partial belongs to exactly one voice. */
  spectrum_clear();
  place_series(A4, 6, 1.0f);
  place_series(Cs5, 6, 1.0f);
  float pair[] = {A4, Cs5};
  OmxHrpVoiceSet v = voices_of(pair, 2);
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &v, &c, g);

  const OmxHrpPartial *a1 = omx_hrp_partial_of(&g[0], 1);
  check(a1 && a1->present && !a1->ambiguous, "A4's own fundamental belongs to A4 alone");
  check(a1 && a1->confidence > OMX_HRP_AMBIGUOUS_CONFIDENCE,
        "an unambiguous partial keeps its voice's confidence");
  check(omx_hrp_unambiguous_count(&g[0]) > 0, "A4 has partials a correction could act on");
}

static void test_coincident_partials_are_marked_ambiguous(void) {
  /* THE case §11 exists for. A4 and E5 are a fifth apart, so A4's H3 (1320.0 Hz) and E5's H2
     (1318.5 Hz) are TWO CENTS apart — one spectral event that both voices predict. Cutting it
     because one voice looks loud there attenuates a note nobody complained about. */
  spectrum_clear();
  place_series(A4, 8, 1.0f);
  place_series(E5, 8, 1.0f);
  float pair[] = {A4, E5};
  OmxHrpVoiceSet v = voices_of(pair, 2);
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &v, &c, g);

  const OmxHrpPartial *a3 = omx_hrp_partial_of(&g[0], 3); /* A4 H3 = 1320.0 */
  const OmxHrpPartial *e2 = omx_hrp_partial_of(&g[1], 2); /* E5 H2 = 1318.5 */
  check(a3 && a3->present, "A4's H3 is measured");
  check(e2 && e2->present, "E5's H2 is measured");
  check(a3 && a3->ambiguous, "A4's H3 is AMBIGUOUS — E5's H2 is in the same place");
  check(e2 && e2->ambiguous, "and so is E5's H2; ambiguity is symmetric");
  check(a3 && a3->confidence <= OMX_HRP_AMBIGUOUS_CONFIDENCE,
        "an ambiguous partial's confidence is REDUCED, so §54 declines the cut");
  check(a3 && a3->confidence > 0.0f,
        "…but not to zero: the partial is real and the analyser must still show it");

  const OmxHrpPartial *a1 = omx_hrp_partial_of(&g[0], 1);
  check(a1 && !a1->ambiguous, "A4's own fundamental is still unambiguously A4's");
}

static void test_ambiguity_does_not_erase_the_voice(void) {
  /* Sharing some partials must not make a voice uncorrectable everywhere — only where it shares. */
  spectrum_clear();
  place_series(A4, 8, 1.0f);
  place_series(E5, 8, 1.0f);
  float pair[] = {A4, E5};
  OmxHrpVoiceSet v = voices_of(pair, 2);
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &v, &c, g);
  check(omx_hrp_unambiguous_count(&g[0]) > 0,
        "a voice sharing some partials still has ones of its own to act on");
}

static void test_octave_pair_shares_everything_audible(void) {
  /* A4 and A5: every partial of the upper voice within the lower voice's AUDIBLE grid is shared,
   * because A5's Hk is always A4's H2k. A correction tier reading these confidences declines
   * across that whole region, which is the right outcome rather than a failure.
   *
   * It does NOT follow that the upper voice owns nothing. Presence is judged against each voice's
   * OWN mean partial — self-relative, so a quiet voice in a loud chord is held to its own shape —
   * and the two means differ. MEASURED here: 4400 Hz carries the same energy for both, and is
   * PRESENT for A5 (mean 0.204, threshold 0.123) while ABSENT for A4 (mean 0.344, threshold
   * 0.206). A4 makes no claim on a partial buried below its own series, so 4400 is A5's alone —
   * and cutting it cannot damage an A4 harmonic nobody can hear.
   *
   * The first draft of this test asserted "owns NO partial of its own" and was simply wrong.
   */
  spectrum_clear();
  place_series(A4, 8, 1.0f);
  place_series(880.0f, 6, 1.0f);
  float pair[] = {A4, 880.0f};
  OmxHrpVoiceSet v = voices_of(pair, 2);
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &v, &c, g);

  /* Everything the upper voice has inside the lower one's audible range is shared. */
  for (uint32_t n = 1; n <= 4; n++) {
    const OmxHrpPartial *p = omx_hrp_partial_of(&g[1], n);
    check(p && p->present && p->ambiguous,
          "an octave voice's low partials are ALL shared with the voice below it");
  }
  const OmxHrpPartial *lower_h1 = omx_hrp_partial_of(&g[0], 1);
  check(lower_h1 && !lower_h1->ambiguous,
        "the LOWER voice still owns its fundamental — the upper one does not reach it");
  const OmxHrpPartial *lower_h3 = omx_hrp_partial_of(&g[0], 3);
  check(lower_h3 && lower_h3->present && !lower_h3->ambiguous,
        "and owns its ODD harmonics, which an octave above can never predict");
}

/* ---- contract ---------------------------------------------------------------------------- */

static void test_single_voice_is_never_ambiguous(void) {
  spectrum_clear();
  place_series(A4, 8, 1.0f);
  OmxHrpVoiceSet v = voices_of(&A4, 1);
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &v, &c, g);
  for (uint32_t i = 0; i < g[0].count; i++)
    check(!g[0].partials[i].ambiguous, "with one voice sounding, nothing is ambiguous");
}

static void test_eight_voices_are_attributed_apart(void) {
  /* THE FULL BUDGET, attributed. OMX_HRP_MAX_VOICES moved 4 -> 8 (2026-09-01), and the grids are
     what the correction tier actually reads — a set that DETECTS eight voices but attributes
     four is a raised ceiling that plants nothing above the fourth.

     Eight tones a semitone apart, each sounding its own three partials at levels that identify
     it: voice k's series is scaled by `1 - k/16`, so the level read at n·f0_k could only have
     come from voice k. The assertion is per voice AND per harmonic — that grid v's partial n
     sits at n·f0_v and reads voice v's own level — which is what "attributed apart" means and
     what a grid that quietly reused voice 0's f0 would fail. */
  const uint32_t VOICES = OMX_HRP_MAX_VOICES;
  float f0[OMX_HRP_MAX_VOICES];
  float amp[OMX_HRP_MAX_VOICES];
  spectrum_clear();
  for (uint32_t k = 0; k < VOICES; k++) {
    f0[k] = A4 * powf(2.0f, (float)k / 12.0f);
    amp[k] = 1.0f - (float)k / 16.0f;
    place_series(f0[k], 3, amp[k]);
  }

  OmxHrpVoiceSet v = voices_of(f0, VOICES);
  check(v.voice_count == VOICES, "the set carries all eight voices into attribution");
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  memset(g, 0, sizeof(g));
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &v, &c, g);

  for (uint32_t k = 0; k < VOICES; k++) {
    check(g[k].count > 0, "every one of the eight voices gets a measured grid");
    const OmxHrpPartial *h1 = omx_hrp_partial_of(&g[k], 1);
    const OmxHrpPartial *h2 = omx_hrp_partial_of(&g[k], 2);
    check(h1 && fabsf(h1->freq_hz - f0[k]) < 0.01f, "voice k's H1 is voice k's OWN fundamental");
    check(h2 && fabsf(h2->freq_hz - 2.0f * f0[k]) < 0.01f, "voice k's H2 is 2 x its own f0");
    /* The level identifies the voice: 20·log10(amp_k) for H1, and H2 is half of it (1/n). */
    check(h1 && fabsf(h1->level_db - 20.0f * log10f(amp[k])) < 0.5f,
          "voice k's H1 reads voice k's own level, not a louder neighbour's");
    check(h1 && h1->present, "every sounding fundamental of the eight is present");
  }

  /* A semitone is 100 cents and the collision window is 60, so eight fundamentals a semitone
     apart are each unambiguous — the discriminating half of this arm, since a grid that credited
     everything to everyone would mark them all ambiguous and still "find" eight voices. */
  uint32_t clean = 0;
  for (uint32_t k = 0; k < VOICES; k++) {
    const OmxHrpPartial *h1 = omx_hrp_partial_of(&g[k], 1);
    if (h1 && !h1->ambiguous) clean++;
  }
  check(clean == VOICES, "eight fundamentals a semitone apart are attributed to eight voices");
}

static void test_degenerate_inputs(void) {
  OmxHrpVoiceSet v = voices_of(&A4, 1);
  OmxHrpConfig c = omx_hrp_config_default();
  OmxHrpVoiceHarmonics g[OMX_HRP_MAX_VOICES];
  memset(g, 0xAB, sizeof(g));
  omx_hrp_attribute(NULL, NBINS, BIN_HZ, &v, &c, g);
  omx_hrp_attribute(g_mag, 0, BIN_HZ, &v, &c, g);
  omx_hrp_attribute(g_mag, NBINS, 0.0f, &v, &c, g);
  check(1, "degenerate inputs return without touching memory they were not given");

  OmxHrpVoiceSet none;
  memset(&none, 0, sizeof(none));
  omx_hrp_attribute(g_mag, NBINS, BIN_HZ, &none, &c, g);
  check(1, "an empty voice set attributes nothing and does not crash");
}

int main(void) {
  test_grid_is_n_times_f0();
  test_absent_harmonics_are_absent_not_quiet();
  test_isolated_partials_are_unambiguous();
  test_coincident_partials_are_marked_ambiguous();
  test_ambiguity_does_not_erase_the_voice();
  test_octave_pair_shares_everything_audible();
  test_single_voice_is_never_ambiguous();
  test_eight_voices_are_attributed_apart();
  test_degenerate_inputs();

  printf("hrp_attribute: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
