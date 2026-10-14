// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Standalone unit test for the OFFLINE HRP render kernel (hrp_render.h) —
 * the recording vsc spec §0.5 over
 * the hrp polyphonic architecture spec:
 *   cc -Wall -Wextra -O2 -o build/hrp_render_test src/hrp_render.test.c -lm && ./build/hrp_render_test
 * (also driven from `pnpm test` via the test:dsp script).
 *
 * The claims, in the order main() makes them:
 *
 *   1. A PLANTED RESONANCE IS MEASURED. A harmonic ladder whose H4 rings +10 dB for a quarter
 *      of the take reports an excess at (note, H4) near ten decibels and a duty near a quarter,
 *      while its steady neighbours H3 and H5 report an excess far below the correction
 *      threshold. Known resonance in, measured excess out.
 *
 *   2. THE RIPENING DELAY IS LIFTED, AND IT IS THE TWO PASSES THAT LIFT IT. The take's opening
 *      SECOND is measured against a model the whole file taught; the same second fed to a render
 *      that has not taught yet measures nothing at all, because a live model cannot ripen in
 *      twenty windows. That difference is the offline privilege (§15 / §22 live, §0.5 offline),
 *      and it is a number, not a claim. The tracker's own stability gate (§13) is NOT lifted and
 *      the test says so: the first three windows of a note are unmeasured either way.
 *
 *   3. THE RENDER APPLIES EXACTLY THE FILTER IT WAS GIVEN. Every harmonic's before/after ratio
 *      equals the transfer function of the supplied coefficients evaluated at that frequency —
 *      an independent closed form, not a re-run of the same code.
 *
 *   4. THE CORRECTION IS SELECTIVE. A −6 dB bell at H4 takes 6 dB off H4 and barely more than a
 *      decibel off its neighbours.
 *
 *   5. BYPASS IS BIT-PRESERVING. No band, and a parked (identity) band, both render the input
 *      byte for byte.
 *
 *   6. THE RENDER IS DETERMINISTIC. The same take analysed twice reports the same numbers, and
 *      rendered twice produces the same samples — the property that makes a rendered file a
 *      thing you can hand somebody.
 *
 *   7. BLOCK SIZE IS NOT AUDIBLE. Streaming a file in odd-sized blocks gives the same samples a
 *      single whole-file call gives, so a shell may chunk for progress without changing the
 *      audio.
 *
 *   8. OFFLINE AND LIVE PROJECT THE SAME VOICE SET. The render's measuring pass and the live
 *      chain in `hrp_analyze.c`'s order, stepped window by window over the same take, project
 *      byte-identical voice sets with identical voice identities. One projection
 *      (`omx_hrp_project_voices`) is what makes that true rather than a coincidence two copies
 *      would have to keep up; before audit D4's fold the live copy was the one no test ran.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_render.h>

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

static void in_range(double got, double lo, double hi, const char *what) {
  g_checks++;
  if (!(got >= lo && got <= hi)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s (got %.9g, want %.9g..%.9g)\n", what, got, lo, hi);
  }
}

/* ---- the synthetic take ------------------------------------------------------------------
 *
 * A sustained harmonic ladder at a fundamental that sits EXACTLY on a bin — 37 whole cycles per
 * 8192-sample window — so every window's magnitude spectrum is the same regardless of where the
 * hop started it. That is what lets a steady partial's deviation be asserted at zero: any excess
 * the test measures on H3 or H5 is the kernel's, not the analysis window's phase.
 *
 * H4 rings +10 dB for one second in four. The ring is RAMPED over 50 ms at each edge: an abrupt
 * gain step is a click, broadband energy the detector would have to survive for no reason the
 * test is about.
 */
#define TEST_RATE 48000u
#define TEST_FFT 8192u
#define TEST_HOP 2048u
#define TEST_SECONDS 8u
#define TEST_FRAMES (TEST_RATE * TEST_SECONDS)
#define TEST_NBINS (TEST_FFT / 2u)

