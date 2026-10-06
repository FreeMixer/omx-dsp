// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Oracle for the native DE-ESSER stage (mix_deesser.h). Compile + run:
 *   cc -Wall -Wextra -Werror -O2 -o build/mix_deesser_test src/mix_deesser.test.c -lm \
 *     && ./build/mix_deesser_test
 *
 * Spec: docs/design/specs/2026-09-22-native-deesser-stage.md §10 (issue #891).
 *
 * WHAT THIS FILE IS FOR. A de-esser's whole claim is DISCRIMINATION: it must move the band and
 * leave everything else alone. A test that only measures "the level went down" would pass over a
 * plain compressor, which is exactly the defect this stage exists to avoid — so every arm here
 * measures TWO frequencies, a sibilant at the band's centre and a vowel a long way below it, and
 * asserts the difference between what happened to them.
 *
 * THE EXPECTATION IS A CLOSED FORM, not a recorded number. In steady state the applied gain `g` is
 * static, so the stage's own algebra gives the output component exactly:
 *
 *     split     out(f) = in(f) * | 1 - (1 - g) * B(f) |      B = the band's COMPLEX response
 *                      = in(f) * | g + (1 - g) * N(f) |      N = 1 - B, the band's complement
 *     wideband  out(f) = in(f) * g
 *
 * and `g` itself is the floored ABOVE characteristic evaluated at the detector's own level, which
 * is `|B(f_s)|*A_s + |B(f_v)|*A_v` (a PEAK detector on a two-tone sees the sum of the magnitudes).
 * This file designs the section, evaluates B on the unit circle from those very coefficients, and
 * compares the measurement with the result — so a wrong band, a wrong characteristic, a missing
 * floor or a swapped path all move the number in a way the tolerance cannot absorb.
 *
 * THE BAND IS THE COOKBOOK BANDPASS, whose complement is the cookbook notch with |N| <= 1 at every
 * frequency — so split's `|g + (1 - g) N|` is <= 1 for every g in (0, 1], which is the stage's
 * `no-gain-added` law in split mode (spec §6; operator ruling, §1 gate line 3). The highpass·lowpass
 * pair this stage first shipped with has no such bound: |1 - (1 - g) H| > 1 wherever Re(H) < 0.
 * `test_split_never_exceeds_input` sweeps a probe over the audio band to MEASURE the bound, with
 * the sibilant hot enough to hold the stage at its floor so `g` is a constant and the measurement
 * is linear (a moving g would put intermodulation on the probe's own bin).
 *
 * EVERY DECLARED RATE, every arm (OMX_DECLARED_RATES: 44 100, 48 000, 88 200, 96 000, 176 400 and
 * 192 000 Hz, the four floor rates among them). The reductions are also asserted to AGREE across
 * them, which is the four-rate law's real content — a detector whose milliseconds silently became
 * samples passes at one rate and fails here.
 *
 * THE POSITIVE CONTROL IS IN THE SAME RUN (false-signals: a probe that reports absence must first
 * be shown able to report presence). `test_positive_control_band_moved` puts the detector band on
 * the VOWEL and requires the reduction to land there and NOT on the sibilant — the exact inversion
 * of the main arm. A run where both arms pass is a run where the measurement can tell the two
 * apart; a "no reduction outside the band" reading from a probe that has never reported presence
 * is not evidence of anything.
 *
 * THE BAND SECTION IS DESIGNED HERE with the RBJ cookbook, as mix_drive.test.c designs its own —
 * the same constant-0 dB-peak bandpass `@freemixer/core`'s `bandpass` kind is (cookbook
 * denominator and all, on purpose: |B|² + |N|² = 1 is an identity of the cookbook pair and of no
 * other), at the Q the cookbook's bandwidth-in-octaves relation gives `widthOct` at each rate. The KERNEL runs whatever
 * section it is handed, and every expectation below is evaluated from the section this file
 * actually handed it.
 *
 * SABOTAGE-VERIFIED (2026-09-22, this lane — an unverified guard is decoration), each run against
 * the shipped kernel and restored green afterwards:
 *   - the detector fed the UNFILTERED signal instead of the band (a de-esser turned back into a
 *     compressor)          -> 21 checks red; the split reduction reads −9.597 dB where the closed
 *                             form says −6.464, and the vowel moves +0.249 dB on a corpus that
 *                             carries no sibilant at all;
 *   - the range FLOOR removed -> 13 checks red; a shouted "s" with range −6 measures −28.12 dB of
 *                             reduction instead of −6.00;
 *   - the two MODES swapped   -> 46 checks red; split ducks the vowel by the same 8.935 dB as the
 *                             sibilant, which is the one thing a split de-esser must never do;
 *   - the BAND restored to the highpass·lowpass pair the stage first shipped with (the operator's
 *     2026-09-22 ruling, spec §1 gate line 3) -> 205 checks red, all of them in
 *     `test_split_never_exceeds_input`: the probe comes out LOUDER than it went in at 76 of the
 *     129 probes (worst +1.050 dB at 2 602 Hz at 44.1 k, +1.040 at 48 k, +0.993 at 96 k, +0.981
 *     at 192 k — the closed form |1 - (1 - g) H| tracks every one of them to 0.0000 dB, so the
 *     bump is the pair's own shape and not a measurement artefact), and the sibilant reads
 *     −13.2 dB instead of the −24 dB floor because that band is 1.9 dB down at its own centre.
 *     Every other arm in this file stayed green under the old band, which is why this one exists.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_deesser.h>

#include "fx_rates.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0, g_checks = 0;
static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}
static void check_near(double got, double want, double tol, const char *what) {
  g_checks++;
  if (!(fabs(got - want) <= tol)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s — got %.4f, want %.4f (+-%.4f)\n", what, got, want, tol);
  }
}

/* Every declared rate (OMX_DECLARED_RATES): the four floor rates and 88.2 and 176.4 kHz. Every arm
 * runs at every one of them. */
#define NRATES ((int)OMX_DECLARED_RATE_COUNT)

#define NB 16384u /* the measurement block; two passes are run and the SECOND is measured */

/* ---- measurement ---------------------------------------------------------------------- */

/* Magnitude of the component at NORMALISED frequency `f` (cycles per sample). The corpus puts
 * every tone on an exact bin of this length, so a rectangular window leaks nothing. */
static double bin_mag(const float *x, uint32_t n, double f) {
  double sr = 0.0, si = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    double a = 2.0 * M_PI * f * (double)i;
    sr += (double)x[i] * cos(a);
    si -= (double)x[i] * sin(a);
  }
  return 2.0 * sqrt(sr * sr + si * si) / (double)n;
}
static double db(double x) { return 20.0 * log10(x < 1e-30 ? 1e-30 : x); }

