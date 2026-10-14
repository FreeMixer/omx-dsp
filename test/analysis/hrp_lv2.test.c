// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Standalone unit test for the LIVE LV2 shell's core (hrp_lv2.h) —
 * the recording vsc spec §0.5's third shell, over
 * the hrp polyphonic architecture spec:
 *   cc -Wall -Wextra -Werror -O2 -o build/hrp_lv2_test src/hrp_lv2.test.c -lm && ./build/hrp_lv2_test
 * (also driven from `pnpm test` via the test:dsp script).
 *
 * There is no LV2 in this file and none in hrp_lv2.h — the shell's core is plain C over the
 * shared engine, so it can be driven here at a plain `cc` against closed-form oracles. What
 * hrp_lv2.c adds on top is a descriptor, six ports and two worker callbacks.
 *
 * The claims, in the order main() makes them:
 *
 *   1. §17: PER-INSTRUMENT. One channel in, one out, and the number is declared. A sum has no
 *      mono insert to rack this at, so the shape refuses the use.
 *
 *   2. §2: THE AUDIO CALLBACK DECIDES NOTHING. Running a whole second of audio through run()
 *      and never calling the worker leaves the filter exactly as it was — no band, no change,
 *      byte for byte. The analysis is not merely "off the RT thread": it is not reachable from
 *      there at all, and that is what makes the claim testable rather than aspirational.
 *
 *   3. THE DEFAULTS ARE INERT. A freshly instantiated instance is a wire. This is the property
 *      that makes the plugin safe to rack on a live channel before anybody asks it for anything.
 *
 *   4. ONE APPLIER, NOT TWO. The audio the shell produces is BIT-IDENTICAL to
 *      omx_hrp_cascade_apply's over the same coefficients — the arm that goes red if the shell
 *      ever grows its own copy of the cascade instead of sharing hrp_correct.h's.
 *
 *   5. A PLANTED RESONANCE IS FOUND AND CUT, LIVE, ONE PASS. A ladder whose H4 rings +10 dB
 *      after the model has heard it steady is corrected near 4·f0 — measured as an attenuation
 *      against the same audio through a bypassed instance, and against the closed-form
 *      transfer function of the coefficients the shell actually published.
 *
 *   6. THE RIPENING DELAY IS NOT LIFTED (§15/§22). The same resonance, with no steady stretch
 *      before it, is NOT cut: live has one read of the audio and cannot correct a note it has
 *      only just met. This is the offline privilege §0.5 grants the render and this shell
 *      refuses, and it is a number, not a claim.
 *
 *   7. §48: THE GAIN MOVES IN BOUNDED STEPS, entering and leaving. No pass moves a band by more
 *      than HRP_GAIN_STEP_DB, a correction leaves by walking to EXACTLY 0 dB, and from there
 *      the output is bit-identical to the input again (§49's "removed/faded", and the reason
 *      bypass does not click).
 *
 *   8. §29: THE BUDGET IS THE CHANNEL'S. Never more live sections than max_bands, whatever the
 *      material offers.
 *
 *   9. §16: THE FAMILY IS A BROAD PRIOR that reaches the search. Setting drums narrows the
 *      detector's range to the family's, and it never moves the confidence floor.
 *
 *  10. ZERO LATENCY, DECLARED. The number a host would compensate is 0, and the plugin says so
 *      rather than staying silent — a host cannot tell "zero" from "never said".
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_lv2.h>

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

static void close_to(double got, double want, double tol, const char *what) {
  g_checks++;
  double d = fabs(got - want);
  if (!(d <= tol)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s (got %.9g, want %.9g, |d| %.3g > %.3g)\n", what, got, want, d, tol);
  }
}

/* ---- the signal ------------------------------------------------------------------------
 *
 * The render oracle's ladder, at the same note and the same integer-cycles-per-window
 * frequency, so a steady partial's deviation is the kernel's answer and not the window's phase.
 * What differs is the SHAPE OF TIME: live reads the audio once, so the model must be taught by
 * a steady stretch BEFORE the resonance rings, exactly as it would be at a show. */
