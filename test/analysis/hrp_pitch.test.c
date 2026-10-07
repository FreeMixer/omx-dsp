// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Standalone unit test for the HRP multi-f0 engine (hrp_pitch.h). No PipeWire, no node:
 *   cc -Wall -Wextra -O2 -o /tmp/hrp_pitch_test src/hrp_pitch.test.c -lm && /tmp/hrp_pitch_test
 * (also driven from `pnpm test` via the test:dsp script).
 *
 * The spectra are BUILT, not captured, which is what makes these oracles rather than
 * observations: a partial is placed at a frequency this file chose, so the answer the detector
 * should give is known exactly, and a wrong answer is wrong against arithmetic rather than
 * against a recording somebody has to trust.
 *
 * Covers the list the spec names — §37 (sine, harmonic series, missing fundamental, strong H2,
 * strong H3, low notes, noise, amplitude variation), §38 (simultaneous voices up to the budget —
 * §7's four, raised to eight by the operator's polyphony sizing of 2026-09-01 — overlap,
 * competing fundamentals, octave ambiguity) and §55 (the octave errors that are "especially
 * dangerous", because every harmonic index shifts with them).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_pitch.h>

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

/* A tolerance in CENTS, not hertz: the sweep is geometric, so its worst-case error is a constant
   fraction of the frequency rather than a constant number of hertz. Half a semitone is 50 cents;
   anything inside 40 identifies the note beyond doubt. */
static void near_note(float got_hz, float want_hz, float tolerance_cents, const char *what) {
  g_checks++;
  if (!(got_hz > 0.0f) || !(want_hz > 0.0f)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s — got %.3f Hz want %.3f Hz\n", what, got_hz, want_hz);
    return;
  }
  float cents = 1200.0f * log2f(got_hz / want_hz);
  if (fabsf(cents) > tolerance_cents) {
    g_fail++;
    fprintf(stderr, "FAIL: %s — got %.3f Hz want %.3f Hz (%.1f cents off)\n",
            what, got_hz, want_hz, cents);
  }
}

/* ---- spectrum construction ------------------------------------------------------------- */

#define NBINS 2048
static float g_mag[NBINS];
static float g_scratch[NBINS];

static void spectrum_clear(void) { memset(g_mag, 0, sizeof(g_mag)); }

/* Put one partial at `hz`. The nearest bin takes the amplitude, matching what a windowed FFT
   reports as the peak of a partial that lands on a bin centre. */
static void place(float bin_hz, float hz, float amp) {
  int b = (int)(hz / bin_hz + 0.5f);
  if (b < 0 || b >= NBINS) return;
  if (amp > g_mag[b]) g_mag[b] = amp;
}

/* A harmonic series with 1/n amplitude falloff — a sawtooth's envelope, and close enough to a
   bowed or blown instrument for the detector's purposes. */
static void place_series(float bin_hz, float f0, uint32_t harmonics, float amp) {
  for (uint32_t n = 1; n <= harmonics; n++) place(bin_hz, (float)n * f0, amp / (float)n);
}

static OmxHrpVoiceSet detect(float bin_hz) {
  OmxHrpVoiceSet out;
  OmxHrpConfig cfg = omx_hrp_config_default();
  omx_hrp_detect(g_mag, NBINS, bin_hz, &cfg, g_scratch, &out);
  return out;
}

/* 48 kHz / 4096-point FFT — the resolution the RTA's upper tier runs at. */
static const float BIN_HZ = 48000.0f / 4096.0f;
/* The decimated low tier, for notes whose fundamental is only a few bins up at the rate above. */
static const float BIN_HZ_LOW = 48000.0f / 16384.0f;

/* ---- §37: the single-voice list ---------------------------------------------------------- */

static void test_pure_sine(void) {
  /* A sine has ONE partial. The comb at f0/2 predicts that same partial as its H2 and scores
     identically on the shared term — so a guard that compares total comb scores drops a clean
     440 Hz sine to 220. This arm exists because that is exactly what the first draft did. */
  spectrum_clear();
  place(BIN_HZ, 440.0f, 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 1, "a pure sine is a voice");
  if (v.voice_count >= 1) {
    near_note(v.voices[0].f0_hz, 440.0f, 40.0f, "a 440 Hz sine is heard as 440, NOT as 220");
    check(v.voices[0].midi_note == 69, "440 Hz is MIDI 69 (A4)");
    check(v.voices[0].valid == 1, "a detected voice is marked valid");
  }
}

static void test_harmonic_series(void) {
  spectrum_clear();
  place_series(BIN_HZ, 220.0f, 10, 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 1, "a harmonic series is a voice");
  if (v.voice_count >= 1) near_note(v.voices[0].f0_hz, 220.0f, 40.0f, "a full series reads its f0");
}

static void test_missing_fundamental(void) {
  /* §55's headline case: the note is 220 but nothing is AT 220. The partials present are its
     H2..H8. Reading the lowest partial as the fundamental would call this 440 and shift every
     harmonic index by an octave — a correction aimed at H4 would land on H8. */
  spectrum_clear();
  for (uint32_t n = 2; n <= 8; n++) place(BIN_HZ, (float)n * 220.0f, 1.0f / (float)n);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 1, "a missing fundamental is still a voice");
  if (v.voice_count >= 1)
    near_note(v.voices[0].f0_hz, 220.0f, 40.0f,
              "a missing fundamental is recovered as 220, not mis-read as 440");
}