/* ---- the band, designed the way the control thread designs it -------------------------- */

/* RBJ band-pass (constant 0 dB peak), the cookbook, normalised to a0 — {b0,b1,b2,a1,a2} with the
 * a-terms SUBTRACTED, which is what omx_biquad expects. Its complement 1 - B is the cookbook notch
 * over the same denominator. */
static void rbj_bandpass(float c[5], double f0, double q, double sr) {
  double w = 2.0 * M_PI * f0 / sr, cw = cos(w), sw = sin(w), alpha = sw / (2.0 * q);
  double a0 = 1.0 + alpha;
  c[0] = (float)(alpha / a0);
  c[1] = 0.0f;
  c[2] = (float)(-alpha / a0);
  c[3] = (float)((-2.0 * cw) / a0);
  c[4] = (float)((1.0 - alpha) / a0);
}
/* The cookbook's bandwidth-in-octaves to Q relation, with its w0/sin(w0) warp term — core's
 * `bandwidthOctavesQ`, which `deEsserBandQ` is. */
static double bw_q(double f0, double bw_oct, double sr) {
  double w0 = 2.0 * M_PI * f0 / sr;
  return 1.0 / (2.0 * sinh(log(2.0) / 2.0 * bw_oct * w0 / sin(w0)));
}

/* ONE section's complex response at normalised frequency `fn`, from the coefficients themselves —
 * never from the design parameters, so a mistyped coefficient cannot hide behind the formula that
 * was meant to produce it. */