#define RATE 48000u
#define FFT 8192u
#define NBINS (FFT / 2u)
#define SECONDS 18u
#define FRAMES (RATE * SECONDS)
/* 37 × (48000 / 8192): an integer number of cycles in one analysis window. */
static const float F0_HZ = 216.796875f;
#define HARMONICS 5u
static const float LADDER[HARMONICS] = {0.315f, 0.14f, 0.105f, 0.07f, 0.056f};
#define HOT_HARMONIC 4u
#define HOT_EXCESS_DB 10.0f
/* The model is taught by this much steady material before the resonance first rings. */
#define TEACH_SECONDS 6.0
/* Then it rings for a second in every three. EPISODIC, because that is what a resonance IS: a
 * partial that rings when the note is struck a certain way. A partial that is hot in EVERY
 * window is not a resonance, it is the instrument's timbre, and the model correctly learns it
 * and stops reporting an excess — measured, and asserted below in its own arm. */
#define RING_ON 1.0
#define RING_PERIOD 3.0
#define RAMP_SECONDS 0.05

static float g_sig[FRAMES];      /* taught, then ringing */
static float g_cold[FRAMES];     /* the same ring with no teaching stretch before it */
static float g_always[FRAMES];   /* taught, then hot in EVERY window: timbre, not resonance */
static float g_out[FRAMES];
static float g_ref[FRAMES];

static float carry[FFT], wre[FFT], wim[FFT], mags[NBINS], resid[NBINS], hopbuf[FFT];
static float carry2[FFT], wre2[FFT], wim2[FFT], mags2[NBINS], resid2[NBINS], hopbuf2[FFT];

/* Episodic from `from`: on for RING_ON of every RING_PERIOD, ramped across both edges so an
 * abrupt gain step does not put broadband click energy into the detector for no reason. */
static double ring_at(double t, double from) {
  if (t < from) return 0.0;
  const double phase = fmod(t - from, RING_PERIOD);
  if (phase >= RING_ON) return 0.0;
  double v = 1.0;
  if (phase < RAMP_SECONDS) v = phase / RAMP_SECONDS;
  if (phase > RING_ON - RAMP_SECONDS) v = (RING_ON - phase) / RAMP_SECONDS;
  return v < 0.0 ? 0.0 : v;
}

/* Is the resonance ringing at this frame? Used to pick the measurement window. */
static int ringing(uint32_t frame, double from) {
  return ring_at((double)frame / (double)RATE, from) > 0.99;
}

/* Hot from `from` onward with no gaps — the control for "a partial that never stops ringing". */
static void synth_continuous(float *dst, double from) {
  const double hot_lin = pow(10.0, HOT_EXCESS_DB / 20.0);
  for (uint32_t i = 0; i < FRAMES; i++) {
    const double t = (double)i / (double)RATE;
    double ring = 0.0;
    if (t >= from) ring = (t < from + RAMP_SECONDS) ? (t - from) / RAMP_SECONDS : 1.0;
    double x = 0.0;
    for (uint32_t k = 1; k <= HARMONICS; k++) {
      double amp = LADDER[k - 1];
      if (k == HOT_HARMONIC) amp *= 1.0 + ring * (hot_lin - 1.0);
      x += amp * sin(2.0 * M_PI * (double)k * (double)F0_HZ * (double)i / (double)RATE);
    }
    dst[i] = (float)x;
  }
}

static void synth(float *dst, double ring_from) {
  const double hot_lin = pow(10.0, HOT_EXCESS_DB / 20.0);
  for (uint32_t i = 0; i < FRAMES; i++) {
    const double ring = ring_at((double)i / (double)RATE, ring_from);
    double x = 0.0;
    for (uint32_t k = 1; k <= HARMONICS; k++) {
      double amp = LADDER[k - 1];
      if (k == HOT_HARMONIC) amp *= 1.0 + ring * (hot_lin - 1.0);
      x += amp * sin(2.0 * M_PI * (double)k * (double)F0_HZ * (double)i / (double)RATE);
    }
    dst[i] = (float)x;
  }
}