/* 37 × (48000 / 8192): an integer number of cycles in one analysis window. */
static const float F0_HZ = 216.796875f;
/* Nearest equal-tempered note to 216.8 Hz — A3, and where every measured cell is indexed. */
#define TEST_NOTE 57
#define TEST_HARMONICS 5u
static const float LADDER[TEST_HARMONICS] = {0.315f, 0.14f, 0.105f, 0.07f, 0.056f};
#define HOT_HARMONIC 4u
#define HOT_EXCESS_DB 10.0f
#define RAMP_SECONDS 0.05

static float g_take[TEST_FRAMES];
static float g_rendered[TEST_FRAMES];
static float g_rendered_b[TEST_FRAMES];

static float carry_a[TEST_FFT], work_re_a[TEST_FFT], work_im_a[TEST_FFT];
static float mags_a[TEST_NBINS], residual_a[TEST_NBINS];
static float carry_b[TEST_FFT], work_re_b[TEST_FFT], work_im_b[TEST_FFT];
static float mags_b[TEST_NBINS], residual_b[TEST_NBINS];
static float probe_re[TEST_FFT], probe_im[TEST_FFT], probe_mag[TEST_NBINS];

static OmxHrpRender g_render_a;
static OmxHrpRender g_render_b;

/* 0 when the resonance is at rest, 1 while it rings, ramped across the edges. */
static double ring_amount(uint32_t frame) {
  const double t = (double)frame / (double)TEST_RATE;
  static const double on[2] = {2.0, 5.0};
  double best = 0.0;
  for (int s = 0; s < 2; s++) {
    const double a = on[s], b = on[s] + 1.0;
    double v = 0.0;
    if (t >= a && t < b) {
      v = 1.0;
      if (t < a + RAMP_SECONDS) v = (t - a) / RAMP_SECONDS;
      if (t > b - RAMP_SECONDS) v = (b - t) / RAMP_SECONDS;
    }
    if (v > best) best = v;
  }
  return best;
}

static void synth_take(void) {
  const double hot_lin = pow(10.0, HOT_EXCESS_DB / 20.0);
  for (uint32_t i = 0; i < TEST_FRAMES; i++) {
    const double ring = ring_amount(i);
    double x = 0.0;
    for (uint32_t k = 1; k <= TEST_HARMONICS; k++) {
      double amp = LADDER[k - 1];
      if (k == HOT_HARMONIC) amp *= 1.0 + ring * (hot_lin - 1.0);
      x += amp * sin(2.0 * M_PI * (double)k * (double)F0_HZ * (double)i / (double)TEST_RATE);
    }
    g_take[i] = (float)x;
  }
}

/* The search this test runs: the console's own floor and harmonic count, narrowed to the range
 * the take actually lives in — exactly what `hrpSearchFor` sends for an assigned family, so the
 * test exercises the production path rather than a wider one nothing uses. */
static OmxHrpRenderConfig test_config(void) {
  OmxHrpRenderConfig cfg = omx_hrp_render_config_default(TEST_RATE);
  cfg.pitch.f0_min_hz = 150.0f;
  cfg.pitch.f0_max_hz = 400.0f;
  return cfg;
}

/* Teach over the whole take, rewind, measure over the whole take. The two-pass arrangement IS
 * the offline core; every claim below about a measurement rests on it. */
static void analyse_whole_take(OmxHrpRender *r, float *carry, float *re, float *im, float *mags,
                               float *residual) {
  OmxHrpRenderConfig cfg = test_config();
  omx_hrp_render_init(r, &cfg, carry, re, im, mags, residual);
  omx_hrp_render_feed(r, g_take, TEST_FRAMES, 1);
  omx_hrp_render_rewind(r);
  omx_hrp_render_feed(r, g_take, TEST_FRAMES, 0);
}

/* ---- 1. a planted resonance is measured -------------------------------------------------- */