static void section_response(const float c[5], double fn, double *re, double *im) {
  double w = 2.0 * M_PI * fn;
  double cw = cos(w), sw = sin(w), cw2 = cos(2.0 * w), sw2 = sin(2.0 * w);
  double nr = c[0] + c[1] * cw + c[2] * cw2;
  double ni = -(c[1] * sw + c[2] * sw2);
  double dr = 1.0 + c[3] * cw + c[4] * cw2;
  double di = -(c[3] * sw + c[4] * sw2);
  double den = dr * dr + di * di;
  *re = (nr * dr + ni * di) / den;
  *im = (ni * dr - nr * di) / den;
}
/* The BAND's complex response: the one section. */
static void band_response(const struct omx_deess *p, double fn, double *re, double *im) {
  section_response(p->bp_c, fn, re, im);
}
static double band_mag(const struct omx_deess *p, double fn) {
  double re, im;
  band_response(p, fn, &re, &im);
  return sqrt(re * re + im * im);
}

/* ---- the stage's defaults, as the controller pushes them ------------------------------- */

/* Spec §4a/§4d: the row's defaults for a vocal, plus the three constants the controller holds
 * (knee 6 dB, PEAK detection, no make-up). `range_db` is overridden per arm. */
static struct omx_deess atom(double sr, double centre_hz, double width_oct, double thresh_db,
                             double ratio, double range_db, int mode) {
  struct omx_deess p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.mode = mode;
  p.dyn.enabled = 1;
  p.dyn.gc.mode = OMX_DYN_ABOVE;
  p.dyn.detect = OMX_DETECT_PEAK;
  p.dyn.gc.thresh_db = (float)thresh_db;
  p.dyn.gc.ratio = (float)ratio;
  p.dyn.gc.knee_db = 6.0f;
  p.dyn.gc.range_db = (float)range_db;
  p.dyn.gc.makeup_lin = 1.0f;
  p.dyn.attack_ms = 1.0f;
  p.dyn.ovs_mode = OMX_DYN_OVS_OFF;
  p.dyn.attack_coeff = omx_pole_from_time_ms(1.0f, (float)sr);
  p.dyn.release_coeff = omx_pole_from_time_ms(60.0f, (float)sr);
  rbj_bandpass(p.bp_c, centre_hz, bw_q(centre_hz, width_oct, sr), sr);
  return p;
}

/* ---- the corpus: a sibilant over a vowel ------------------------------------------------ */

struct corpus {
  double fv, fs;   /* the two tones, NORMALISED (exact bins of NB) */
  double av, as;   /* their amplitudes */
};

/* Snap a frequency to an exact bin of NB at `sr`, so the measurement leaks nothing. */
static double snap(double hz, double sr) {
  double k = floor(hz * (double)NB / sr + 0.5);
  return k / (double)NB;
}

static void fill(float *x, uint32_t n, const struct corpus *c, uint32_t phase) {
  for (uint32_t i = 0; i < n; i++) {
    double t = (double)(i + phase);
    x[i] = (float)(c->av * sin(2.0 * M_PI * c->fv * t) + c->as * sin(2.0 * M_PI * c->fs * t));
  }
}

/* Run the stage over TWO blocks and measure the SECOND — the first is the detector's attack
 * transient, and reading it would measure the ramp rather than the stage. Returns the measured
 * change in dB at the two tones (negative = reduction). */
static void run_two_tone(const struct omx_deess *p, const struct corpus *c, uint32_t chunk,
                         double *d_vowel, double *d_sib) {
  static float buf[NB];
  struct omx_deess_state st;
  omx_deess_state_init(&st);
  for (int pass = 0; pass < 2; pass++) {
    fill(buf, NB, c, (uint32_t)pass * NB);
    for (uint32_t off = 0; off < NB; off += chunk) {
      uint32_t m = (NB - off < chunk) ? (NB - off) : chunk;
      omx_deess_process(buf + off, NULL, m, p, &st);
    }
  }
  *d_vowel = db(bin_mag(buf, NB, c->fv)) - db(c->av);
  *d_sib = db(bin_mag(buf, NB, c->fs)) - db(c->as);
}

/* THE CLOSED FORM the measurement is held to (file header): the detector level a PEAK detector
 * settles on for this two-tone, through the floored ABOVE characteristic, through the stage's own
 * algebra at the frequency asked about. */
static double expected_change_db(const struct omx_deess *p, const struct corpus *c, double fn) {
  double level = band_mag(p, c->fs) * c->as + band_mag(p, c->fv) * c->av;
  double g = pow(10.0, omx_deess_gain_db(p, (float)level) / 20.0);
  if (p->mode == OMX_DEESS_WIDEBAND) return db(g);
  double re, im;
  band_response(p, fn, &re, &im);
  double sr_ = 1.0 - (1.0 - g) * re, si_ = -(1.0 - g) * im;
  return db(sqrt(sr_ * sr_ + si_ * si_));
}

