// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Standalone unit test for the SHARED correcting half (hrp_correct.h) —
 * docs/design/specs/2026-08-18-hrp-polyphonic-architecture.md §16/§23/§25/§26/§29/§30:
 *   cc -Wall -Wextra -O2 -o build/hrp_correct_test src/hrp_correct.test.c -lm && ./build/hrp_correct_test
 * (also driven from `pnpm test` via the test:dsp script).
 *
 * This header is what the LIVE LV2 shell links instead of calling TypeScript, so every arm
 * below asks the same question in a different place: does the C say what the console says?
 *
 *   1. THE CURVE IS THE DESIGN'S. Every row of hrp_peaking_corpus.h is reproduced by
 *      omx_hrp_peaking_coeffs to float precision, AND is independently recomputed here in
 *      double from the design's mathematics — so the corpus cannot be quietly wrong and still
 *      agreed with. A corpus that only its own producer checks proves nothing.
 *
 *   2. A 0 dB BELL IS IDENTITY, provably: A = 1 collapses numerator and denominator term for
 *      term, so b0=a0, b1=a1, b2=a2 and the section passes every sample through untouched.
 *      This is what `eqBandIsIdentity` asserts on the TypeScript side, and it is why a parked
 *      band can be skipped without changing a sample.
 *
 *   3. THE DESIGNED FILTER DOES WHAT IT SAYS AT THE FREQUENCY IT NAMES. A −6 dB bell at 1 kHz
 *      attenuates a 1 kHz tone by 6 dB and leaves a distant tone alone — measured through the
 *      cascade on real samples, against a closed-form transfer function, not against itself.
 *
 *   4. §16: THE FAMILY IS A BROAD PRIOR, ten of them, each narrowing the search — and NONE of
 *      them moving the confidence floor, which is the one rule a transcription would plausibly
 *      get wrong (a narrowed budget would raise the floor to 0.25 at two voices and the family
 *      would go deaf on its own instrument).
 *
 *   5. §23/§25: ONLY THE PART ABOVE THE THRESHOLD IS CORRECTABLE, the amount scales it, the cap
 *      bounds it, and nothing on this path can ever return a boost.
 *
 *   6. §29/§30: ONE GLOBALLY RANKED LIST. Every voice's candidates meet before anything is
 *      spent; a near-twin inside the collision window is discarded rather than deepening a
 *      bell; collisions resolve BEFORE the budget, so a discarded twin never occupies a rank a
 *      distant candidate deserved; and the order does not depend on the input order.
 *
 *   7. BYPASS IS BIT-PRESERVING, which is the whole reason a live insert may be left racked.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_attribute.h> /* for §11's window, which §30's must not collide with */
#include <omxdsp/analysis/omx_hrp_correct.h>
#include "hrp_peaking_corpus.h"

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

/* The MATCHED peaking section, recomputed here in double straight from the mathematics — an
 * oracle INDEPENDENT of the implementation under test and of the generator that wrote the
 * corpus. The prototype is (s^2 + A s/Q + 1)/(s^2 + s/(A Q) + 1) with A = 10^(g/40); both pairs
 * are finite, so the matched-Z map z = exp(sT) places all four poles and zeros exactly and one
 * scalar sets unity at DC.
 *
 * `out[2]` — the pair's polynomial at z = 1 — is FACTORED, not summed, and that is not a
 * stylistic echo of the implementation. Writing it as `1 + p1 + p2` was tried here first and
 * this test caught it: for a 40 Hz bell at 192 kHz p1 -> -2 and p2 -> 1, and the sum throws
 * away eight digits of an answer that is itself the size of the digits it threw away. The two
 * forms disagreed by 1.6e-8 on the 0.25 Hz row. An oracle may be written differently from the
 * code it judges; it may not be written worse. */