static void test_a_planted_resonance_is_measured_and_its_neighbours_are_not(void) {
  analyse_whole_take(&g_render_a, carry_a, work_re_a, work_im_a, mags_a, residual_a);

  check(g_render_a.report.windows > 150u, "the measuring pass analysed the whole take");
  check(g_render_a.report.pitched_windows > g_render_a.report.windows / 2u,
        "a sustained harmonic ladder is heard as pitched material in most windows");

  const OmxHrpRenderCell *h4 = &g_render_a.report.cell[TEST_NOTE][HOT_HARMONIC - 1u];
  const OmxHrpRenderCell *h3 = &g_render_a.report.cell[TEST_NOTE][2];
  const OmxHrpRenderCell *h5 = &g_render_a.report.cell[TEST_NOTE][4];

  check(h4->frames >= OMX_HRP_RENDER_MIN_FRAMES, "H4 was measured over enough windows to speak");
  check(h3->frames >= OMX_HRP_RENDER_MIN_FRAMES, "H3 was measured over enough windows to speak");
  check(h5->frames >= OMX_HRP_RENDER_MIN_FRAMES, "H5 was measured over enough windows to speak");

  /* The ring is +10 dB; the model drifts up under it by the render's own step, so the measured
     mean excess sits a little below ten and never above it. */
  in_range(omx_hrp_render_excess_db(h4), 6.0, 10.5, "H4's measured excess is the planted ring");
  check(omx_hrp_render_peak_excess_db(h4) >= 8.5f,
        "the first window of a ring sees nearly the whole excess, before the model moves");

  /* One second in four, ramped — the duty is the temporal-stability evidence §24 asks for. */
  in_range(omx_hrp_render_duty(h4), 0.15, 0.45, "H4's duty is the quarter of the take it rang for");

  /* A partial that never moves has nothing above its own baseline. Half a decibel is far below
     HRP_EXCESS_THRESHOLD_DB (3), so neither neighbour could produce a cut. */
  check(omx_hrp_render_excess_db(h3) < 0.5f, "H3 is steady, so it reports no resonance");
  check(omx_hrp_render_excess_db(h5) < 0.5f, "H5 is steady, so it reports no resonance");

  /* The fundamental is measured like any other partial — refusing to CUT it is the policy
     tier's law (harmonic-excess.ts), not a hole in the measurement. */
  check(g_render_a.report.cell[TEST_NOTE][0].frames > 0u, "H1 is measured, and never cut here");
}

/* ---- 2. the ripening delay, lifted --------------------------------------------------------- */

static void test_the_takes_opening_is_measured_only_because_the_file_taught_first(void) {
  OmxHrpRenderConfig cfg = test_config();
  /* One second — twenty windows. Long enough for the tracker's own three-window stability gate
     (§13, which still binds offline: an attack is not a resonance), and far too short for the
     baseline's twenty-four accepted updates. This is exactly where a live corrector stands one
     second into a note: hearing it, and with nothing to compare it to. */
  const uint32_t opening = TEST_RATE;

  omx_hrp_render_init(&g_render_b, &cfg, carry_b, work_re_b, work_im_b, mags_b, residual_b);
  omx_hrp_render_feed(&g_render_b, g_take, TEST_FRAMES, 1);
  omx_hrp_render_rewind(&g_render_b);
  omx_hrp_render_reset_report(&g_render_b);
  const uint32_t windows = omx_hrp_render_feed(&g_render_b, g_take, opening, 0);
  const uint32_t taught_frames = g_render_b.report.cell[TEST_NOTE][2].frames;

  /* The same opening, on a render that has learned nothing (§15, §22 live). */
  static OmxHrpRender fresh;
  omx_hrp_render_init(&fresh, &cfg, carry_b, work_re_b, work_im_b, mags_b, residual_b);
  const uint32_t fresh_windows = omx_hrp_render_feed(&fresh, g_take, opening, 0);
  const uint32_t untaught_frames = fresh.report.cell[TEST_NOTE][2].frames;

  check(windows == fresh_windows, "both renders saw the same windows of the same audio");
  check(windows < cfg.baseline.ripe_updates + cfg.track.promote_frames,
        "one second is too short for a live model to ripen — which is the point");
  check(taught_frames > 0u, "the take's OPENING is measured against a model the whole file taught");
  check(untaught_frames == 0u, "the same opening measures nothing when nothing was taught");
}