/* ---- the arms --------------------------------------------------------------------------- */

/* A vocal corpus: a 1 kHz vowel at -12 dBFS and a 7 kHz sibilant at -16.5 dBFS. The sibilant sits
 * at the band's centre and the vowel 2.8 octaves below it. */
static struct corpus vocal_corpus(double sr) {
  struct corpus c;
  c.fv = snap(1000.0, sr);
  c.fs = snap(7000.0, sr);
  c.av = 0.25;
  c.as = 0.15;
  return c;
}

static void test_bypass_is_bit_identical(double sr) {
  static float a[NB], b[NB];
  struct corpus c = vocal_corpus(sr);
  fill(a, NB, &c, 0);
  memcpy(b, a, sizeof a);
  struct omx_deess p = atom(sr, 7000.0, 1.0, -30.0, 4.0, -24.0, OMX_DEESS_SPLIT);
  p.enabled = 0;
  struct omx_deess_state st, zero;
  omx_deess_state_init(&st);
  omx_deess_state_init(&zero);
  for (uint32_t off = 0; off < NB; off += 64u) omx_deess_process(b + off, NULL, 64u, &p, &st);
  check(memcmp(a, b, sizeof a) == 0, "bypass: every sample bit-identical");
  check(memcmp(&st, &zero, sizeof st) == 0, "bypass: not one state word advanced");
}

static void test_below_threshold_passes(double sr) {
  struct corpus c = vocal_corpus(sr);
  c.as = 0.0; /* the vowel alone — nothing in the band */
  struct omx_deess p = atom(sr, 7000.0, 1.0, -30.0, 4.0, -24.0, OMX_DEESS_SPLIT);
  double dv, ds;
  run_two_tone(&p, &c, 64u, &dv, &ds);
  (void)ds;
  check_near(dv, 0.0, 0.05, "below threshold: the vowel passes untouched");
}

static double test_split_reduces_the_band_only(double sr, double *deviation) {
  struct corpus c = vocal_corpus(sr);
  struct omx_deess p = atom(sr, 7000.0, 1.0, -30.0, 4.0, -24.0, OMX_DEESS_SPLIT);
  double dv, ds;
  run_two_tone(&p, &c, 64u, &dv, &ds);
  const double want = expected_change_db(&p, &c, c.fs);
  check_near(ds, want, 1.0, "split: the sibilant follows the closed form");
  check_near(dv, expected_change_db(&p, &c, c.fv), 0.3, "split: the vowel follows the closed form");
  check(ds < -4.0, "split: the sibilant is measurably reduced");
  check(fabs(dv) < 0.5, "split: the vowel is left alone");
  check(ds < dv - 3.0, "split: the stage DISCRIMINATES — band moved, vowel not");
  printf("  %6.0f Hz  split    sibilant %+7.3f dB (closed form %+7.3f, deviation %+6.3f)   vowel %+6.3f dB\n",
         sr, ds, want, ds - want, dv);
  *deviation = ds - want;
  return ds;
}

static void test_wideband_reduces_both(double sr) {
  struct corpus c = vocal_corpus(sr);
  struct omx_deess p = atom(sr, 7000.0, 1.0, -30.0, 4.0, -24.0, OMX_DEESS_WIDEBAND);
  double dv, ds;
  run_two_tone(&p, &c, 64u, &dv, &ds);
  double want = expected_change_db(&p, &c, c.fs);
  check_near(ds, want, 1.0, "wideband: the sibilant follows the closed form");
  check_near(dv, want, 1.0, "wideband: the vowel follows the SAME closed form");
  check(fabs(ds - dv) < 0.3, "wideband: both tones move by the same gain");
  check(ds < -4.0, "wideband: the strip is measurably ducked");
  printf("  %6.0f Hz  wideband sibilant %+7.3f dB   vowel %+7.3f dB (closed form %+7.3f)\n", sr, ds,
         dv, want);
}

/* THE POSITIVE CONTROL. The same corpus, the same measurement, the band moved onto the VOWEL: the
 * reduction must land on 1 kHz and not on 7 kHz. Without this arm, "the vowel did not move" is a
 * reading from a probe that has never been shown able to see a vowel move. */