static void test_strong_h2(void) {
  /* H2 louder than H1. The peak of the spectrum is NOT the fundamental. */
  spectrum_clear();
  place(BIN_HZ, 220.0f, 0.3f);
  place(BIN_HZ, 440.0f, 1.0f);
  place(BIN_HZ, 660.0f, 0.4f);
  place(BIN_HZ, 880.0f, 0.3f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 1, "strong H2: still a voice");
  if (v.voice_count >= 1)
    near_note(v.voices[0].f0_hz, 220.0f, 40.0f, "a dominant H2 does not become the fundamental");
}

static void test_strong_h3(void) {
  spectrum_clear();
  place(BIN_HZ, 220.0f, 0.3f);
  place(BIN_HZ, 440.0f, 0.3f);
  place(BIN_HZ, 660.0f, 1.0f);
  place(BIN_HZ, 880.0f, 0.3f);
  place(BIN_HZ, 1100.0f, 0.3f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 1, "strong H3: still a voice");
  if (v.voice_count >= 1)
    near_note(v.voices[0].f0_hz, 220.0f, 40.0f, "a dominant H3 does not become the fundamental");
}

static void test_low_note(void) {
  /* E1 = 41.2 Hz. At the upper tier's 11.7 Hz bins its fundamental is bin 3.5 and the note is
     unresolvable; this is why the RTA has a decimated low tier at all. */
  spectrum_clear();
  place_series(BIN_HZ_LOW, 41.2f, 12, 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ_LOW);
  check(v.voice_count >= 1, "a low E1 is a voice on the low tier");
  if (v.voice_count >= 1) near_note(v.voices[0].f0_hz, 41.2f, 45.0f, "E1 reads as 41.2 Hz");
}

static void test_noise_is_not_a_voice(void) {
  /* Broadband energy with no harmonic structure. The honest answer is "nothing pitched", and the
     dangerous answer is a confident wrong one — this is the arm that keeps HRP silent on a
     cymbal, a crowd, or a hiss. */
  spectrum_clear();
  for (uint32_t b = 10; b < 800; b++) g_mag[b] = 0.05f;
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count == 0, "flat broadband noise yields NO voice");
}

static void test_silence(void) {
  spectrum_clear();
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count == 0, "digital silence yields no voice");
}