/* ---- the transfer function, evaluated independently --------------------------------------- */

/* RBJ peaking-EQ section, normalised the way `@freemixer/core`'s eqCoeffs normalises it:
 * {b0, b1, b2, a1, a2} with a0 folded to one and the a-terms SUBTRACTED by omx_biquad.
 * This lives in the TEST as the oracle's generator — production designs its coefficients in
 * TypeScript, and a second production home for the curve maths is exactly what §1 forbids. */
static void rbj_peaking(double f_hz, double q, double gain_db, double rate, float out[5]) {
  const double a = pow(10.0, gain_db / 40.0);
  const double w0 = 2.0 * M_PI * f_hz / rate;
  const double alpha = sin(w0) / (2.0 * q);
  const double b0 = 1.0 + alpha * a;
  const double b1 = -2.0 * cos(w0);
  const double b2 = 1.0 - alpha * a;
  const double a0 = 1.0 + alpha / a;
  const double a1 = -2.0 * cos(w0);
  const double a2 = 1.0 - alpha / a;
  out[0] = (float)(b0 / a0);
  out[1] = (float)(b1 / a0);
  out[2] = (float)(b2 / a0);
  out[3] = (float)(a1 / a0);
  out[4] = (float)(a2 / a0);
}

/* |H(e^{jw})| in dB for a normalised section — the closed form, from the coefficients alone. */
static double section_response_db(const float c[5], double f_hz, double rate) {
  const double w = 2.0 * M_PI * f_hz / rate;
  const double nr = c[0] + c[1] * cos(w) + c[2] * cos(2.0 * w);
  const double ni = -(c[1] * sin(w) + c[2] * sin(2.0 * w));
  const double dr = 1.0 + c[3] * cos(w) + c[4] * cos(2.0 * w);
  const double di = -(c[3] * sin(w) + c[4] * sin(2.0 * w));
  const double num = sqrt(nr * nr + ni * ni);
  const double den = sqrt(dr * dr + di * di);
  return 20.0 * log10(num / den);
}

/* The magnitude at `hz` in one window of `buf` starting at `from` — the same reading the
 * analyser takes, so a level here and a level in the report mean the same thing. */
static double window_level(const float *buf, uint32_t from, double hz) {
  omx_rta_spectrum(buf + from, TEST_FFT, probe_re, probe_im, probe_mag);
  const float bin_hz = (float)TEST_RATE / (float)TEST_FFT;
  const float peak = omx_hrp_bin_peak(probe_mag, TEST_NBINS, bin_hz, (float)hz);
  return peak > 0.0f ? 20.0 * log10((double)peak) : -999.0;
}

/* ---- 3 + 4. the render applies exactly the filter, and it is selective --------------------- */

#define PROBE_FROM 32768u

static void test_the_render_applies_exactly_the_filter_it_was_given(void) {
  float coeffs[5];
  const double centre = (double)F0_HZ * (double)HOT_HARMONIC;
  rbj_peaking(centre, 5.0, -6.0, (double)TEST_RATE, coeffs);

  OmxHrpCascade cascade;
  omx_hrp_cascade_init(&cascade);
  check(omx_hrp_cascade_add(&cascade, coeffs, 1) == 1, "a designed section is taken");
  check(omx_hrp_cascade_live(&cascade) == 1u, "one section will run");

  omx_hrp_cascade_apply(&cascade, g_take, g_rendered, TEST_FRAMES);

  for (uint32_t k = 1; k <= TEST_HARMONICS; k++) {
    const double hz = (double)k * (double)F0_HZ;
    const double before = window_level(g_take, PROBE_FROM, hz);
    const double after = window_level(g_rendered, PROBE_FROM, hz);
    char what[96];
    snprintf(what, sizeof(what), "H%u moves by exactly the section's response", k);
    close_to(after - before, section_response_db(coeffs, hz, (double)TEST_RATE), 0.05, what);
  }

  const double h3 = window_level(g_rendered, PROBE_FROM, 3.0 * F0_HZ) -
                    window_level(g_take, PROBE_FROM, 3.0 * F0_HZ);
  const double h4 = window_level(g_rendered, PROBE_FROM, 4.0 * F0_HZ) -
                    window_level(g_take, PROBE_FROM, 4.0 * F0_HZ);
  const double h5 = window_level(g_rendered, PROBE_FROM, 5.0 * F0_HZ) -
                    window_level(g_take, PROBE_FROM, 5.0 * F0_HZ);

  close_to(h4, -6.0, 0.15, "a -6 dB correction takes 6 dB off the resonance");
  check(fabs(h3) < 1.2, "the neighbour below loses barely a decibel");
  check(fabs(h5) < 1.2, "the neighbour above loses barely a decibel");
  check(fabs(h4) > 4.0 * fabs(h3), "the cut is far deeper at the resonance than beside it");
  check(fabs(h4) > 4.0 * fabs(h5), "the cut is far deeper at the resonance than above it");
}