static int init_instance(OmxHrpLv2 *s, float *c, float *re, float *im, float *m, float *r,
                         float *h) {
  if (!omx_hrp_lv2_init(s, RATE, c, re, im, m, r, h)) return 0;
  /* The search a `strings` channel would send — the production path, not a wider one. */
  omx_hrp_lv2_set_family(s, OMX_HRP_FAMILY_STRINGS);
  s->render.cfg.pitch.f0_min_hz = 150.0f;
  s->render.cfg.pitch.f0_max_hz = 400.0f;
  omx_hrp_lv2_set_amount(s, 1.0f, OMX_HRP_LV2_MAX_BANDS);
  return 1;
}

/* Drive the shell exactly as a host would: quanta through run(), a worker pass whenever one is
 * due. Records the deepest cut and the frequency it was placed at, plus the largest single
 * gain move any one pass made. */
typedef struct {
  uint32_t passes;
  uint32_t max_live;
  float deepest_cut_db;
  float deepest_freq_hz;
  float biggest_step_db;
} Drive;

#define QUANTUM 256u

static void drive(OmxHrpLv2 *s, const float *in, float *out, uint32_t frames, int bypass,
                  Drive *d) {
  float prev[OMX_HRP_LV2_MAX_BANDS];
  for (uint32_t i = 0; i < OMX_HRP_LV2_MAX_BANDS; i++) prev[i] = 0.0f;
  memset(d, 0, sizeof(*d));
  for (uint32_t off = 0; off < frames; off += QUANTUM) {
    uint32_t n = frames - off < QUANTUM ? frames - off : QUANTUM;
    omx_hrp_lv2_run(s, in + off, out + off, n);
    if (omx_hrp_lv2_due(s, bypass)) {
      OmxHrpLv2Set set;
      omx_hrp_lv2_analyse(s, bypass, &set);
      omx_hrp_lv2_adopt(s, &set);
      d->passes++;
      for (uint32_t b = 0; b < set.count; b++) {
        if (set.cut_db[b] > d->deepest_cut_db) {
          d->deepest_cut_db = set.cut_db[b];
          d->deepest_freq_hz = set.freq_hz[b];
        }
      }
      for (uint32_t i = 0; i < OMX_HRP_LV2_MAX_BANDS; i++) {
        float now = i < set.count ? set.cut_db[i] : 0.0f;
        float step = fabsf(now - prev[i]);
        if (step > d->biggest_step_db) d->biggest_step_db = step;
        prev[i] = now;
      }
      uint32_t live = omx_hrp_cascade_live(&s->live);
      if (live > d->max_live) d->max_live = live;
    }
  }
}

/* Narrowband level at `hz`, in dB, over the last `n` frames — a Goertzel-style projection, an
 * independent measurement of what came out rather than a re-run of what went in. */
static double level_db_at(const float *x, uint32_t from, uint32_t n, double hz) {
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    const double w = 2.0 * M_PI * hz * (double)(from + i) / (double)RATE;
    re += (double)x[from + i] * cos(w);
    im += (double)x[from + i] * sin(w);
  }
  const double mag = 2.0 * sqrt(re * re + im * im) / (double)n;
  return 20.0 * log10(mag < 1e-12 ? 1e-12 : mag);
}

static double biquad_mag_db(const float c[5], double f_hz) {
  const double w = 2.0 * M_PI * f_hz / (double)RATE;
  const double cw = cos(w), sw = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  const double nr = c[0] + c[1] * cw + c[2] * c2, ni = -(c[1] * sw + c[2] * s2);
  const double dr = 1.0 + c[3] * cw + c[4] * c2, di = -(c[3] * sw + c[4] * s2);
  return 20.0 * log10(sqrt((nr * nr + ni * ni) / (dr * dr + di * di)));
}