static void matched_pair_oracle(double wn, double zeta, double out[3]) {
  const double w = wn > M_PI ? M_PI : wn;
  if (zeta < 1.0) {
    const double e = exp(-zeta * w), th = sqrt(1.0 - zeta * zeta) * w;
    const double ec = e * cos(th), es = e * sin(th);
    out[0] = -2.0 * ec;
    out[1] = e * e;
    out[2] = (1.0 - ec) * (1.0 - ec) + es * es;
  } else {
    const double sq = sqrt(zeta * zeta - 1.0);
    const double z1 = exp(-w / (zeta + sq)), z2 = exp(-w * (zeta + sq));
    out[0] = -(z1 + z2);
    out[1] = z1 * z2;
    out[2] = (1.0 - z1) * (1.0 - z2);
  }
}
static void matched_peaking(double f, double q, double g, double rate, double out[5]) {
  const double nyq = rate * 0.5;
  double f0 = f < 1.0 ? 1.0 : f;
  if (f0 > nyq * 0.999) f0 = nyq * 0.999;
  const double qq = q <= 0.0 ? 1e-3 : q;
  const double w0 = 2.0 * M_PI * f0 / rate;
  const double A = pow(10.0, g / 40.0);
  double p[3], z[3];
  matched_pair_oracle(w0, 1.0 / (2.0 * A * qq), p);
  matched_pair_oracle(w0, A / (2.0 * qq), z);
  const double k = p[2] / z[2];
  out[0] = k; out[1] = k * z[0]; out[2] = k * z[1]; out[3] = p[0]; out[4] = p[1];
}

/* |H(e^jw)| for a normalised biquad — the closed form, so a measured ratio is checked against
 * mathematics rather than against another run of the same cascade. */