/* ---- 5. bypass is bit-preserving ---------------------------------------------------------- */

static void test_a_render_with_nothing_to_correct_is_the_original(void) {
  OmxHrpCascade empty;
  omx_hrp_cascade_init(&empty);
  memset(g_rendered, 0, sizeof(g_rendered));
  omx_hrp_cascade_apply(&empty, g_take, g_rendered, TEST_FRAMES);
  check(memcmp(g_take, g_rendered, sizeof(g_take)) == 0,
        "an empty cascade renders the take byte for byte");

  /* A band at 0 dB is identity and is published PARKED — the kernel must skip it, not run it. */
  float identity[5];
  rbj_peaking((double)F0_HZ * 4.0, 5.0, 0.0, (double)TEST_RATE, identity);
  OmxHrpCascade parked;
  omx_hrp_cascade_init(&parked);
  omx_hrp_cascade_add(&parked, identity, 0);
  check(omx_hrp_cascade_live(&parked) == 0u, "a parked band is not a live section");
  memset(g_rendered, 0, sizeof(g_rendered));
  omx_hrp_cascade_apply(&parked, g_take, g_rendered, TEST_FRAMES);
  check(memcmp(g_take, g_rendered, sizeof(g_take)) == 0,
        "a parked band renders the take byte for byte");
}

/* ---- 6. determinism ----------------------------------------------------------------------- */

static void test_the_same_take_renders_to_the_same_file_twice(void) {
  analyse_whole_take(&g_render_a, carry_a, work_re_a, work_im_a, mags_a, residual_a);
  analyse_whole_take(&g_render_b, carry_b, work_re_b, work_im_b, mags_b, residual_b);
  check(memcmp(&g_render_a.report, &g_render_b.report, sizeof(OmxHrpRenderReport)) == 0,
        "the same take analysed twice reports the same numbers");

  float coeffs[5];
  rbj_peaking((double)F0_HZ * 4.0, 5.0, -4.5, (double)TEST_RATE, coeffs);
  OmxHrpCascade one, two;
  omx_hrp_cascade_init(&one);
  omx_hrp_cascade_init(&two);
  omx_hrp_cascade_add(&one, coeffs, 1);
  omx_hrp_cascade_add(&two, coeffs, 1);
  omx_hrp_cascade_apply(&one, g_take, g_rendered, TEST_FRAMES);
  omx_hrp_cascade_apply(&two, g_take, g_rendered_b, TEST_FRAMES);
  check(memcmp(g_rendered, g_rendered_b, sizeof(g_rendered)) == 0,
        "the same take rendered twice is the same samples");
}

/* ---- 7. block size is not audible --------------------------------------------------------- */