/* ---- 1 ---------------------------------------------------------------------------------- */

static void test_per_instrument(void) {
  check(OMX_HRP_LV2_CHANNELS == 1, "§17: one channel — HRP is per-instrument, never on a sum");
}

/* ---- 2, 3, 4 ---------------------------------------------------------------------------- */

static void test_the_audio_callback_decides_nothing(void) {
  static OmxHrpLv2 s;
  check(init_instance(&s, carry, wre, wim, mags, resid, hopbuf) == 1, "the instance initialises");

  /* A whole second of a screaming resonance through run(), and never a worker pass. */
  for (uint32_t off = 0; off < RATE; off += QUANTUM)
    omx_hrp_lv2_run(&s, g_sig + off, g_out + off, QUANTUM);

  check(omx_hrp_cascade_live(&s.live) == 0u,
        "§2: a second of audio through run() plants no band, because run() cannot analyse");
  check(memcmp(g_sig, g_out, RATE * sizeof(float)) == 0,
        "and the samples are the input, byte for byte");
}

static void test_the_defaults_are_inert(void) {
  static OmxHrpLv2 s;
  init_instance(&s, carry, wre, wim, mags, resid, hopbuf);
  check(omx_hrp_cascade_live(&s.live) == 0u, "a fresh instance has no live section");
  omx_hrp_lv2_run(&s, g_sig, g_out, 4096u);
  check(memcmp(g_sig, g_out, 4096u * sizeof(float)) == 0, "so it is a wire");

  /* Bypassed, over the whole signal, with the worker running: still a wire. */
  static OmxHrpLv2 b;
  init_instance(&b, carry2, wre2, wim2, mags2, resid2, hopbuf2);
  Drive d;
  drive(&b, g_sig, g_out, FRAMES, 1 /* bypass */, &d);
  check(omx_hrp_cascade_live(&b.live) == 0u, "a bypassed instance never plants anything");
  check(memcmp(g_sig, g_out, FRAMES * sizeof(float)) == 0,
        "and passes a whole signal through byte for byte");
}

static void test_one_applier_not_two(void) {
  /* If the shell ever grows its own copy of the cascade instead of sharing hrp_correct.h's,
   * these two will stop agreeing to the last bit. */
  static OmxHrpLv2 s;
  init_instance(&s, carry, wre, wim, mags, resid, hopbuf);

  OmxHrpLv2Set set;
  memset(&set, 0, sizeof(set));
  set.count = 2u;
  omx_hrp_peaking_coeffs(867.2f, OMX_HRP_BAND_Q, -3.0f, RATE, set.coeffs[0]);
  omx_hrp_peaking_coeffs(1300.0f, OMX_HRP_BAND_Q, -5.0f, RATE, set.coeffs[1]);
  omx_hrp_lv2_adopt(&s, &set);
  check(omx_hrp_cascade_live(&s.live) == 2u, "both adopted sections are live");

  OmxHrpCascade ref;
  omx_hrp_cascade_init(&ref);
  omx_hrp_cascade_add(&ref, set.coeffs[0], 1);
  omx_hrp_cascade_add(&ref, set.coeffs[1], 1);

  const uint32_t n = 32768u;
  for (uint32_t off = 0; off < n; off += QUANTUM) omx_hrp_lv2_run(&s, g_sig + off, g_out + off, QUANTUM);
  omx_hrp_cascade_apply(&ref, g_sig, g_ref, n);
  check(memcmp(g_out, g_ref, n * sizeof(float)) == 0,
        "the shell's audio is omx_hrp_cascade_apply's, bit for bit — one applier, not two");
}

/* ---- 5, 6 ------------------------------------------------------------------------------- */