static void test_positive_control_band_moved(double sr) {
  struct corpus c = vocal_corpus(sr);
  struct omx_deess p = atom(sr, 1000.0, 1.0, -30.0, 4.0, -24.0, OMX_DEESS_SPLIT);
  double dv, ds;
  run_two_tone(&p, &c, 64u, &dv, &ds);
  check(dv < -4.0, "positive control: the band on the VOWEL reduces the vowel");
  check(fabs(ds) < 0.5, "positive control: the sibilant is then the one left alone");
  check(dv < ds - 3.0, "positive control: the discrimination INVERTS with the band");
  check_near(dv, expected_change_db(&p, &c, c.fv), 1.0,
             "positive control: the vowel follows the closed form");
}

/* The FLOOR (§4c): a hot sibilant with a shallow range must stop exactly at the range, and not
 * where the ratio would have taken it. */
static void test_range_floors_the_reduction(double sr) {
  struct corpus c = vocal_corpus(sr);
  c.as = 0.5; /* a shouted "s" — the characteristic alone would ask for far more than 6 dB */
  struct omx_deess p = atom(sr, 7000.0, 1.0, -40.0, 8.0, -6.0, OMX_DEESS_WIDEBAND);
  double dv, ds;
  run_two_tone(&p, &c, 64u, &dv, &ds);
  (void)dv;
  check_near(ds, -6.0, 0.35, "range: the reduction stops at the declared floor");
  struct omx_deess deep = p;
  deep.dyn.gc.range_db = -24.0f;
  double dv2, ds2;
  run_two_tone(&deep, &c, 64u, &dv2, &ds2);
  check(ds2 < ds - 3.0, "range: a deeper floor DOES let the same signal go further down");
}

/* The gain computer itself, at four levels, against the arithmetic §4c states. A characteristic
 * checked only through the audio is a characteristic no reader can verify. */
static void test_gain_computer_closed_form(double sr) {
  struct omx_deess p = atom(sr, 7000.0, 1.0, -30.0, 4.0, -12.0, OMX_DEESS_SPLIT);
  /* thresh -30, ratio 4 (slope -0.75), knee 6 (half 3), floor -12. */
  check_near(omx_deess_gain_db(&p, (float)pow(10.0, -50.0 / 20.0)), 0.0, 1e-5,
             "gain: far below the knee, no reduction");
  check_near(omx_deess_gain_db(&p, (float)pow(10.0, -33.0 / 20.0)), 0.0, 1e-4,
             "gain: at the knee's lower edge, no reduction");
  /* at -30 (the threshold, knee centre): over = 0, e = 3 -> slope*e*e/(2*knee) = -0.75*9/12 */
  check_near(omx_deess_gain_db(&p, (float)pow(10.0, -30.0 / 20.0)), -0.5625, 2e-3,
             "gain: mid-knee is the parabola, not the slope");
  /* at -20: over = 10 > half -> slope * over */
  check_near(omx_deess_gain_db(&p, (float)pow(10.0, -20.0 / 20.0)), -7.5, 5e-3,
             "gain: above the knee is the ratio's slope");
  /* at -6: slope*24 = -18, floored at -12 */
  check_near(omx_deess_gain_db(&p, (float)pow(10.0, -6.0 / 20.0)), -12.0, 1e-5,
             "gain: the floor holds against a hot band");
}

/* ZERO LATENCY (§5), measured from an impulse rather than read off a declaration. */
static void test_zero_latency(double sr) {
  static float x[1024];
  struct omx_deess p = atom(sr, 7000.0, 1.0, -60.0, 4.0, -24.0, OMX_DEESS_SPLIT);
  struct omx_deess_state st;
  omx_deess_state_init(&st);
  memset(x, 0, sizeof x);
  x[100] = 1.0f;
  omx_deess_process(x, NULL, 1024u, &p, &st);
  int first = -1;
  for (int i = 0; i < 1024; i++)
    if (x[i] != 0.0f) {
      first = i;
      break;
    }
  check(first == 100, "latency: the first non-zero output sample is the input's own index");
  check(omx_deess_latency(&p) == 0, "latency: the stage declares zero, engaged");
  p.enabled = 0;
  check(omx_deess_latency(&p) == 0, "latency: and zero, disengaged");
}