static void test_streaming_in_blocks_gives_the_same_samples_as_one_call(void) {
  float coeffs[5];
  rbj_peaking((double)F0_HZ * 4.0, 5.0, -5.0, (double)TEST_RATE, coeffs);

  OmxHrpCascade whole, chunked;
  omx_hrp_cascade_init(&whole);
  omx_hrp_cascade_init(&chunked);
  omx_hrp_cascade_add(&whole, coeffs, 1);
  omx_hrp_cascade_add(&chunked, coeffs, 1);

  omx_hrp_cascade_apply(&whole, g_take, g_rendered, TEST_FRAMES);
  uint32_t off = 0;
  uint32_t block = 1u;
  while (off < TEST_FRAMES) {
    uint32_t n = block;
    if (n > TEST_FRAMES - off) n = TEST_FRAMES - off;
    omx_hrp_cascade_apply(&chunked, g_take + off, g_rendered_b + off, n);
    off += n;
    block = block * 3u + 7u; /* deliberately unaligned to any window or hop */
    if (block > 9001u) block = 1u;
  }
  check(memcmp(g_rendered, g_rendered_b, sizeof(g_rendered)) == 0,
        "a file streamed in odd blocks renders to the same samples as one call");
}

/* ---- 8. the offline and live paths project the SAME tracker to the SAME bytes -------------
 *
 * The two HRP shells read the tracker out into a voice set, and until the fold of audit D4 each
 * had its own seventeen lines to do it with — the live one (hrp_analyze.c) unoracled, because it
 * is N-API and cannot be compiled into a standalone test. A divergence between them would have
 * been a desk and a rendered file disagreeing about what the same performance contains, with no
 * gate anywhere able to see it.
 *
 * So this runs BOTH chains over the take: the offline render's own measuring pass, window by
 * window, and beside it the live chain in hrp_analyze.c's order — RTA spectrum, detect, track
 * update, project — over the very same window of samples, with the search the live tier sends
 * and the track config `hrpCreate` installs. After every window the two trackers are projected
 * and the voice sets are compared BYTE FOR BYTE, identities included. Twice: over the fixture's
 * own ladder, and over the fixture plus a second ladder a tritone up, because the packing of
 * more than one voice is a thing the two paths could differ about and one voice never shows it.
 *
 * What the comparison can and cannot see: the magnitudes are the same because §1 gives both
 * tiers one FFT, so what is under test here is everything from the detector's input onward —
 * detection, identity, and the marshalling this lane folded. The live desk's mags arrive across
 * N-API from the RTA the console already runs; that crossing is `hrp-analysis-tier` territory
 * and not this claim's.
 */

/* The live tier's own scratch. No carry buffer: window w of the render's slide is exactly
   `g_take + w * hop`, contiguous, so the live chain reads the same samples the render's carry
   held without a second copy of the window machine to get wrong. */
static float live_work_re[TEST_FFT], live_work_im[TEST_FFT];
static float live_mags[TEST_NBINS], live_residual[TEST_NBINS];

/* The fixture with a second ladder a TRITONE up, summed at half level so the pair stays inside
 * full scale. Two voices are needed because a projection that packed wrongly — dropped a slot,
 * left a hole, ordered by anything but the slot — cannot be caught in a window that only ever
 * holds one voice, and the sustained ladder never holds two. A tritone because it is the
 * interval whose harmonics coincide with the fundamental's least: 216.8 Hz's comb is 216.8,
 * 433.6, 650.4 … and 306.6 Hz's is 306.6, 613.2, 919.8 …, so the second voice is a voice and not
 * a harmonic of the first. It lands in `g_rendered_b`, which is spent scratch by the time this
 * claim runs (main() order), rather than in a second megabyte of statics. */
static void synth_polyphonic_take(void) {
  const double f2 = (double)F0_HZ * 1.4142135623730951; /* 2^(6/12) */
  for (uint32_t i = 0; i < TEST_FRAMES; i++) {
    double x = 0.5 * (double)g_take[i];
    for (uint32_t k = 1; k <= TEST_HARMONICS; k++)
      x += 0.5 * (double)LADDER[k - 1] *
           sin(2.0 * M_PI * (double)k * f2 * (double)i / (double)TEST_RATE);
    g_rendered_b[i] = (float)x;
  }
}

/* What one lockstep comparison found. Counts rather than checks, so 184 windows contribute one
 * verdict each to a handful of claims instead of 184 lines of noise. */
typedef struct {
  uint32_t windows;
  uint32_t nonempty;
  uint32_t mismatched;
  uint32_t id_mismatched;
  uint32_t most_voices;
  int first_bad;
} ParityRun;