static void test_a_planted_resonance_is_cut_live(void) {
  static OmxHrpLv2 s;
  init_instance(&s, carry, wre, wim, mags, resid, hopbuf);
  Drive d;
  drive(&s, g_sig, g_out, FRAMES, 0, &d);

  check(d.passes > 100u, "the worker ran a pass per hop over the signal");
  check(d.max_live >= 1u, "a correction was planted while the resonance rang");
  check(d.deepest_cut_db > 1.0f, "and it is a real cut, not a token one");

  const double want_hz = (double)F0_HZ * (double)HOT_HARMONIC;
  const double placed = (double)d.deepest_freq_hz;
  check(fabs(1200.0 * log2(placed / want_hz)) < 60.0,
        "placed within a bin's musical width of the partial that actually rang");

  /* MEASURED, not asserted: the same audio through a bypassed instance, and the difference at
   * 4·f0 read off the output with an independent narrowband projection. */
  static OmxHrpLv2 b;
  init_instance(&b, carry2, wre2, wim2, mags2, resid2, hopbuf2);
  Drive db;
  drive(&b, g_sig, g_ref, FRAMES, 1, &db);

  /* Measure INSIDE the last ring episode — the window where a correction is supposed to be in
   * force. Found by walking back from the end for a stretch that is actually ringing, so the
   * window follows the signal rather than a hand-typed offset that a retimed signal would
   * silently invalidate. */
  uint32_t tail_end = FRAMES;
  while (tail_end > 0u && !ringing(tail_end - 1u, TEACH_SECONDS)) tail_end--;
  check(tail_end > RATE, "found a ring episode to measure in (positive control)");
  const uint32_t tail_n = RATE / 2u;
  const uint32_t tail_from = tail_end - tail_n;
  const double engaged = level_db_at(g_out, tail_from, tail_n, want_hz);
  const double bypassed = level_db_at(g_ref, tail_from, tail_n, want_hz);
  const double delta = engaged - bypassed;
  printf("  hrp_lv2: H%u over %.2f-%.2f s engaged %.2f dBFS vs bypassed %.2f dBFS -> %.2f dB, "
         "band at %.1f Hz (want %.1f), deepest cut %.2f dB\n",
         HOT_HARMONIC, (double)tail_from / RATE, (double)tail_end / RATE, engaged, bypassed,
         delta, placed, want_hz, (double)d.deepest_cut_db);
  check(delta < -1.0, "the engaged path is quieter at the resonance than the bypassed one");

  /* The attenuation is the transfer function of the coefficients the shell published — an
   * independent closed form, not a second run of the same cascade. */
  /* The attenuation is bounded by the deepest cut the shell ever published at that frequency:
   * a Q-5 bell of that depth cannot take off more than its own |H|, and the measured window
   * spans the ramp so it takes off somewhat less. Both ends are asserted, because "quieter" on
   * its own would also be satisfied by a bug that attenuated everything. */
  float deepest[5];
  omx_hrp_peaking_coeffs(d.deepest_freq_hz, OMX_HRP_BAND_Q, -d.deepest_cut_db, RATE, deepest);
  const double bound = biquad_mag_db(deepest, want_hz);
  printf("  hrp_lv2: the deepest published bell answers %.2f dB at that frequency\n", bound);
  check(delta >= bound - 0.35, "no more attenuation than the deepest bell it published can give");

  /* A partial nobody rang is left alone. */
  const double h3 = (double)F0_HZ * 3.0;
  const double d3 = level_db_at(g_out, tail_from, tail_n, h3) -
                    level_db_at(g_ref, tail_from, tail_n, h3);
  printf("  hrp_lv2: steady neighbour H3 moved %.2f dB\n", d3);
  check(fabs(d3) < 1.5, "and its steady neighbour H3 is not touched");
}