static void test_amplitude_variation(void) {
  /* The same note at two levels: the pitch must not move, and the reported amplitude must. */
  spectrum_clear();
  place_series(BIN_HZ, 330.0f, 8, 1.0f);
  OmxHrpVoiceSet loud = detect(BIN_HZ);
  spectrum_clear();
  place_series(BIN_HZ, 330.0f, 8, 0.1f);
  OmxHrpVoiceSet quiet = detect(BIN_HZ);

  check(loud.voice_count >= 1 && quiet.voice_count >= 1, "the note is found at both levels");
  if (loud.voice_count >= 1 && quiet.voice_count >= 1) {
    near_note(quiet.voices[0].f0_hz, loud.voices[0].f0_hz, 5.0f,
              "20 dB quieter is the same pitch");
    check(quiet.voices[0].amplitude_db < loud.voices[0].amplitude_db - 15.0f,
          "the reported amplitude follows the level down");
  }
}

/* The production resolution: 96 kHz / 16384-point — 5.86 Hz bins (mix_dsp.h derives it). */
static const float BIN_HZ_PROD = 96000.0f / 16384.0f;

static void test_salience_is_the_share_of_pitched_content(void) {
  /* A lone voice owns ALL the pitched content — salience 1.0 — however much unpitched noise
     surrounds it (the number a clearly-heard solo over an orchestra bed deserves, which raw
     confidence structurally cannot give it). Two voices split it by their explained energy. */
  spectrum_clear();
  place_series(BIN_HZ, 220.0f, 8, 1.0f);
  for (int b = 900; b < 1100; b++) g_mag[b] = 0.02f; /* an unpitched bed */
  OmxHrpVoiceSet solo = detect(BIN_HZ);
  check(solo.voice_count == 1, "salience: one voice over a noise bed");
  if (solo.voice_count == 1) {
    check(solo.voices[0].salience > 0.999f, "a lone voice's salience is 1.0 — all the pitched content");
    check(solo.voices[0].confidence < 0.9f, "while raw confidence is capped by the bed's share");
  }

  spectrum_clear();
  place_series(BIN_HZ, 220.0f, 8, 1.0f);
  place_series(BIN_HZ, 330.0f, 8, 0.5f);
  OmxHrpVoiceSet duet = detect(BIN_HZ);
  check(duet.voice_count == 2, "salience: a duet is two voices");
  if (duet.voice_count == 2) {
    float sum = duet.voices[0].salience + duet.voices[1].salience;
    check(sum > 0.999f && sum < 1.001f, "saliences over one frame sum to 1");
    check(duet.voices[0].salience > duet.voices[1].salience,
          "the louder voice carries the larger share");
  }
}

static void test_vibrato_displaced_partials_still_count(void) {
  /* A REAL note is not a bin-aligned comb: vibrato and inharmonicity displace partial n by a
     frequency PROPORTIONAL to n. At 25 cents sharp, H6 of A#2 (~700 Hz) sits ~10 Hz — nearly
     two production bins — off the comb's prediction, and a fixed ±1-bin read scores it absent.
     The reading tolerance must scale with the partial's own frequency, or confidence collapses
     exactly on expressive playing (measured live, trombone, 2026-08-18). */
  const float f0 = 116.54f; /* A#2 */
  const float sharp = powf(2.0f, 25.0f / 1200.0f);

  spectrum_clear();
  place_series(BIN_HZ_PROD, f0, 10, 1.0f);
  OmxHrpVoiceSet aligned = detect(BIN_HZ_PROD);
  check(aligned.voice_count >= 1, "aligned series: a voice");
  float conf_aligned = aligned.voice_count >= 1 ? aligned.voices[0].confidence : 0.0f;

  spectrum_clear();
  for (uint32_t n = 1; n <= 10; n++) {
    /* Low partials near the comb, uppers displaced as vibrato displaces them. */
    float hz = (float)n * f0 * (n >= 3 ? sharp : 1.0f);
    place(BIN_HZ_PROD, hz, 1.0f / (float)n);
  }
  /* The unit seam itself: the reading of H6 (comb predicts ~699 Hz, the real partial sits
     ~10 Hz — 1.8 bins — sharp) must FIND that peak; a fixed +-1-bin read answers zero. */
  check(omx_hrp_bin_peak(g_mag, NBINS, BIN_HZ_PROD, 6.0f * f0) > 0.1f,
        "a partial 1.8 bins sharp is still read as this harmonic's level");

  OmxHrpVoiceSet vib = detect(BIN_HZ_PROD);
  check(vib.voice_count >= 1, "vibrato-displaced series: still a voice");
  if (vib.voice_count >= 1) {
    near_note(vib.voices[0].f0_hz, f0, 40.0f, "vibrato-displaced series still reads its note");
    /* The displaced uppers carry real energy of THIS note; the score must keep most of it.
       Under a fixed +-1-bin read the displaced case loses every partial from H3 up. */
    check(vib.voices[0].confidence > 0.9f * conf_aligned,
          "displaced partials still credit the note (tolerance scales with frequency)");
  }
}