static ParityRun compare_live_and_offline_over(const float *take) {
  ParityRun run;
  memset(&run, 0, sizeof(run));
  run.first_bad = -1;

  OmxHrpRenderConfig cfg = test_config();
  check(omx_hrp_render_init(&g_render_a, &cfg, carry_a, work_re_a, work_im_a, mags_a, residual_a)
            == 1,
        "the render binds for the parity run");
  /* Teach over the whole take, then rewind: the measuring pass starts from an EMPTY tracker,
     which is the state a live channel starts an arm from. That is what makes the two
     comparable at all — the baseline differs between the two shells (§0.5's whole-file
     privilege) and the tracker does not. */
  omx_hrp_render_feed(&g_render_a, take, TEST_FRAMES, OMX_HRP_RENDER_LEARN);
  omx_hrp_render_rewind(&g_render_a);

  /* The live side: a fresh tracker and the configs hrpCreate installs, narrowed by the same
     search the offline render was given (`hrpSearchFor`'s own range for an assigned family). */
  OmxHrpTracker live_tracker;
  omx_hrp_tracker_init(&live_tracker);
  const OmxHrpTrackConfig live_track_cfg = omx_hrp_track_config_default();
  const float bin_hz = (float)TEST_RATE / (float)TEST_FFT;

  uint32_t fed = 0;

  while (fed + (run.windows == 0u ? TEST_FFT : TEST_HOP) <= TEST_FRAMES) {
    /* Window w covers take[w * hop .. + fft): the render's carry slides by one hop per window,
       so feeding a whole window and then one hop at a time steps the two in lockstep over
       identical samples. */
    const uint32_t n = (run.windows == 0u) ? TEST_FFT : TEST_HOP;
    if (omx_hrp_render_feed(&g_render_a, take + fed, n, OMX_HRP_RENDER_MEASURE) != 1u) break;
    const uint32_t start = run.windows * TEST_HOP;
    fed += n;

    /* hrp_analyze.c's chain, in its order, over the same window. */
    OmxHrpVoiceSet detected;
    omx_rta_spectrum(take + start, TEST_FFT, live_work_re, live_work_im, live_mags);
    omx_hrp_detect(live_mags, TEST_NBINS, bin_hz, &cfg.pitch, live_residual, &detected);
    omx_hrp_track_update(&live_tracker, &detected, &live_track_cfg);

    OmxHrpVoiceSet live_set;
    const OmxHrpTrack *live_by_slot[OMX_HRP_MAX_VOICES];
    const OmxHrpTrack *offline_by_slot[OMX_HRP_MAX_VOICES];
    omx_hrp_project_voices(&live_tracker, &live_set, live_by_slot);
    /* The OFFLINE side is not recomputed here: `r->voices` is the set the window itself
       projected and handed to attribution. Recomputing it from the tracker would compare this
       test's arithmetic with itself and pass however the render marshalled. */
    const OmxHrpVoiceSet offline_set = g_render_a.voices;
    /* by_slot is the only route to an IDENTITY — a voice set carries no id — so the offline
       tracker is projected once more purely for its slots, and never for the bytes compared. */
    OmxHrpVoiceSet offline_slots_only;
    omx_hrp_project_voices(&g_render_a.tracker, &offline_slots_only, offline_by_slot);

    if (memcmp(&live_set, &offline_set, sizeof(live_set)) != 0) {
      if (run.first_bad < 0) run.first_bad = (int)run.windows;
      run.mismatched++;
    }
    for (uint32_t i = 0; i < live_set.voice_count; i++) {
      if (live_by_slot[i] == NULL || offline_by_slot[i] == NULL ||
          live_by_slot[i]->id != offline_by_slot[i]->id)
        run.id_mismatched++;
    }
    if (live_set.voice_count > 0u) run.nonempty++;
    if (live_set.voice_count > run.most_voices) run.most_voices = live_set.voice_count;
    run.windows++;
  }
  return run;
}