static void test_a_partial_that_never_stops_is_timbre(void) {
  /* §22/§23: the baseline is what this instrument NORMALLY sounds like. A partial hot in every
   * window is not a resonance ringing against the norm — after enough windows it IS the norm,
   * and HRP must stop cutting it. Otherwise the corrector would walk an instrument's own
   * voicing down 6 dB for as long as it kept playing.
   *
   * Measured: the cut appears while the model still disagrees, then RETIRES as the model
   * converges, and the audio at that partial comes back to the bypassed level. */
  static OmxHrpLv2 s;
  init_instance(&s, carry, wre, wim, mags, resid, hopbuf);
  Drive d;
  drive(&s, g_always, g_out, FRAMES, 0, &d);

  static OmxHrpLv2 b;
  init_instance(&b, carry2, wre2, wim2, mags2, resid2, hopbuf2);
  Drive db;
  drive(&b, g_always, g_ref, FRAMES, 1, &db);

  const double want_hz = (double)F0_HZ * (double)HOT_HARMONIC;
  const uint32_t n = RATE / 2u, from = FRAMES - n;
  const double settled = level_db_at(g_out, from, n, want_hz) -
                         level_db_at(g_ref, from, n, want_hz);
  printf("  hrp_lv2: an ALWAYS-hot partial ends %.2f dB from bypassed (deepest cut on the way "
         "in was %.2f dB)\n", settled, (double)d.deepest_cut_db);
  check(d.max_live >= 1u, "the excess was found while the model still disagreed");
  check(fabs(settled) < 0.5,
        "and retired once the model learned it: a permanent partial is timbre, not resonance");
}

static void test_the_ripening_delay_is_not_lifted(void) {
  /* The same resonance, ringing from the first sample: live has ONE read and no model, so
   * §15/§22's ripening is not lifted and there is nothing to correct against. */
  static OmxHrpLv2 s;
  init_instance(&s, carry, wre, wim, mags, resid, hopbuf);
  Drive d;
  drive(&s, g_cold, g_out, RATE * 3u, 0, &d);
  check(d.max_live == 0u,
        "§15/§22: a note met for the first time is not cut — the offline two-pass privilege "
        "is NOT lifted here");
}

/* ---- 7, 8 ------------------------------------------------------------------------------- */

static void test_the_gain_moves_in_bounded_steps(void) {
  static OmxHrpLv2 s;
  init_instance(&s, carry, wre, wim, mags, resid, hopbuf);
  Drive d;
  drive(&s, g_sig, g_out, FRAMES, 0, &d);
  check(d.max_live >= 1u, "a correction was in force (positive control for the walk-out below)");
  check(d.biggest_step_db <= OMX_HRP_GAIN_STEP_DB + 1e-4f,
        "§48: no pass moves a band by more than the declared step");

  /* Now bypass, and keep driving: every band must walk to EXACTLY zero and the path must come
   * back to bit-identical. §49's "removed/faded", and the reason a bypass does not click. */
  uint32_t guard = 0u;
  while (omx_hrp_cascade_live(&s.live) > 0u && guard < 400u) {
    omx_hrp_lv2_run(&s, g_sig, g_out, QUANTUM);
    if (omx_hrp_lv2_due(&s, 1)) {
      OmxHrpLv2Set set;
      omx_hrp_lv2_analyse(&s, 1, &set);
      omx_hrp_lv2_adopt(&s, &set);
      guard++;
    }
  }
  check(guard < 400u, "the walk-out terminates");
  check(omx_hrp_cascade_live(&s.live) == 0u, "§49: bypass walks every correction out to nothing");
  for (uint32_t i = 0; i < OMX_HRP_LV2_MAX_BANDS; i++)
    close_to(s.slot[i].applied_db, 0.0, 0.0, "and lands on EXACTLY 0 dB, not near it");

  omx_hrp_lv2_run(&s, g_sig, g_out, 4096u);
  check(memcmp(g_sig, g_out, 4096u * sizeof(float)) == 0,
        "from which removal is bit-identical — the provable parked identity");
}

/* Drive with an IRREGULAR worker: the pass happens some number of quanta after it became due,
 * and that number keeps changing — which is what a host thread actually does. Records the
 * published cut at every pass so two schedules can be compared exactly. */