/* The `no-gain-added` law, both modes: over a swept level the peak never grows. */
static void test_never_adds_gain(double sr) {
  static float x[4096];
  for (int k = 0; k < 8; k++) {
    double amp = 0.01 * pow(2.0, k);
    if (amp > 0.9) amp = 0.9;
    struct omx_deess p = atom(sr, 7000.0, 1.0, -45.0, 6.0, -24.0,
                              (k & 1) ? OMX_DEESS_WIDEBAND : OMX_DEESS_SPLIT);
    struct omx_deess_state st;
    omx_deess_state_init(&st);
    double peak_in = 0.0, peak_out = 0.0;
    for (uint32_t i = 0; i < 4096u; i++) {
      double t = (double)i;
      x[i] = (float)(amp * (sin(2.0 * M_PI * 7000.0 * t / sr) + 0.5 * sin(2.0 * M_PI * 700.0 * t / sr)));
      if (fabs((double)x[i]) > peak_in) peak_in = fabs((double)x[i]);
    }
    omx_deess_process(x, NULL, 4096u, &p, &st);
    for (uint32_t i = 0; i < 4096u; i++)
      if (fabs((double)x[i]) > peak_out) peak_out = fabs((double)x[i]);
    check(peak_out <= peak_in + 1e-6, "law no-gain-added: the stage never adds gain, either mode");
  }
}

/*
 * THE `no-gain-added` LAW IN SPLIT MODE, MEASURED AT EVERY FREQUENCY (spec §6; the operator's
 * ruling, §1 gate line 3). A 7 kHz sibilant hot enough to hold the stage at its -24 dB floor —
 * so g is a CONSTANT and the stage is, for the length of the measurement, a linear filter — and
 * a small probe tone swept over the audio band. The probe must never come out louder than it went
 * in, and must sit on the closed form |g + (1 - g) N(f)|. The sibilant itself must read the floor:
 * the band's centre is where the notch is null, so split at the floor takes the "s" down by
 * exactly g there.
 *
 * WHY THE FLOOR. With g moving, the detector follows the beat between the probe and the sibilant
 * and g(t) carries a component at their difference frequency, which lands back on the probe's own
 * bin through the 7 kHz carrier — a +0.1 dB reading that is intermodulation, not gain. At the
 * floor the characteristic asks for more than the range allows, the clamp wins, and g is a number.
 *
 * WHY THIS ARM IS THE ONE THAT MATTERS. The highpass·lowpass band this stage first shipped with
 * passes every other arm in this file and fails this one at +1.0 dB (2.5 kHz), +0.8 (3.5 kHz),
 * +0.9 (16 kHz), +0.3 (1 kHz) — the sabotage recorded in the header.
 */
static void test_split_never_exceeds_input(double sr) {
  struct omx_deess p = atom(sr, 7000.0, 1.0, -40.0, 8.0, -24.0, OMX_DEESS_SPLIT);
  double worst = -1e9, worst_hz = 0.0, worst_form_err = 0.0;
  int probes = 0;
  for (double hz = 30.0; hz < 0.45 * sr; hz *= 1.25) {
    struct corpus c;
    c.fv = snap(hz, sr);
    c.fs = snap(7000.0, sr);
    if (fabs(c.fv - c.fs) * (double)NB < 2.5) continue; /* the probe on the sibilant's own bin */
    c.av = 0.02;  /* the probe: small, so it cannot lift the detector off the floor */
    c.as = 0.5;   /* the "s": -6 dBFS, ~26 dB over a -40 threshold at ratio 8 -> past the floor */
    double dv, ds;
    run_two_tone(&p, &c, 64u, &dv, &ds);
    double want = expected_change_db(&p, &c, c.fv);
    if (dv > worst) { worst = dv; worst_hz = hz; }
    if (fabs(dv - want) > worst_form_err) worst_form_err = fabs(dv - want);
    check(dv <= 0.01, "split no-gain-added: the probe never comes out louder than it went in");
    check_near(dv, want, 0.02, "split no-gain-added: the probe sits on |g + (1 - g) N(f)|");
    check_near(ds, -24.0, 0.05, "split no-gain-added: the sibilant reads the floor throughout");
    probes++;
  }
  check(probes >= 20, "split no-gain-added: the sweep covered the band (positive control)");
  printf("  %6.0f Hz  split@floor  %d probes, worst %+6.3f dB at %.0f Hz, closed-form error %.4f dB\n",
         sr, probes, worst, worst_hz, worst_form_err);
}