static void assert_parity(const ParityRun *run, const char *what) {
  printf("hrp_render: %s — %u windows compared, %u byte-identical, most voices %u\n", what,
         run->windows, run->windows - run->mismatched, run->most_voices);
  if (run->first_bad >= 0) fprintf(stderr, "  first divergent window: %d\n", run->first_bad);

  check(run->windows > 150u, "the two chains were stepped over the whole take");
  check(run->mismatched == 0u,
        "every window's voice set is byte-identical between live and offline");
  check(run->id_mismatched == 0u,
        "and the two chains agree on the voices' IDENTITIES, not just their numbers");
  /* THE GUARD ON THE CLAIM ABOVE: two empty sets are byte-identical for free, and a comparison
     that only ever compared silence would pass while proving nothing. Both takes are sustained
     ladders, so most windows must carry a voice. */
  check(run->nonempty > run->windows / 2u, "and the sets compared were voices, not empty sets");
}

static void test_the_offline_and_live_paths_project_the_same_voice_set(void) {
  ParityRun mono = compare_live_and_offline_over(g_take);
  assert_parity(&mono, "one voice");
  check(mono.most_voices >= 1u, "the fixture's own ladder is projected as a voice");

  synth_polyphonic_take();
  ParityRun poly = compare_live_and_offline_over(g_rendered_b);
  assert_parity(&poly, "two voices");
  /* The packing itself under comparison: a window holding more than one voice is the only one
     in which a dropped slot or a hole could differ between the two paths. */
  check(poly.most_voices >= 2u, "the tritone pair is projected as two PACKED voices");
}

/* ---- the refusals ------------------------------------------------------------------------- */

static void test_an_unusable_config_refuses_rather_than_rendering_nonsense(void) {
  OmxHrpRenderConfig cfg = test_config();
  OmxHrpRender r;

  cfg.fft_size = 8191u; /* not a power of two */
  check(omx_hrp_render_init(&r, &cfg, carry_a, work_re_a, work_im_a, mags_a, residual_a) == 0,
        "a window that is not a power of two is refused");

  cfg = test_config();
  cfg.hop = 0u;
  check(omx_hrp_render_init(&r, &cfg, carry_a, work_re_a, work_im_a, mags_a, residual_a) == 0,
        "a zero hop is refused");

  cfg = test_config();
  cfg.hop = cfg.fft_size + 1u;
  check(omx_hrp_render_init(&r, &cfg, carry_a, work_re_a, work_im_a, mags_a, residual_a) == 0,
        "a hop longer than the window is refused");

  cfg = test_config();
  check(omx_hrp_render_init(&r, &cfg, NULL, work_re_a, work_im_a, mags_a, residual_a) == 0,
        "a missing buffer is refused");
  check(omx_hrp_render_init(&r, &cfg, carry_a, work_re_a, work_im_a, mags_a, residual_a) == 1,
        "the render's own config is accepted");
  check(omx_hrp_render_feed(&r, NULL, 128u, 1) == 0u, "feeding nothing analyses nothing");

  OmxHrpCascade full;
  omx_hrp_cascade_init(&full);
  float coeffs[5];
  rbj_peaking(1000.0, 5.0, -3.0, (double)TEST_RATE, coeffs);
  for (uint32_t i = 0; i < OMX_EQ_MAX_BANDS; i++)
    check(omx_hrp_cascade_add(&full, coeffs, 1) == 1, "a section below the cap is taken");
  check(omx_hrp_cascade_add(&full, coeffs, 1) == 0,
        "a section past the cascade's capacity is REFUSED, never dropped silently");
}

int main(void) {
  synth_take();

  test_a_planted_resonance_is_measured_and_its_neighbours_are_not();
  test_the_takes_opening_is_measured_only_because_the_file_taught_first();
  test_the_render_applies_exactly_the_filter_it_was_given();
  test_a_render_with_nothing_to_correct_is_the_original();
  test_the_same_take_renders_to_the_same_file_twice();
  test_streaming_in_blocks_gives_the_same_samples_as_one_call();
  test_the_offline_and_live_paths_project_the_same_voice_set();
  test_an_unusable_config_refuses_rather_than_rendering_nonsense();

  printf("hrp_render: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