static uint32_t drive_irregular(OmxHrpLv2 *s, const float *in, float *out, uint32_t frames,
                                uint32_t seed, float *cuts, uint32_t cuts_max) {
  uint32_t n_cuts = 0u;
  uint32_t pending = 0u;
  uint32_t rng = seed;
  for (uint32_t off = 0; off + QUANTUM <= frames; off += QUANTUM) {
    omx_hrp_lv2_run(s, in + off, out + off, QUANTUM);
    if (pending > 0u && --pending == 0u) {
      OmxHrpLv2Set set;
      omx_hrp_lv2_analyse(s, 0, &set);
      omx_hrp_lv2_adopt(s, &set);
      if (n_cuts < cuts_max) {
        float deepest = 0.0f;
        for (uint32_t b = 0; b < set.count; b++)
          if (set.cut_db[b] > deepest) deepest = set.cut_db[b];
        cuts[n_cuts++] = deepest;
      }
    }
    if (pending == 0u && omx_hrp_lv2_due(s, 0)) {
      rng = rng * 1103515245u + 12345u;
      /* 1..8 quanta of lateness — up to a whole hop behind, which is as late as a worker can
       * be while still keeping up at all. Seed 0 means "run it immediately", the regular case. */
      pending = seed == 0u ? 1u : 1u + ((rng >> 16) & 7u);
    }
  }
  return n_cuts;
}

static void test_when_the_worker_runs_does_not_change_what_it_analyses(void) {
  /* THE REGRESSION THIS FILE EXISTS FOR, and it was found by a real host and not by this file.
   *
   * The first shape of the RT->worker handoff had run() reset a "frames pending" counter when
   * it SCHEDULED a job, and the worker then read the ring's NEWEST hop whenever the host got
   * round to running it. Both halves made the engine's diet depend on thread scheduling: hops
   * were double-counted or skipped at random, and the model therefore ripened at a different
   * point in the signal every run. Through lv2-host-probe that measured -0.68, -4.20, 0.00,
   * -0.70 and -2.42 dB on the same partial over the same half-second of identical audio.
   *
   * A cursor pair fixed it — written by the audio thread, consumed by the worker, and the
   * worker takes exactly the frames nobody has taken. This arm is the mechanical form of that:
   * the SEQUENCE of published cuts must be identical under three different worker schedules. */
  enum { CUTS = 512 };
  static float cuts_a[CUTS], cuts_b[CUTS], cuts_c[CUTS];
  static OmxHrpLv2 a, b, c;

  init_instance(&a, carry, wre, wim, mags, resid, hopbuf);
  const uint32_t na = drive_irregular(&a, g_sig, g_out, FRAMES, 0u, cuts_a, CUTS);
  init_instance(&b, carry2, wre2, wim2, mags2, resid2, hopbuf2);
  const uint32_t nb = drive_irregular(&b, g_sig, g_ref, FRAMES, 12345u, cuts_b, CUTS);
  init_instance(&c, carry, wre, wim, mags, resid, hopbuf);
  const uint32_t nc = drive_irregular(&c, g_sig, g_out, FRAMES, 99991u, cuts_c, CUTS);

  check(na > 100u, "the regular schedule ran a useful number of passes (positive control)");
  check(na == nb && nb == nc, "every schedule takes the SAME number of passes over the audio");

  uint32_t differ = 0u;
  for (uint32_t i = 0; i < na && i < CUTS; i++) {
    if (cuts_a[i] != cuts_b[i]) differ++;
    if (cuts_a[i] != cuts_c[i]) differ++;
  }
  printf("  hrp_lv2: %u passes under three worker schedules, %u differing cuts\n", na, differ);
  check(differ == 0u,
        "and publishes the identical cut at every one — when the worker runs cannot change "
        "what it analyses");

  float deepest_a = 0.0f;
  for (uint32_t i = 0; i < na && i < CUTS; i++)
    if (cuts_a[i] > deepest_a) deepest_a = cuts_a[i];
  check(deepest_a > 1.0f, "and a real cut was published (else the arm above compares nothing)");
}