static void test_finite_under_abuse(double sr) {
  static float x[2048];
  struct omx_deess p = atom(sr, 7000.0, 1.0, -30.0, 4.0, -24.0, OMX_DEESS_SPLIT);
  struct omx_deess_state st;
  omx_deess_state_init(&st);
  for (int phase = 0; phase < 3; phase++) {
    for (uint32_t i = 0; i < 2048u; i++) {
      if (phase == 0) x[i] = (i & 1u) ? 1.0f : -1.0f;      /* full scale, every sample */
      else if (phase == 1) x[i] = 0.0f;                     /* digital silence */
      else x[i] = (i & 1u) ? 1e-30f : -1e-30f;              /* subnormal territory */
    }
    omx_deess_process(x, NULL, 2048u, &p, &st);
    int finite = 1;
    for (uint32_t i = 0; i < 2048u; i++)
      if (!(x[i] - x[i] == 0.0f)) finite = 0;
    check(finite, "law finite: a finite block in gives a finite block out");
  }
  const float *w = (const float *)&st;
  int ok = 1;
  for (size_t i = 0; i < sizeof st / sizeof(float); i++)
    if (!(w[i] - w[i] == 0.0f)) ok = 0;
  check(ok, "law finite: and a finite state");
}

/* `no-denormal-state`: after a burst decays into silence, no state word sits in the subnormal
 * range where a multiply costs two orders of magnitude more (mix_dsp.h's omx_flush block). */
static void test_no_denormal_state(double sr) {
  static float x[4096];
  struct omx_deess p = atom(sr, 7000.0, 1.0, -40.0, 4.0, -24.0, OMX_DEESS_SPLIT);
  struct omx_deess_state st;
  omx_deess_state_init(&st);
  for (uint32_t i = 0; i < 4096u; i++) x[i] = (float)(0.7 * sin(2.0 * M_PI * 7000.0 * (double)i / sr));
  omx_deess_process(x, NULL, 4096u, &p, &st);
  for (int block = 0; block < 64; block++) { /* ~2.7 s at 96 k of pure silence */
    memset(x, 0, sizeof x);
    omx_deess_process(x, NULL, 4096u, &p, &st);
  }
  const float *w = (const float *)&st;
  int clean = 1;
  for (size_t i = 0; i < sizeof st / sizeof(float); i++) {
    float v = fabsf(w[i]);
    if (v != 0.0f && v < 1e-20f) clean = 0;
  }
  check(clean, "law no-denormal-state: every state word is zero or normal after a decay");
}

/* A stage correct at one block size and wrong at another is wrong on a console whose quantum
 * changed — the defect mix_drive.test.c's block-size arm caught for the drive. */
static void test_block_size_independence(double sr) {
  struct corpus c = vocal_corpus(sr);
  struct omx_deess p = atom(sr, 7000.0, 1.0, -30.0, 4.0, -24.0, OMX_DEESS_SPLIT);
  double dv16, ds16, dv64, ds64, dv257, ds257;
  run_two_tone(&p, &c, 16u, &dv16, &ds16);
  run_two_tone(&p, &c, 64u, &dv64, &ds64);
  run_two_tone(&p, &c, 257u, &dv257, &ds257);
  check_near(ds16, ds64, 1e-4, "block size: 16 and 64 give the same reduction");
  check_near(ds257, ds64, 1e-4, "block size: 257 and 64 give the same reduction");
  check_near(dv16, dv257, 1e-4, "block size: the vowel agrees too");
}

/* A stereo pair takes ONE gain: the image cannot shift under de-essing. */
static void test_stereo_takes_one_gain(double sr) {
  static float l[NB], r[NB], lm[NB];
  struct corpus c = vocal_corpus(sr);
  struct omx_deess p = atom(sr, 7000.0, 1.0, -30.0, 4.0, -24.0, OMX_DEESS_WIDEBAND);
  struct omx_deess_state st;
  omx_deess_state_init(&st);
  for (int pass = 0; pass < 2; pass++) {
    fill(l, NB, &c, (uint32_t)pass * NB);
    for (uint32_t i = 0; i < NB; i++) r[i] = 0.5f * l[i]; /* the same signal, 6 dB down on R */
    memcpy(lm, l, sizeof lm);
    for (uint32_t off = 0; off < NB; off += 64u) omx_deess_process(l + off, r + off, 64u, &p, &st);
  }
  double bl = db(bin_mag(l, NB, c.fs)), br = db(bin_mag(r, NB, c.fs));
  check_near(bl - br, 6.0206, 0.02, "stereo: the L/R balance is exactly what it was");
}