/* ---- §38: polyphony ---------------------------------------------------------------------- */

/* Equal temperament from A4. */
static const float A4 = 440.0f;
static const float C5 = 523.251f;
static const float E5 = 659.255f;
static const float G5 = 783.991f;

/* Is `hz` among the detected voices, within tolerance? */
static int found(const OmxHrpVoiceSet *v, float hz, float tolerance_cents) {
  for (uint32_t i = 0; i < v->voice_count; i++) {
    if (v->voices[i].f0_hz <= 0.0f) continue;
    if (fabsf(1200.0f * log2f(v->voices[i].f0_hz / hz)) <= tolerance_cents) return 1;
  }
  return 0;
}

static void test_one_voice(void) {
  spectrum_clear();
  place_series(BIN_HZ, A4, 8, 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count == 1, "one note yields exactly ONE voice, not eight imaginary ones");
  check(found(&v, A4, 40.0f), "A4 found");
}

static void test_two_voices(void) {
  spectrum_clear();
  place_series(BIN_HZ, A4, 8, 1.0f);
  place_series(BIN_HZ, C5, 8, 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 2, "A4 + C5 yields at least two voices");
  check(found(&v, A4, 40.0f), "A4 found in the dyad");
  check(found(&v, C5, 40.0f), "C5 found in the dyad");
}

static void test_three_voices(void) {
  spectrum_clear();
  place_series(BIN_HZ, A4, 8, 1.0f);
  place_series(BIN_HZ, C5, 8, 1.0f);
  place_series(BIN_HZ, E5, 8, 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 3, "A4 + C5 + E5 yields at least three voices");
  check(found(&v, A4, 40.0f), "A4 found in the triad");
  check(found(&v, C5, 40.0f), "C5 found in the triad");
  check(found(&v, E5, 40.0f), "E5 found in the triad");
}

static void test_four_voices(void) {
  /* KNOWN LIMITATION, pinned deliberately rather than tuned away — see the increment 3+4 report.
   *
   * A4 + C5 + E5 + G5 is not four independent notes to a greedy estimate-and-cancel search: C5 is
   * exactly H2 of C4 and G5 is exactly H3 of it, so the pair collapses into their common
   * subharmonic at ~262 Hz. The arithmetic is not wrong. Separating that hypothesis from a real
   * missing-fundamental C4 needs the WHOLE voice set scored at once, which this architecture does
   * not do and which §10 rules out for now ("do NOT implement full source separation").
   *
   * What is asserted here is what must remain true regardless: every pitch reported is real
   * musical content of this chord — a note of it, or the exact common subharmonic of two of them
   * — and A4 and E5, which stand in no such relation to the others, are still found. Nothing is
   * invented. When joint scoring lands, this test SHOULD go red and be rewritten to demand four.
   */
  spectrum_clear();
  place_series(BIN_HZ, A4, 8, 1.0f);
  place_series(BIN_HZ, C5, 8, 1.0f);
  place_series(BIN_HZ, E5, 8, 1.0f);
  place_series(BIN_HZ, G5, 8, 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 3, "a four-note chord yields at least three voices");
  check(found(&v, A4, 40.0f), "A4 found in the tetrad — it is nobody's harmonic here");
  check(found(&v, E5, 40.0f), "E5 found in the tetrad — it is nobody's harmonic here");
  /* No invented pitches: everything reported is a note of the chord or the C4 the C5/G5 pair
     collapses to. A detector that starts naming frequencies outside this set has a real bug. */
  for (uint32_t i = 0; i < v.voice_count; i++) {
    float f = v.voices[i].f0_hz;
    int legitimate = found(&v, A4, 40.0f) &&
                     (fabsf(1200.0f * log2f(f / A4)) <= 40.0f ||
                      fabsf(1200.0f * log2f(f / C5)) <= 40.0f ||
                      fabsf(1200.0f * log2f(f / E5)) <= 40.0f ||
                      fabsf(1200.0f * log2f(f / G5)) <= 40.0f ||
                      fabsf(1200.0f * log2f(f / (C5 * 0.5f))) <= 40.0f);
    check(legitimate, "every reported voice is real content of the chord, never an invention");
  }
}