static void test_the_budget_is_the_channels(void) {
  static OmxHrpLv2 s;
  init_instance(&s, carry, wre, wim, mags, resid, hopbuf);
  omx_hrp_lv2_set_amount(&s, 1.0f, 2u);
  Drive d;
  drive(&s, g_sig, g_out, FRAMES, 0, &d);
  check(d.max_live <= 2u, "§29: never more live sections than the channel's budget allows");

  omx_hrp_lv2_set_amount(&s, 1.0f, 999u);
  check(s.max_bands == OMX_HRP_LV2_MAX_BANDS, "a budget past the ceiling is clamped, not taken");
  omx_hrp_lv2_set_amount(&s, 5.0f, 0u);
  check(s.max_bands == 1u, "and a zero budget is one band, never zero-and-silently-dead");
  close_to(s.amount, 1.0, 1e-6, "the amount is clamped into its declared travel");
}

/* ---- 9, 10 ------------------------------------------------------------------------------ */

static void test_the_family_reaches_the_search(void) {
  static OmxHrpLv2 s;
  omx_hrp_lv2_init(&s, RATE, carry, wre, wim, mags, resid, hopbuf);
  const float wide_max = s.render.cfg.pitch.f0_max_hz;

  omx_hrp_lv2_set_family(&s, OMX_HRP_FAMILY_DRUMS);
  close_to(s.render.cfg.pitch.f0_max_hz, 400.0, 1e-6, "§16: drums narrows the search to 400 Hz");
  check(s.render.cfg.pitch.max_voices == 1u, "and to one voice");
  close_to(s.render.cfg.pitch.min_confidence, omx_hrp_min_confidence_for(OMX_HRP_MAX_VOICES),
           1e-9, "and never moves the confidence floor");

  omx_hrp_lv2_set_family(&s, OMX_HRP_FAMILY_KEYS);
  close_to(s.render.cfg.pitch.f0_max_hz, 4200.0, 1e-6, "keys reaches C8");
  check(s.render.cfg.pitch.max_voices == 8u, "with the full voice budget");

  omx_hrp_lv2_set_family(&s, OMX_HRP_FAMILY_NONE);
  close_to(s.render.cfg.pitch.f0_max_hz, wide_max, 1e-6, "and no family is the console's own");
}

static void test_zero_latency_declared(void) {
  close_to(OMX_HRP_LV2_LATENCY_FRAMES, 0.0, 0.0,
           "§2: HRP adds zero intentional audio latency, and the plugin publishes the zero");
}

static void test_absence_is_a_fact(void) {
  OmxHrpLv2 s;
  check(omx_hrp_lv2_init(&s, 0u, carry, wre, wim, mags, resid, hopbuf) == 0,
        "a zero rate is refused");
  check(s.ready == 0, "and leaves the instance not ready");
  /* A not-ready instance still passes audio: a plugin that could not analyse must not also
   * stop the show. */
  omx_hrp_lv2_run(&s, g_sig, g_out, 1024u);
  check(memcmp(g_sig, g_out, 1024u * sizeof(float)) == 0, "and still passes audio through");
  check(omx_hrp_lv2_due(&s, 0) == 0, "and never asks for a worker pass");
  OmxHrpLv2Set set;
  omx_hrp_lv2_analyse(&s, 0, &set);
  check(set.count == 0u, "and analyses nothing if one is forced on it");
}

int main(void) {
  synth(g_sig, TEACH_SECONDS);
  synth(g_cold, 0.0);
  synth_continuous(g_always, TEACH_SECONDS);

  test_per_instrument();
  test_the_audio_callback_decides_nothing();
  test_the_defaults_are_inert();
  test_one_applier_not_two();
  test_a_planted_resonance_is_cut_live();
  test_a_partial_that_never_stops_is_timbre();
  test_the_ripening_delay_is_not_lifted();
  test_the_gain_moves_in_bounded_steps();
  test_when_the_worker_runs_does_not_change_what_it_analyses();
  test_the_budget_is_the_channels();
  test_the_family_reaches_the_search();
  test_zero_latency_declared();
  test_absence_is_a_fact();

  printf("hrp_lv2: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