static double biquad_mag(const float c[5], double f_hz, double rate) {
  const double w = 2.0 * M_PI * f_hz / rate;
  const double cw = cos(w), sw = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  const double nr = c[0] + c[1] * cw + c[2] * c2, ni = -(c[1] * sw + c[2] * s2);
  const double dr = 1.0 + c[3] * cw + c[4] * c2, di = -(c[3] * sw + c[4] * s2);
  return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

static void test_the_curve_is_the_designs(void) {
  check(OMX_HRP_PEAKING_CORPUS_ROWS >= 30u, "the corpus is not a token two rows");
  for (uint32_t i = 0; i < OMX_HRP_PEAKING_CORPUS_ROWS; i++) {
    const OmxHrpPeakingRow *r = &OMX_HRP_PEAKING_CORPUS[i];
    double want[5];
    matched_peaking(r->freq_hz, r->q, r->gain_db, (double)r->rate, want);
    float got[5];
    omx_hrp_peaking_coeffs((float)r->freq_hz, (float)r->q, (float)r->gain_db, r->rate, got);
    for (int k = 0; k < 5; k++) {
      /* The corpus agrees with an independent recomputation ... */
      close_to(r->coeffs[k], want[k], 1e-12, "corpus row matches the design recomputed here");
      /* ... and the shipped C agrees with the corpus, to float32's seven figures. */
      const double tol = 1e-6 * (fabs(r->coeffs[k]) + 1.0);
      close_to((double)got[k], r->coeffs[k], tol, r->why);
    }
  }
}

static void test_a_zero_db_bell_is_identity(void) {
  float c[5];
  omx_hrp_peaking_coeffs(1000.0f, OMX_HRP_BAND_Q, 0.0f, 48000u, c);
  close_to(c[0], 1.0, 1e-7, "0 dB: b0 = 1");
  close_to(c[1], c[3], 1e-7, "0 dB: b1 = a1");
  close_to(c[2], c[4], 1e-7, "0 dB: b2 = a2");

  /* AND THIS IS WHY THE BAND IS PARKED RATHER THAN RUN. The section is identity as
   * MATHEMATICS — numerator and denominator equal term for term — but a biquad is a
   * recursion, and running it in float32 accumulates rounding that exact cancellation in
   * real arithmetic would not have. Measured below: a resonant (Q=5) identity bell run over
   * a full-scale tone deviates by ~8e-06, about -102 dBFS. Inaudible, and still not zero.
   * `eqBandIsIdentity` reads the coefficient property above and parks the section; the
   * parked path IS bit-exact, and test 7 measures that. So the two claims are different and
   * both are made: identity COEFFICIENTS are provable, identity ARITHMETIC is not, and the
   * design is built on the first. */
  OmxHrpCascade cas;
  omx_hrp_cascade_init(&cas);
  check(omx_hrp_cascade_add(&cas, c, 1) == 1, "an identity section is still a section");
  float sig[512], out[512];
  for (int i = 0; i < 512; i++) sig[i] = sinf(2.0f * (float)M_PI * 1000.0f * (float)i / 48000.0f);
  omx_hrp_cascade_apply(&cas, sig, out, 512u);
  double worst = 0.0;
  for (int i = 0; i < 512; i++) {
    const double e = fabs((double)out[i] - (double)sig[i]);
    if (!(e <= worst)) worst = e; /* not fmax: tools/reduction-check.sh */
  }
  check(worst < 1e-4, "a 0 dB bell RUN is identity to within float rounding (< -80 dBFS)");
  check(worst > 0.0, "but not bit-exact, which is what parking the band is for");
}

static double rms_at(const float *x, uint32_t n) {
  double s = 0.0;
  for (uint32_t i = 0; i < n; i++) s += (double)x[i] * (double)x[i];
  return sqrt(s / (double)n);
}

static void test_the_filter_does_what_it_names(void) {
  enum { N = 48000 };
  static float on[N], off[N], out[N];
  const double rate = 48000.0;
  for (int i = 0; i < N; i++) {
    on[i] = sinf(2.0f * (float)M_PI * 1000.0f * (float)i / (float)rate);
    off[i] = sinf(2.0f * (float)M_PI * 130.0f * (float)i / (float)rate);
  }
  float c[5];
  omx_hrp_peaking_coeffs(1000.0f, OMX_HRP_BAND_Q, -6.0f, 48000u, c);

  OmxHrpCascade cas;
  omx_hrp_cascade_init(&cas);
  omx_hrp_cascade_add(&cas, c, 1);
  omx_hrp_cascade_apply(&cas, on, out, (uint32_t)N);
  /* Skip the filter's own settling before measuring a steady-state ratio. */
  const uint32_t skip = 4096u, span = (uint32_t)N - skip;
  double ratio_on = rms_at(out + skip, span) / rms_at(on + skip, span);
  close_to(20.0 * log10(ratio_on), -6.0, 0.05, "a -6 dB bell takes 6 dB off the tone it names");
  close_to(20.0 * log10(ratio_on), 20.0 * log10(biquad_mag(c, 1000.0, rate)), 0.05,
           "and the measured ratio equals the closed-form |H| there");

  omx_hrp_cascade_init(&cas);
  omx_hrp_cascade_add(&cas, c, 1);
  omx_hrp_cascade_apply(&cas, off, out, (uint32_t)N);
  double ratio_off = rms_at(out + skip, span) / rms_at(off + skip, span);
  check(fabs(20.0 * log10(ratio_off)) < 0.6,
        "and leaves a tone three octaves below almost untouched");
}

static void test_the_family_is_a_broad_prior(void) {
  const OmxHrpConfig wide = omx_hrp_config_default();
  const float floor_now = omx_hrp_min_confidence_for(OMX_HRP_MAX_VOICES);

  for (int f = 0; f < OMX_HRP_FAMILY_COUNT; f++) {
    OmxHrpConfig c = omx_hrp_family_search(f);
    check(c.max_voices >= 1u && c.max_voices <= OMX_HRP_MAX_VOICES,
          "a family's voice count is inside the console's budget");
    check(c.f0_min_hz > 0.0f && c.f0_max_hz > c.f0_min_hz, "a family's search range is ordered");
    /* THE RULE A TRANSCRIPTION WOULD GET WRONG: the floor is the console's, never the
     * family's narrowed one. Deriving it from max_voices would give 0.5 for drums. */
    close_to(c.min_confidence, floor_now, 1e-9, "a family never moves the confidence floor");
  }

  close_to(omx_hrp_family_search(OMX_HRP_FAMILY_DRUMS).f0_max_hz, 400.0, 1e-6,
           "drums: no fundamental above 400 Hz, so no cut on a crash");
  check(omx_hrp_family_search(OMX_HRP_FAMILY_DRUMS).max_voices == 1u, "a shell sounds one note");
  check(omx_hrp_family_search(OMX_HRP_FAMILY_GUITAR).max_voices == 6u, "six strings, six voices");
  check(omx_hrp_family_search(OMX_HRP_FAMILY_KEYS).max_voices == 8u, "a two-handed voicing");
  check(omx_hrp_family_search(OMX_HRP_FAMILY_STRINGS).max_voices == 8u, "a section is polyphonic");
  close_to(omx_hrp_family_search(OMX_HRP_FAMILY_KEYS).f0_min_hz, 27.5, 1e-6, "keys: A0");

  /* Absence is a fact: no family means the console's own defaults, not a copy of them. */
  OmxHrpConfig none = omx_hrp_family_search(OMX_HRP_FAMILY_NONE);
  close_to(none.f0_min_hz, wide.f0_min_hz, 1e-9, "no family: the console's own low bound");
  close_to(none.f0_max_hz, wide.f0_max_hz, 1e-9, "no family: the console's own high bound");
  check(none.max_voices == wide.max_voices, "no family: the console's own voice budget");
  OmxHrpConfig bogus = omx_hrp_family_search(4242);
  close_to(bogus.f0_max_hz, wide.f0_max_hz, 1e-9, "an unknown family reads as no family");
}

static void test_only_the_excess_is_correctable(void) {
  close_to(omx_hrp_cut_db(3.0f, 1.0f, OMX_HRP_MAX_CUT_DB), 0.0, 1e-7,
           "a deviation AT the threshold is expression, not resonance");
  close_to(omx_hrp_cut_db(2.0f, 1.0f, OMX_HRP_MAX_CUT_DB), 0.0, 1e-7,
           "and below it there is nothing to correct");
  close_to(omx_hrp_cut_db(9.0f, 1.0f, OMX_HRP_MAX_CUT_DB), 6.0, 1e-6,
           "9 dB hot, full amount: only the 6 above the threshold, and the cap allows it");
  close_to(omx_hrp_cut_db(7.0f, 0.5f, OMX_HRP_MAX_CUT_DB), 2.0, 1e-6,
           "the operator's amount scales what remains above the threshold");
  close_to(omx_hrp_cut_db(30.0f, 1.0f, OMX_HRP_MAX_CUT_DB), 6.0, 1e-6,
           "and the cap bounds a wild measurement");
  close_to(omx_hrp_cut_db(9.0f, 0.0f, OMX_HRP_MAX_CUT_DB), 0.0, 1e-7,
           "amount zero authorises nothing");
  /* §25 in the sign: there is no input on this path that returns a boost. */
  for (float e = -20.0f; e <= 40.0f; e += 0.25f)
    check(omx_hrp_cut_db(e, 1.0f, OMX_HRP_MAX_CUT_DB) >= 0.0f, "the cut is never a boost");

  close_to(omx_hrp_resonance_score(4.0f, 0.5f, 0.5f), 1.0, 1e-6, "score = cut x voice x partial");
  close_to(omx_hrp_resonance_score(4.0f, 0.0f, 0.5f), 0.0, 1e-9, "an unsure voice scores zero");
  close_to(omx_hrp_resonance_score(0.0f, 0.9f, 0.9f), 0.0, 1e-9, "no cut is no candidate");
}

static OmxHrpCandidate cand(float hz, float cut, float score, int32_t voice, uint32_t h) {
  OmxHrpCandidate c;
  c.freq_hz = hz; c.cut_db = cut; c.score = score; c.voice_id = voice; c.harmonic = h;
  return c;
}

static void test_one_globally_ranked_list(void) {
  /* §29: two voices' candidates meet in ONE list. Voice 7's single strong partial must beat
   * voice 3's two weaker ones — which independent per-voice allocation could not do. */
  OmxHrpCandidate a[6];
  a[0] = cand(400.0f, 2.0f, 0.8f, 3, 2);
  a[1] = cand(800.0f, 2.0f, 0.6f, 3, 4);
  a[2] = cand(1500.0f, 5.0f, 4.0f, 7, 3);
  a[3] = cand(2500.0f, 1.0f, 0.2f, 7, 5);
  a[4] = cand(3300.0f, 3.0f, 2.0f, 3, 8);
  a[5] = cand(5000.0f, 0.0f, 0.0f, 7, 9);
  uint32_t kept = omx_hrp_select(a, 6u, OMX_HRP_SELECT_COLLISION_CENTS, 2u);
  check(kept == 2u, "the budget is the channel's, and it is spent on two");
  check(a[0].voice_id == 7 && a[0].harmonic == 3u, "the strongest partial wins, whoever sounded it");
  check(a[1].voice_id == 3 && a[1].harmonic == 8u, "then the next strongest, across voices");

  /* A scoreless candidate is not a candidate, even with slots to spare. */
  OmxHrpCandidate z[2];
  z[0] = cand(1000.0f, 0.0f, 0.0f, 1, 2);
  z[1] = cand(2000.0f, 0.0f, 0.0f, 1, 3);
  check(omx_hrp_select(z, 2u, OMX_HRP_SELECT_COLLISION_CENTS, 8u) == 0u,
        "nothing to correct is not the weakest correction");

  /* §30: a near-twin inside the window is discarded, not merged into a deeper bell. */
  OmxHrpCandidate t[3];
  t[0] = cand(1000.0f, 4.0f, 3.0f, 1, 2);
  t[1] = cand(1030.0f, 4.0f, 2.9f, 2, 2); /* ~51 cents away: the same spectral region */
  t[2] = cand(1400.0f, 2.0f, 1.0f, 3, 2); /* ~583 cents away: its own region */
  check(omx_hrp_select(t, 3u, OMX_HRP_SELECT_COLLISION_CENTS, 8u) == 2u, "a near-twin is discarded");
  close_to(t[0].freq_hz, 1000.0, 1e-4, "the stronger of the pair is the one kept");
  close_to(t[1].freq_hz, 1400.0, 1e-4, "and the distant candidate takes the second slot");

  /* Collisions resolve BEFORE the budget: with one slot, the twin must not have consumed the
   * rank the distant candidate deserved — the whole reason the order is fixed this way. */
  OmxHrpCandidate u[3];
  u[0] = cand(1000.0f, 4.0f, 3.0f, 1, 2);
  u[1] = cand(1030.0f, 4.0f, 2.9f, 2, 2);
  u[2] = cand(1400.0f, 2.0f, 1.0f, 3, 2);
  check(omx_hrp_select(u, 3u, OMX_HRP_SELECT_COLLISION_CENTS, 1u) == 1u, "one slot spends one");
  close_to(u[0].freq_hz, 1000.0, 1e-4, "on the strongest region");

  /* The answer does not depend on the order the caller happened to build the array in. */
  OmxHrpCandidate f[4], r[4];
  f[0] = cand(300.0f, 1.0f, 1.0f, 1, 2);
  f[1] = cand(900.0f, 3.0f, 3.0f, 2, 3);
  f[2] = cand(2700.0f, 2.0f, 2.0f, 3, 4);
  f[3] = cand(8100.0f, 4.0f, 4.0f, 4, 5);
  for (int i = 0; i < 4; i++) r[i] = f[3 - i];
  uint32_t kf = omx_hrp_select(f, 4u, OMX_HRP_SELECT_COLLISION_CENTS, 4u);
  uint32_t kr = omx_hrp_select(r, 4u, OMX_HRP_SELECT_COLLISION_CENTS, 4u);
  check(kf == 4u && kr == 4u, "four distinct regions, four slots, four kept");
  for (uint32_t i = 0; i < kf; i++)
    close_to(f[i].freq_hz, r[i].freq_hz, 1e-4, "reversing the input does not reorder the answer");

  /* Ties break by voice then harmonic, so equal scores still give one fixed answer. */
  OmxHrpCandidate e[2];
  e[0] = cand(2000.0f, 1.0f, 1.0f, 9, 2);
  e[1] = cand(4000.0f, 1.0f, 1.0f, 4, 7);
  check(omx_hrp_select(e, 2u, OMX_HRP_SELECT_COLLISION_CENTS, 2u) == 2u, "both regions are kept");
  check(e[0].voice_id == 4, "an exact tie breaks by voice id, not by input order");

  check(omx_hrp_select(NULL, 4u, OMX_HRP_SELECT_COLLISION_CENTS, 4u) == 0u, "no candidates, no answer");
  check(omx_hrp_select(f, 4u, OMX_HRP_SELECT_COLLISION_CENTS, 0u) == 0u, "no budget, nothing spent");
}

static void test_bypass_is_bit_preserving(void) {
  float sig[333], out[333];
  for (int i = 0; i < 333; i++) sig[i] = sinf((float)i * 0.11f) * 0.7f + 0.01f * (float)(i % 7);

  OmxHrpCascade empty;
  omx_hrp_cascade_init(&empty);
  check(omx_hrp_cascade_live(&empty) == 0u, "an empty cascade runs nothing");
  omx_hrp_cascade_apply(&empty, sig, out, 333u);
  check(memcmp(sig, out, sizeof(sig)) == 0, "an empty cascade is a byte-for-byte copy");

  float ident[5];
  omx_hrp_peaking_coeffs(1000.0f, OMX_HRP_BAND_Q, 0.0f, 48000u, ident);
  OmxHrpCascade parked;
  omx_hrp_cascade_init(&parked);
  omx_hrp_cascade_add(&parked, ident, 0);
  check(omx_hrp_cascade_live(&parked) == 0u, "a parked band is not a live section");
  omx_hrp_cascade_apply(&parked, sig, out, 333u);
  check(memcmp(sig, out, sizeof(sig)) == 0, "and a parked band is byte-for-byte too");

  /* The refusal at the cap: a band that did not reach the audio must not be reported as if
   * it had. */
  OmxHrpCascade full;
  omx_hrp_cascade_init(&full);
  float c[5];
  omx_hrp_peaking_coeffs(1000.0f, OMX_HRP_BAND_Q, -3.0f, 48000u, c);
  for (uint32_t i = 0; i < OMX_EQ_MAX_BANDS; i++)
    check(omx_hrp_cascade_add(&full, c, 1) == 1, "a section below the cap is taken");
  check(omx_hrp_cascade_add(&full, c, 1) == 0, "and one past it is refused, never dropped");
}

/* The two "collision" windows are different laws and must stay different names.
 *
 * They shared a name for exactly one commit: hrp_attribute.h's is §11's ATTRIBUTION window (how
 * close two partials sit before the desk cannot tell which voice owns the energy — the
 * analyser's resolution) and hrp_correct.h's is §30's SELECTION window (how close two
 * CORRECTIONS sit before planting both merely deepens one bell — a Q-5 bell's half-width). The
 * redefinition was a warning that a grep for "failures" hid, so the guard is now the BUILD:
 * this binary and hrp_render_test compile with -Werror, and both include both headers. */
static void test_the_two_collision_windows_are_different_laws(void) {
  check(OMX_HRP_SELECT_COLLISION_CENTS != OMX_HRP_ATTRIBUTION_CENTS,
        "§30's selection window is not §11's attribution window");
  /* And the selection window is the wider of the two, which is the direction that matters: two
   * partials the analyser CAN separate may still be one correction. */
  check(OMX_HRP_SELECT_COLLISION_CENTS > OMX_HRP_ATTRIBUTION_CENTS,
        "a correction covers more of the spectrum than a measurement resolves");
}

int main(void) {
  test_the_two_collision_windows_are_different_laws();
  test_the_curve_is_the_designs();
  test_a_zero_db_bell_is_identity();
  test_the_filter_does_what_it_names();
  test_the_family_is_a_broad_prior();
  test_only_the_excess_is_correctable();
  test_one_globally_ranked_list();
  test_bypass_is_bit_preserving();

  printf("hrp_correct: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