/* Eight simultaneous tones, one semitone apart from A4 — the chord the raised budget exists for
   (a piano/string voicing sits at eight, a guitar sounds six). Written into `g_mag` by the
   caller's chosen builder so the same cluster can be sounded as bare tones or as full series. */
static void place_cluster8(float bin_hz, int as_series) {
  spectrum_clear();
  for (int i = 0; i < 8; i++) {
    float f0 = A4 * powf(2.0f, (float)i / 12.0f);
    if (as_series) place_series(bin_hz, f0, 3, 1.0f);
    else place(bin_hz, f0, 1.0f);
  }
}

static void test_eight_voices(void) {
  /* THE FULL BUDGET, end to end. OMX_HRP_MAX_VOICES moved 4 -> 8 on 2026-09-01, and a ceiling
     nothing ever reaches is a number, not a capability: this arm makes the search, the set, the
     cancellation and the salience normalisation all carry eight.

     BARE TONES, deliberately. Each voice here owns exactly one partial, so nothing is shared and
     the eight are genuinely independent — which is what isolates the BUDGET as the thing under
     test. The same cluster sounded as harmonic series returns seven, for the reason
     `test_four_voices` documents at length and `test_eight_harmonic_voices` pins below. */
  place_cluster8(BIN_HZ, 0);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count == 8, "an eight-tone chord yields EIGHT voices — the raised budget is real");
  check(v.voice_count == OMX_HRP_MAX_VOICES, "and eight IS the declared maximum");
  for (int i = 0; i < 8; i++) {
    float want = A4 * powf(2.0f, (float)i / 12.0f);
    check(found(&v, want, 40.0f), "every tone of the eight-tone chord is found");
  }
  float salience = 0.0f;
  for (uint32_t i = 0; i < v.voice_count; i++) salience += v.voices[i].salience;
  check(salience > 0.999f && salience < 1.001f, "eight voices' saliences still sum to one");
}

static void test_ninth_tone_does_not_fit(void) {
  /* The ceiling is what stops it, not the material: nine tones, eight slots. Without this the
     arm above would pass just as well on a detector with no limit at all. */
  spectrum_clear();
  for (int i = 0; i < 9; i++) place(BIN_HZ, A4 * powf(2.0f, (float)i / 12.0f), 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count == OMX_HRP_MAX_VOICES, "a ninth tone does not fit — the budget caps it");
}