int main(void) {
  omx_fx_require_rate_floor();
  printf("fx/deesser: the band-limited detector, %d declared rates\n", NRATES);
  double split_at_rate[NRATES], deviation[NRATES];
  for (int k = 0; k < NRATES; k++) {
    double sr = OMX_DECLARED_RATES[k];
    test_bypass_is_bit_identical(sr);
    test_below_threshold_passes(sr);
    split_at_rate[k] = test_split_reduces_the_band_only(sr, &deviation[k]);
    test_wideband_reduces_both(sr);
    test_positive_control_band_moved(sr);
    test_range_floors_the_reduction(sr);
    test_gain_computer_closed_form(sr);
    test_zero_latency(sr);
    test_never_adds_gain(sr);
    test_split_never_exceeds_input(sr);
    test_finite_under_abuse(sr);
    test_no_denormal_state(sr);
    test_block_size_independence(sr);
    test_stereo_takes_one_gain(sr);
  }
  /*
   * THE FOUR-RATE LAW's real content: the same controls in milliseconds and hertz must produce the
   * same audio whatever the graph runs at. A detector whose time constants silently became SAMPLES
   * passes every arm above at one rate and fails here.
   *
   * The law is stated over the KERNEL's own deviation from its closed form, not over the raw
   * reduction, and the difference is measured rather than asserted away: the raw split reduction
   * moves 0.14 dB from 44.1 k to 192 k (−10.569 → −10.428), and that is the BILINEAR WARP of the
   * cookbook section's skirt at the VOWEL — the bandpass reads 0 dB at its own centre at every
   * rate, so the sibilant's share of the detector level is rate-free, and what moves is the
   * 1 kHz vowel's share through a Q the bandwidth relation corrects for the centre but not for a
   * point 2.8 octaves below it. The closed form tracks it (−11.259 → −11.170) because it is
   * evaluated from the very coefficients the kernel ran.
   *
   * What the kernel ADDS to the closed form — the detector's envelope cascade settling a few
   * percent under the two-tone PEAK the closed form assumes — is PINNED, not tolerated: +0.69 …
   * +0.74 dB, the same at every rate to 0.05 dB. A 1 dB tolerance on the split arm alone would
   * let a detector change of that size through; this arm is what sees it. (The number moved from
   * +0.34 … +0.36 under the highpass·lowpass band to +0.69 … +0.74 under the bandpass: the offset
   * scales with how far above the knee the detector sits, and the bandpass reads the sibilant
   * 1.9 dB hotter at the centre — which is the point of it.)
   */
  double lo = split_at_rate[0], hi = split_at_rate[0], dlo = deviation[0], dhi = deviation[0];
  for (int k = 1; k < NRATES; k++) {
    if (split_at_rate[k] < lo) lo = split_at_rate[k];
    if (split_at_rate[k] > hi) hi = split_at_rate[k];
    if (deviation[k] < dlo) dlo = deviation[k];
    if (deviation[k] > dhi) dhi = deviation[k];
  }
  printf("  rate spread: raw reduction %.3f dB (", hi - lo);
  for (int k = 0; k < NRATES; k++) printf("%s%.1f %.3f", k ? " | " : "", OMX_DECLARED_RATES[k] / 1000.0, split_at_rate[k]);
  printf(")\n  rate spread: kernel deviation from its closed form %.3f dB (", dhi - dlo);
  for (int k = 0; k < NRATES; k++) printf("%s%+.3f", k ? " | " : "", deviation[k]);
  printf(")\n");
  check(dhi - dlo <= 0.1, "every declared rate: the kernel's deviation from its closed form agrees within 0.1 dB");
  for (int k = 0; k < NRATES; k++)
    check_near(deviation[k], 0.715, 0.05,
               "every declared rate: the kernel's offset from its closed form is the pinned +0.69 ... +0.74 dB");
  check(hi - lo <= 1.0, "every declared rate: the raw reduction stays within 1 dB across them");

  printf("%s: %d checks, %d failures\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