static void test_eight_harmonic_voices(void) {
  /* KNOWN LIMITATION at eight, the same one `test_four_voices` pins at four, and pinned here so
     a change in it is visible rather than discovered on a piano.

     Eight harmonic series a semitone apart return SEVEN voices, and the first of them is A3 —
     which is not in the chord. A3 is admitted because the cluster manufactures its evidence: A3's
     H2 is the A4 that IS sounding, its H3 (660 Hz) lands within 30 cents of the cluster's top
     note, and its H5 (1100) within 14 cents of another note's H2. Greedy estimate-and-cancel
     cannot refuse that without scoring the whole voice set jointly, which §10 rules out for now.

     What must remain true regardless is asserted: most of the chord is found, nothing invented
     outside the chord and its subharmonics, and the budget is never exceeded. */
  place_cluster8(BIN_HZ, 1);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 6, "an eight-note harmonic cluster still yields at least six voices");
  check(v.voice_count <= OMX_HRP_MAX_VOICES, "and never more than the budget");
  for (uint32_t i = 0; i < v.voice_count; i++) {
    float f = v.voices[i].f0_hz;
    int legitimate = 0;
    for (int k = 0; k < 8 && !legitimate; k++) {
      float note = A4 * powf(2.0f, (float)k / 12.0f);
      /* the note itself, or the octave/twelfth below it — the two descents the guard allows */
      if (fabsf(1200.0f * log2f(f / note)) <= 40.0f) legitimate = 1;
      if (fabsf(1200.0f * log2f(f / (note * 0.5f))) <= 40.0f) legitimate = 1;
      if (fabsf(1200.0f * log2f(f / (note / 3.0f))) <= 40.0f) legitimate = 1;
    }
    check(legitimate, "every reported voice is a note of the cluster or a subharmonic of one");
  }
}

static void test_confidence_floor_is_derived_from_the_budget(void) {
  /* The floor is HALF a voice's fair share, and it is a function so that it follows the budget
     instead of being re-typed beside it (`omx_hrp_min_confidence_for`). */
  check(fabsf(omx_hrp_min_confidence_for(4) - 0.125f) < 1e-6f,
        "four voices: the floor is half a quarter — the 0.12 that used to be typed here");
  check(fabsf(omx_hrp_min_confidence_for(8) - 0.0625f) < 1e-6f, "eight voices: half an eighth");
  check(omx_hrp_config_default().min_confidence == omx_hrp_min_confidence_for(OMX_HRP_MAX_VOICES),
        "the default searches with the floor its own budget derives");

  /* THE OTHER END, MEASURED rather than asserted: run the noise spectrum with the floor removed
     and read what a comb on hiss actually scores. The floor must stand clear of it, or the
     halving has walked the gate into the noise. */
  spectrum_clear();
  for (uint32_t b = 10; b < 800; b++) g_mag[b] = 0.05f;
  OmxHrpConfig open = omx_hrp_config_default();
  open.min_confidence = 0.0f;
  OmxHrpVoiceSet noise;
  omx_hrp_detect(g_mag, NBINS, BIN_HZ, &open, g_scratch, &noise);
  check(noise.voice_count > 0, "with no floor, hiss does score — the control can detect presence");
  float worst = 0.0f;
  for (uint32_t i = 0; i < noise.voice_count; i++)
    if (noise.voices[i].confidence > worst) worst = noise.voices[i].confidence;
  check(worst > 0.0f && worst < 0.02f, "a comb on flat hiss scores ~0.015");
  check(omx_hrp_config_default().min_confidence > 2.0f * worst,
        "the derived floor stands clear of the measured noise score by more than a factor of two");
}

static void test_octave_ambiguity(void) {
  /* A4 and A5 sounding together. Every partial of A5 is also a partial of A4, so the two are
     genuinely hard to separate — the requirement is that the LOWER one is heard (it explains
     everything) and that nothing invents a third voice out of the shared partials. */
  spectrum_clear();
  place_series(BIN_HZ, A4, 10, 1.0f);
  place_series(BIN_HZ, 880.0f, 6, 1.0f);
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 1, "an octave pair is at least one voice");
  check(found(&v, A4, 40.0f), "the LOWER of an octave pair is heard");
}

static void test_competing_fundamentals(void) {
  /* Two unrelated notes, neither a harmonic of the other: a tritone. Nothing is shared, so both
     must survive cancellation. */
  spectrum_clear();
  place_series(BIN_HZ, 261.626f, 8, 1.0f); /* C4 */
  place_series(BIN_HZ, 369.994f, 8, 1.0f); /* F#4 */
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count >= 2, "two unrelated fundamentals both survive");
  check(found(&v, 261.626f, 40.0f), "C4 found");
  check(found(&v, 369.994f, 40.0f), "F#4 found");
}

/* ---- API contract ------------------------------------------------------------------------ */

static void test_api_never_assumes_one_voice(void) {
  /* §5's requirement, asserted structurally: the answer is a SET with a count, and the count is
     the only thing that says how many there are. */
  spectrum_clear();
  OmxHrpVoiceSet v = detect(BIN_HZ);
  check(v.voice_count <= OMX_HRP_MAX_VOICES, "voice_count never exceeds the declared maximum");
  check(sizeof(v.voices) / sizeof(v.voices[0]) == OMX_HRP_MAX_VOICES,
        "the set carries room for MAX_VOICES, whatever this frame found");
}

static void test_degenerate_inputs(void) {
  OmxHrpVoiceSet v;
  OmxHrpConfig cfg = omx_hrp_config_default();

  omx_hrp_detect(NULL, NBINS, BIN_HZ, &cfg, g_scratch, &v);
  check(v.voice_count == 0, "a null spectrum is no voices, not a crash");

  omx_hrp_detect(g_mag, 0, BIN_HZ, &cfg, g_scratch, &v);
  check(v.voice_count == 0, "zero bins is no voices");

  omx_hrp_detect(g_mag, NBINS, 0.0f, &cfg, g_scratch, &v);
  check(v.voice_count == 0, "a zero bin width is no voices");

  omx_hrp_detect(g_mag, NBINS, BIN_HZ, &cfg, NULL, &v);
  check(v.voice_count == 0, "no scratch buffer is no voices — nothing is allocated on our behalf");
}

static void test_max_voices_is_respected(void) {
  /* Ask for two voices out of a four-note chord and get two, not four. */
  spectrum_clear();
  place_series(BIN_HZ, A4, 8, 1.0f);
  place_series(BIN_HZ, C5, 8, 1.0f);
  place_series(BIN_HZ, E5, 8, 1.0f);
  place_series(BIN_HZ, G5, 8, 1.0f);
  OmxHrpVoiceSet v;
  OmxHrpConfig cfg = omx_hrp_config_default();
  cfg.max_voices = 2;
  omx_hrp_detect(g_mag, NBINS, BIN_HZ, &cfg, g_scratch, &v);
  check(v.voice_count == 2, "a caller's voice budget is honoured");
}

static void test_midi_mapping(void) {
  float cents = 99.0f;
  check(omx_hrp_midi_note(440.0f, &cents) == 69, "440 Hz is MIDI 69");
  check(fabsf(cents) < 0.01f, "440 Hz is 0 cents from A4");
  check(omx_hrp_midi_note(880.0f, &cents) == 81, "880 Hz is MIDI 81, an octave up");
  check(omx_hrp_midi_note(261.626f, &cents) == 60, "261.626 Hz is middle C");
  check(omx_hrp_midi_note(-1.0f, &cents) == -1, "a nonsense frequency has no note");

  /* 452 Hz, not 453: 453 is 50.4 cents above A4, which rounds to the NEXT note and comes back as
     −49.6 cents. Naming a value that sits on the rounding boundary tests the boundary, not the
     mapping. */
  omx_hrp_midi_note(452.0f, &cents);
  check(cents > 40.0f && cents < 50.0f, "452 Hz is most of a half-semitone sharp of A4");
}

int main(void) {
  test_pure_sine();
  test_harmonic_series();
  test_missing_fundamental();
  test_strong_h2();
  test_strong_h3();
  test_low_note();
  test_noise_is_not_a_voice();
  test_silence();
  test_amplitude_variation();
  test_vibrato_displaced_partials_still_count();
  test_salience_is_the_share_of_pitched_content();

  test_one_voice();
  test_two_voices();
  test_three_voices();
  test_four_voices();
  test_eight_voices();
  test_ninth_tone_does_not_fit();
  test_eight_harmonic_voices();
  test_confidence_floor_is_derived_from_the_budget();
  test_octave_ambiguity();
  test_competing_fundamentals();

  test_api_never_assumes_one_voice();
  test_degenerate_inputs();
  test_max_voices_is_respected();
  test_midi_mapping();

  printf("hrp_pitch: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
