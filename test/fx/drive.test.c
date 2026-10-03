// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Oracle for the native DRIVE stage (omx_drive.h), every arm at every rate in OMX_DECLARED_RATES:
 *   make test-fx
 *
 * Spec: docs/design/specs/2026-09-14-native-drive-stage.md §4.
 *
 * WHAT THIS FILE IS FOR. A saturator's whole job is the harmonics it adds, so a test that only
 * proves "the numbers changed" proves nothing — it would pass over a stage that added noise.
 * Every claim here is a claim about a SPECTRUM, measured bin by bin with the fundamental on an
 * exact bin so a rectangular window leaks nothing, and on a SECOND steady-state pass so the
 * oversampler's start-up transient is not read as an alias floor. That mistake cost this lane an
 * evening: the first reading said -80 dBc, and so did a DOUBLE-PRECISION direct convolution,
 * while the filter table's own response at that frequency is -112.8 dB. When the reference and
 * the implementation agree and both disagree with the algebra, the MEASUREMENT is wrong.
 *
 * The controls a resolver would normally fill in are built here by `atom()`, with the biquad
 * coefficients designed in this file by the SAME RBJ cookbook `@freemixer/core`'s rbjSection
 * transcribes — so the shapes under test are the shapes the console pushes.
 *
 * SABOTAGE-VERIFIED (2026-09-14, this lane — an unverified guard is decoration):
 *   - the dry path's delay compensation removed  -> 32 checks red
 *   - `character` reduced to the odd residue only ->  3 checks red
 *   - the DC blocker switched off                ->  1 check  red
 * and restored green each time. Again 2026-09-15, for §4b's bounded bias:
 *   - `omx_drive_sample` put back to the SUPERSEDED residue blend -> 10 checks red, worst
 *     |y| 125.19 at the shaper and 86.2 through the stage; restored green.
 * Again 2026-09-27, for the Enhancer preset arms (9b):
 *   - the post-curve roll-off skipped            -> 24 arm A checks red, worst 18.7 dB
 *   - the HIGH band fed the full signal          ->  6 arm B checks red (-6.8 dB vs -32.4 bound)
 *   - the exciter's sum turned into a crossfade  ->  arm 9's topology check red (A/B are silent:
 *     they measure the band and the harmonics, and the topology is 9's claim)
 *
 * THREE REAL DEFECTS THIS FILE CAUGHT, recorded because each was invisible to a green run:
 *   1. The compensation delay ring was 128 long while 4x needs latency(66) + chunk(64) = 130, so
 *      the read for output 0 landed on a sample written three lines earlier in the same pass.
 *      Only the BLOCK-SIZE arm saw it: at one block size the stage was right and at another it
 *      was wrong.
 *   2. The auto-gain's one-pole coefficient is per SAMPLE but the detector updates once per
 *      CHUNK, so the declared 50 ms window was really 3.2 s and the control did nothing an
 *      operator could hear. The level-match arm measured 6.2 dB of uncorrected rise.
 *   3. Two assertions in the first draft were about the wrong mechanism (a THD ordering the
 *      knees do not have, and a stereo BALANCE the curve legitimately compresses). Both are
 *      replaced by tests of what the code actually decides, with the reasoning kept in place.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <omxdsp/fx/omx_drive.h>

#include "fx_rates.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0, g_checks = 0;
static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) { g_fail++; fprintf(stderr, "FAIL: %s\n", what); }
}
static void check_num(int cond, const char *what, double got, double want) {
  g_checks++;
  if (!cond) { g_fail++; fprintf(stderr, "FAIL: %s — got %.4f, want %.4f\n", what, got, want); }
}

/* The rate the fixed-rate arms run at: main() walks it over OMX_DECLARED_RATES. */
static double g_sr = 96000.0;
#define SR g_sr

/*
 * Two claims are measured at every rate and ASSERTED only at base rates of 88.2 kHz and above:
 * the 4x alias floor (-110 dBc) and the stage's reconstruction-ripple bound (1.15). The engine's
 * oracle stated both at the rate the rig runs (96 kHz) and the drive spec calls them claims at
 * that rate; at 44.1 and 48 kHz the 4x shaper measures -100.8 and -83.1 dBc and a stage peak of
 * 1.165 and 1.163, printed on every run and not asserted. The no-oversampling CONTROL is asserted
 * wherever a product of order 9 folds into the audio band; above 96 kHz none does, and the line
 * says so instead of passing.
 */
static int high_rate_claim(void) { return SR >= 88200.0; }
#define NB 16384u

/* ---- measurement ---------------------------------------------------------------------- */

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

/* RBJ low-pass, the cookbook @freemixer/core's rbjSection transcribes, normalised to a0. */
static void rbj_lowpass(float c[5], double f0, double q, double sr) {
  double w = 2.0 * M_PI * f0 / sr, cw = cos(w), sw = sin(w), alpha = sw / (2.0 * q);
  double b0 = (1.0 - cw) / 2.0, b1 = 1.0 - cw, b2 = b0;
  double a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
  c[0] = (float)(b0 / a0); c[1] = (float)(b1 / a0); c[2] = (float)(b2 / a0);
  c[3] = (float)(a1 / a0); c[4] = (float)(a2 / a0);
}
/* A high shelf and its EXACT inverse (numerator and denominator swapped) — the tilt pair. */
static void rbj_highshelf(float c[5], double f0, double gain_db, double sr, int invert) {
  double A = pow(10.0, gain_db / 40.0);
  double w = 2.0 * M_PI * f0 / sr, cw = cos(w), sw = sin(w);
  double S = 1.0, alpha = sw / 2.0 * sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
  double ta = 2.0 * sqrt(A) * alpha;
  double b0 = A * ((A + 1.0) + (A - 1.0) * cw + ta);
  double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
  double b2 = A * ((A + 1.0) + (A - 1.0) * cw - ta);
  double a0 = (A + 1.0) - (A - 1.0) * cw + ta;
  double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
  double a2 = (A + 1.0) - (A - 1.0) * cw - ta;
  if (invert) {
    double t;
    t = b0; b0 = a0; a0 = t;
    t = b1; b1 = a1; a1 = t;
    t = b2; b2 = a2; a2 = t;
  }
  c[0] = (float)(b0 / a0); c[1] = (float)(b1 / a0); c[2] = (float)(b2 / a0);
  c[3] = (float)(a1 / a0); c[4] = (float)(a2 / a0);
}

/* A control atom at the console's declared defaults, with the two overrides a test cares about. */
static struct omx_drive atom_at(int curve, double drive_db, double character, double mix, int factor,
                                double sr) {
  struct omx_drive p;
  memset(&p, 0, sizeof(p));
  p.enabled = 1;
  p.curve = curve;
  p.band = OMX_DRIVE_BAND_FULL;
  p.drive_lin = (float)pow(10.0, drive_db / 20.0);
  p.even_w = (float)((character + 1.0) / 2.0);
  p.mix = (float)mix;
  p.trim_lin = 1.0f;
  p.auto_gain = 0;
  p.stereo_link = 1;
  p.hf_on = 0;
  p.os_factor = factor;
  rbj_lowpass(p.band_c, 2000.0, 0.7071, sr);
  rbj_highshelf(p.tilt_c, 2000.0, 12.0, sr, 0);
  rbj_highshelf(p.tilt_inv_c, 2000.0, 12.0, sr, 1);
  rbj_lowpass(p.hf_c, 12000.0, 0.7071, sr);
  p.dc_coeff = (float)(1.0 - 2.0 * M_PI * 5.0 / sr);
  p.ag_coeff = (float)exp(-1.0 / (0.05 * sr));
  return p;
}
static struct omx_drive atom(int curve, double drive_db, double character, double mix, int factor) {
  return atom_at(curve, drive_db, character, mix, factor, SR);
}

static struct omx_drive_state *make_state(int factor) {
  struct omx_drive_state *s = (struct omx_drive_state *)calloc(1, sizeof(*s));
  omx_drive_state_init(s, factor);
  return s;
}

/* Run a periodic block through the stage TWICE and analyse the second — steady state, no
 * start-up transient, so the rectangular-window analyser leaks nothing. */
static void run_steady(const struct omx_drive *p, struct omx_drive_state *s, const float *in,
                       uint32_t n, float *out) {
  for (int pass = 0; pass < 2; pass++) {
    memcpy(out, in, n * sizeof(float));
    omx_drive_process(out, NULL, n, p, s);
  }
}

/* A sine on an exact bin. Returns the bin. */
static int tone(float *x, uint32_t n, double hz, double amp) {
  int k = (int)(hz * (double)n / SR + 0.5);
  for (uint32_t i = 0; i < n; i++) x[i] = (float)(amp * sin(2.0 * M_PI * (double)k * (double)i / (double)n));
  return k;
}

/* ---- 0. the positive control ---------------------------------------------------------- */

/*
 * THE HARMONIC ANALYSER MUST SEE HARMONICS. A tone through a curve at a real drive setting has
 * to show a 3rd harmonic well above the floor, and the same tone through a DISABLED stage has to
 * show none. Without both halves, "character -1 has no even harmonics" and "the analyser is
 * looking at the wrong bin" are the same output.
 */
static void test_control_analyser(void) {
  static float x[NB], y[NB];
  int k = tone(x, NB, 1000.0, 0.5);
  struct omx_drive p = atom(OMX_DRIVE_SOFT, 18.0, -1.0, 1.0, 1);
  struct omx_drive_state *s = make_state(1);
  run_steady(&p, s, x, NB, y);
  double f = bin_mag(y, NB, (double)k / NB);
  double h3 = bin_mag(y, NB, (double)(3 * k) / NB);
  check_num(db(h3 / f) > -40.0, "CONTROL: the analyser SEES the 3rd harmonic a curve makes",
            db(h3 / f), -40.0);

  struct omx_drive off = p;
  off.enabled = 0;
  struct omx_drive_state *s2 = make_state(1);
  run_steady(&off, s2, x, NB, y);
  double h3off = bin_mag(y, NB, (double)(3 * k) / NB);
  check_num(db(h3off / bin_mag(y, NB, (double)k / NB)) < -140.0,
            "CONTROL: and finds NO 3rd harmonic through a disabled stage",
            db(h3off / bin_mag(y, NB, (double)k / NB)), -140.0);
  free(s);
  free(s2);
}

/* ---- 1. zero cost when disengaged ----------------------------------------------------- */

static void test_bypass_is_untouched(void) {
  static float x[512], l[512], r[512], l0[512], r0[512];
  for (int i = 0; i < 512; i++) x[i] = 0.6f * (float)sin(0.11 * i) + 0.3f * (float)sin(1.3 * i);
  memcpy(l, x, sizeof(x));
  memcpy(r, x, sizeof(x));
  memcpy(l0, x, sizeof(x));
  memcpy(r0, x, sizeof(x));
  struct omx_drive p = atom(OMX_DRIVE_TUBE, 24.0, 0.5, 1.0, 4);
  p.enabled = 0;
  struct omx_drive_state *s = make_state(4);
  /* A byte-for-byte copy of the state, to prove not one state WORD moved either. */
  struct omx_drive_state *before = (struct omx_drive_state *)malloc(sizeof(*before));
  memcpy(before, s, sizeof(*s));
  omx_drive_process(l, r, 512, &p, s);
  check(memcmp(l, l0, sizeof(l)) == 0, "a disabled stage leaves the L leg BIT-IDENTICAL");
  check(memcmp(r, r0, sizeof(r)) == 0, "a disabled stage leaves the R leg BIT-IDENTICAL");
  check(memcmp(before, s, sizeof(*s)) == 0,
        "a disabled stage does not advance ONE state word — no history, no detector, no ring");
  /* And a NULL state is a passthrough rather than a crash. */
  p.enabled = 1;
  omx_drive_process(l, r, 512, &p, NULL);
  check(memcmp(l, l0, sizeof(l)) == 0, "a NULL state is a passthrough, not a fault");
  free(before);
  free(s);
}

/* ---- 2. mix 0 is bit-identical dry, at EVERY band and EVERY factor --------------------- */

static void test_mix_zero_is_dry(void) {
  static float x[1024], y[1024];
  for (int i = 0; i < 1024; i++) x[i] = 0.7f * (float)sin(0.07 * i) + 0.2f * (float)sin(2.1 * i);
  const int bands[4] = { OMX_DRIVE_BAND_FULL, OMX_DRIVE_BAND_LOW, OMX_DRIVE_BAND_HIGH,
                         OMX_DRIVE_BAND_TILT };
  const int curves[4] = { OMX_DRIVE_SOFT, OMX_DRIVE_TAPE, OMX_DRIVE_TUBE, OMX_DRIVE_EXCITER };
  for (int f = 1; f <= 4; f *= 2) {
    for (int b = 0; b < 4; b++) {
      for (int c = 0; c < 4; c++) {
        struct omx_drive p = atom(curves[c], 30.0, 0.7, 0.0, f);
        p.band = bands[b];
        p.auto_gain = 1; /* the auto-gain must not spoil the identity either */
        struct omx_drive_state *s = make_state(f);
        memcpy(y, x, sizeof(x));
        omx_drive_process(y, NULL, 1024, &p, s);
        /* "dry" means the stage's own input, DELAYED by the latency it declares — an engaged
         * stage costs its latency whatever the mix is, and `latencySamples` says so on the row.
         * Comparing against the UNdelayed input would be asking the stage to be two things. */
        uint32_t lat = (uint32_t)omx_drive_latency(&p);
        int same = 1;
        for (uint32_t i = lat; i < 1024u; i++) if (y[i] != x[i - lat]) same = 0;
        check(same, "mix 0 is BIT-IDENTICAL delayed dry — every band, every curve, every factor");
        free(s);
      }
    }
  }
}

/* ---- 3. the three knees, and their ordering ------------------------------------------- */

static void test_curve_shapes(void) {
  /* The six numbers the header's table states, asserted from the shipped code. */
  check_num(fabs(omx_drive_shape(OMX_DRIVE_SOFT, 1.0f) - 0.76159f) < 1e-4, "SOFT F(1) = 0.7616",
            omx_drive_shape(OMX_DRIVE_SOFT, 1.0f), 0.76159);
  check_num(fabs(omx_drive_shape(OMX_DRIVE_TAPE, 1.0f) - 0.70711f) < 1e-4, "TAPE F(1) = 0.7071",
            omx_drive_shape(OMX_DRIVE_TAPE, 1.0f), 0.70711);
  check_num(fabs(omx_drive_shape(OMX_DRIVE_TUBE, 1.0f) - 0.50000f) < 1e-4, "TUBE F(1) = 0.5000",
            omx_drive_shape(OMX_DRIVE_TUBE, 1.0f), 0.5);
  check_num(fabs(omx_drive_shape(OMX_DRIVE_SOFT, 3.0f) - 0.99505f) < 1e-4, "SOFT F(3) = 0.9951",
            omx_drive_shape(OMX_DRIVE_SOFT, 3.0f), 0.99505);
  check_num(fabs(omx_drive_shape(OMX_DRIVE_TAPE, 3.0f) - 0.94868f) < 1e-4, "TAPE F(3) = 0.9487",
            omx_drive_shape(OMX_DRIVE_TAPE, 3.0f), 0.94868);
  check_num(fabs(omx_drive_shape(OMX_DRIVE_TUBE, 3.0f) - 0.75000f) < 1e-4, "TUBE F(3) = 0.7500",
            omx_drive_shape(OMX_DRIVE_TUBE, 3.0f), 0.75);
  /* Unity small-signal gain: every curve passes a quiet signal at 1.0, so DRIVE adds harmonics
   * and not level. */
  for (int c = 0; c <= 2; c++) {
    float e = omx_drive_shape(c, 1e-4f) / 1e-4f;
    check_num(fabsf(e - 1.0f) < 1e-3f, "every knee has unity small-signal gain", e, 1.0);
  }
  /* The EXCITER member runs the SOFT shape — it is a topology, not a fourth knee. */
  check(omx_drive_shape(OMX_DRIVE_EXCITER, 0.7f) == omx_drive_shape(OMX_DRIVE_SOFT, 0.7f),
        "EXCITER is a TOPOLOGY over the SOFT shape, not a fourth curve");
}

/* ---- 4. THD per curve at a known drive ------------------------------------------------ */

/* Total harmonic distortion of `y` relative to its fundamental at bin `k`, harmonics 2..20. */
static double thd_dbc(const float *y, uint32_t n, int k) {
  double f = bin_mag(y, n, (double)k / n), sum = 0.0;
  for (int h = 2; h <= 20; h++) {
    if (h * k >= (int)n / 2) break;
    double m = bin_mag(y, n, (double)(h * k) / n);
    sum += m * m;
  }
  return db(sqrt(sum) / f);
}

static void test_thd_per_curve(void) {
  static float x[NB], y[NB];
  int k = tone(x, NB, 1000.0, 0.5);
  const int curves[3] = { OMX_DRIVE_SOFT, OMX_DRIVE_TAPE, OMX_DRIVE_TUBE };
  const char *names[3] = { "soft", "tape", "tube" };
  double thd[3];
  for (int c = 0; c < 3; c++) {
    struct omx_drive p = atom(curves[c], 12.0, -1.0, 1.0, 4);
    struct omx_drive_state *s = make_state(4);
    run_steady(&p, s, x, NB, y);
    thd[c] = thd_dbc(y, NB, k);
    printf("  THD: %s at +12 dB drive, 1 kHz @ -6 dBFS -> %.2f dBc\n", names[c], thd[c]);
    free(s);
  }
  for (int c = 0; c < 3; c++) {
    check_num(thd[c] > -40.0 && thd[c] < 0.0, "each curve distorts at +12 dB drive", thd[c], -40.0);
  }
  /*
   * THD ALONE DOES NOT ORDER THE KNEES, and the first version of this test asserted that it did.
   * It is measured against the FUNDAMENTAL, and a gentler knee compresses the fundamental too —
   * so the three came out within 0.5 dB of each other in no useful order. What actually
   * distinguishes the shapes is the harmonic ROLL-OFF: a firmer knee puts more energy in the
   * high orders. H5/H3 is that, in one number.
   */
  double roll[3];
  for (int c = 0; c < 3; c++) {
    struct omx_drive p = atom(curves[c], 12.0, -1.0, 1.0, 4);
    struct omx_drive_state *st = make_state(4);
    run_steady(&p, st, x, NB, y);
    double h3 = bin_mag(y, NB, (double)(3 * k) / NB), h5 = bin_mag(y, NB, (double)(5 * k) / NB);
    roll[c] = db(h5 / h3);
    printf("  roll-off: %s -> H5/H3 %.2f dB\n", names[c], roll[c]);
    free(st);
  }
  /* MEASURED, and the opposite of the first guess: tanh is analytic, so its harmonics die away
   * fastest; u/(1+|u|) carries an |u| whose higher derivatives are discontinuous, so TUBE spreads
   * energy furthest up the series. Firmer knee, faster roll-off. */
  check_num(roll[2] > roll[1], "TUBE (the gentlest knee) rolls off most SLOWLY", roll[2], roll[1]);
  check_num(roll[1] > roll[0], "TAPE rolls off more slowly than SOFT (the firmest knee)", roll[1],
            roll[0]);

  /* Drive is monotone: more drive, more harmonics. */
  double prev = -999.0;
  for (double d = 0.0; d <= 24.0; d += 6.0) {
    struct omx_drive p = atom(OMX_DRIVE_SOFT, d, -1.0, 1.0, 4);
    struct omx_drive_state *s = make_state(4);
    run_steady(&p, s, x, NB, y);
    double t = thd_dbc(y, NB, k);
    check_num(t > prev, "THD rises monotonically with DRIVE", t, prev);
    prev = t;
    free(s);
  }
}

/* ---- 5. character: -1 is pure ODD, +1 is EVEN-DOMINANT, and BOTH ends are bounded ------ */

/*
 * Spec §4b as amended 2026-09-15: `character` is a BIAS on the shaper's input,
 * out = (F(u + b) - F(b)) / (1 + F(b)) with b = w/sqrt(2). What that changes here:
 *
 *   - `character -1` is unchanged and still EXACT: b = 0 makes the shaper odd, so H2 sits at the
 *     arithmetic floor. The old assertion stands, unweakened.
 *   - `character +1` is EVEN-DOMINANT, not pure even. The old "H3 at the floor" assertion was a
 *     claim about a formula that has been withdrawn for being unbounded; what replaces it is a
 *     MEASURED lead of H2 over H3, at the drive where the knob is a colour control.
 *
 * The reference is 12 dB of drive (the previous 18 dB sat past the crossover measured below) on
 * a 1 kHz tone at -6 dBFS, x4, all three knees.
 */
static void test_character_even_odd(void) {
  static float x[NB], y[NB];
  int k = tone(x, NB, 1000.0, 0.5);
  const int curves[3] = { OMX_DRIVE_SOFT, OMX_DRIVE_TAPE, OMX_DRIVE_TUBE };
  const char *names[3] = { "soft", "tape", "tube" };
  const double chs[3] = { -1.0, 0.0, 1.0 };
  double h2[3][3], h3[3][3];

  printf("  character at +12 dB drive, 1 kHz @ -6 dBFS, x4 — the spec §4b table:\n");
  for (int c = 0; c < 3; c++) {
    for (int i = 0; i < 3; i++) {
      struct omx_drive p = atom(curves[c], 12.0, chs[i], 1.0, 4);
      struct omx_drive_state *s = make_state(4);
      run_steady(&p, s, x, NB, y);
      double f = bin_mag(y, NB, (double)k / NB);
      h2[c][i] = db(bin_mag(y, NB, (double)(2 * k) / NB) / f);
      h3[c][i] = db(bin_mag(y, NB, (double)(3 * k) / NB) / f);
      free(s);
    }
    printf("    %-5s  ch -1: H2 %8.1f H3 %6.1f | ch 0: H2 %6.1f H3 %6.1f | ch +1: H2 %6.1f H3 %6.1f\n",
           names[c], h2[c][0], h3[c][0], h2[c][1], h3[c][1], h2[c][2], h3[c][2]);
  }

  for (int c = 0; c < 3; c++) {
    /* b = 0 at character -1, so the shaper is the bare odd curve and this end is EXACT. */
    check_num(h2[c][0] < -120.0, "character -1: the 2nd harmonic is at the floor — PURE ODD",
              h2[c][0], -120.0);
    check_num(h3[c][0] > -40.0, "character -1: the 3rd harmonic is there", h3[c][0], -40.0);
    /* The relaxed claim, and the whole of it: EVEN-DOMINANT, with the lead measured. */
    check_num(h2[c][2] > -40.0, "character +1: the 2nd harmonic is there", h2[c][2], -40.0);
    check_num(h2[c][2] - h3[c][2] > 6.0,
              "character +1: H2 LEADS H3 by more than 6 dB — even-dominant",
              h2[c][2] - h3[c][2], 6.0);
    /* The knob has to MOVE the even content, monotonically, or it is a decoration. */
    check_num(h2[c][1] > h2[c][0] + 60.0 && h2[c][2] > h2[c][1],
              "H2 rises monotonically from character -1 to 0 to +1", h2[c][2], h2[c][1]);
    check_num(h3[c][1] > -60.0, "character 0: BOTH are present", h3[c][1], -60.0);
  }

  /*
   * WHERE EVEN-DOMINANCE ENDS, measured rather than asserted away. A hard-clipped wave's EVEN
   * harmonics come from a duty-cycle asymmetry, and a FIXED bias is a shrinking fraction of a
   * growing swing — so past the knee the clipping's odd harmonics take over whatever the bias
   * is. The spec says the crossover is near 15 dB; this prints the sweep that says so, and the
   * two checks pin the ends. (The alternative, a bias that grows with the drive, keeps evens at
   * every drive and GATES any input below -3 dBFS — measured, and refused in §4b.)
   */
  printf("  character +1, soft: H2 - H3 vs drive —");
  double lead_lo = 0.0, lead_hi = 0.0;
  for (double d = 0.0; d <= 24.001; d += 6.0) {
    struct omx_drive p = atom(OMX_DRIVE_SOFT, d, 1.0, 1.0, 4);
    struct omx_drive_state *s = make_state(4);
    run_steady(&p, s, x, NB, y);
    double lead = db(bin_mag(y, NB, (double)(2 * k) / NB)) - db(bin_mag(y, NB, (double)(3 * k) / NB));
    printf("  %.0f dB: %+.1f", d, lead);
    if (d == 12.0) lead_lo = lead;
    if (d == 24.0) lead_hi = lead;
    free(s);
  }
  printf("\n");
  check_num(lead_lo > 6.0, "even-dominance HOLDS at 12 dB of drive", lead_lo, 6.0);
  check_num(lead_hi < lead_lo, "and falls away past the knee, as §4b says it must", lead_hi,
            lead_lo);

  /* The biased path carries DC by construction; the blocker is what keeps it off the bus. */
  struct omx_drive p = atom(OMX_DRIVE_SOFT, 18.0, 1.0, 1.0, 4);
  struct omx_drive_state *s = make_state(4);
  /* The blocker's corner is 5 Hz — a 32 ms time constant — so it is given a second of signal
   * before its residue is read. */
  for (int pass = 0; pass < 6; pass++) { memcpy(y, x, sizeof(x)); omx_drive_process(y, NULL, NB, &p, s); }
  /* Over the WHOLE block, because every harmonic completes a whole number of cycles in NB and
   * in no shorter window — a mean taken over the last quarter is not the signal's DC, it is a
   * fraction of a cycle of the 2nd harmonic, and it reads as an offset that is not there. */
  double mean = 0.0;
  for (uint32_t i = 0; i < NB; i++) mean += (double)y[i];
  mean /= (double)NB;
  printf("  DC: an asymmetric curve leaves a residual offset of %.2e after settling\n", mean);
  check_num(fabs(mean) < 1e-4, "the DC blocker keeps an asymmetric curve's offset off the bus",
            mean, 0.0);
  free(s);
}

/* ---- 5b. THE BOUND: no setting of any knob can put a sample outside full scale --------- */

/*
 * THE TEST THE OLD FORMULA COULD NOT PASS, and the reason §4b was replaced.
 *
 * The residue blend this stage ran until 2026-09-15 rearranged, for u < 0, to
 * y = u + (|u| - F(|u|))*(1 - 2w) — slope -2 and NO FLOOR at w = 1. A full-scale sample at 36 dB
 * of drive computed to about -125 linear, and §4d's auto-gain clamps at ±24 dB, so nothing
 * downstream could have brought it back. The bias form is bounded BY CONSTRUCTION: F has
 * ceiling 1, so F(u+b) - F(b) is inside [-1 - F(b), 1 - F(b)] whose widest magnitude is the
 * divisor 1 + F(b).
 *
 * SABOTAGE-VERIFIED 2026-09-15: with `omx_drive_sample` put back to the old residue blend, this
 * arm reports a worst |y| of 125.19 on all four curve members and 43.6 / 86.2 through the stage
 * at character 0 / +1 — 10 checks red. Restored: 1.000000, 0.9999, 0.9888 and green.
 *
 * The sweep is over the WHOLE declared travel — every curve member, character end to end, drive
 * 0 to its 36 dB maximum, and a full-scale input — because a bound that only holds at the
 * settings the author tried is not a bound.
 */
static void test_character_is_bounded(void) {
  const int curves[4] = { OMX_DRIVE_SOFT, OMX_DRIVE_TAPE, OMX_DRIVE_TUBE, OMX_DRIVE_EXCITER };
  const char *names[4] = { "soft", "tape", "tube", "exciter" };
  for (int c = 0; c < 4; c++) {
    double worst = 0.0;
    double worst_ch = 0.0, worst_d = 0.0;
    for (double ch = -1.0; ch <= 1.0001; ch += 0.05) {
      float w = (float)((ch + 1.0) / 2.0);
      struct omx_drive_bias bias = omx_drive_bias_for(curves[c], w);
      for (double d = 0.0; d <= 36.0001; d += 1.0) {
        float drive = (float)pow(10.0, d / 20.0);
        for (double xv = -1.0; xv <= 1.0001; xv += 0.001) {
          float y = omx_drive_sample(curves[c], drive * (float)xv, &bias);
          if (fabsf(y) > worst) { worst = fabsf(y); worst_ch = ch; worst_d = d; }
        }
      }
    }
    printf("  bound: %-7s worst |y| = %.6f (character %+.2f, drive %.0f dB)\n", names[c], worst,
           worst_ch, worst_d);
    check_num(worst <= 1.0, "the shaper never leaves full scale, at ANY curve/character/drive",
              worst, 1.0);
  }

  /*
   * And the same claim through the WHOLE stage, on the real signal path — the oversampler's
   * reconstruction filter is an FIR and rings on a hard-clipped edge, so the stage's peak is
   * allowed a little more than the shaper's and the margin is MEASURED, not assumed.
   *
   * The exciter is excluded from this arm and not from the one above: its topology ADDS the
   * driven band on top of the full dry signal (§4c), so its declared ceiling is 1 + mix and a
   * 1.0 assertion would be testing the wrong law.
   */
  static float x[NB], y[NB];
  tone(x, NB, 1000.0, 1.0);
  const int cross[3] = { OMX_DRIVE_SOFT, OMX_DRIVE_TAPE, OMX_DRIVE_TUBE };
  for (int c = 0; c < 3; c++) {
    for (double ch = -1.0; ch <= 1.0001; ch += 1.0) {
      struct omx_drive p = atom(cross[c], 36.0, ch, 1.0, 4);
      struct omx_drive_state *s = make_state(4);
      run_steady(&p, s, x, NB, y);
      double pk = 0.0;
      for (uint32_t i = 0; i < NB; i++) if (fabs((double)y[i]) > pk) pk = fabs((double)y[i]);
      printf("  bound: %-7s stage peak at 36 dB drive, character %+.0f, full scale = %.4f\n",
             names[c], ch, pk);
      if (high_rate_claim())
        check_num(pk < 1.15, "the STAGE stays inside full scale plus the reconstruction ripple",
                  pk, 1.15);
      else
        printf("  bound: NOT ASSERTED at %.0f Hz (the 1.15 claim is stated at 88.2 kHz and above)\n", SR);
      free(s);
    }
  }
}

/* ---- 6. the alias floor: the reason this stage is native ------------------------------ */

static void test_alias_floor(void) {
  static float x[NB], y[NB];
  /* The measurement note's own two-tone, at the rate the rig runs: 1 kHz + 15 kHz, 0.35 each. */
  int k1 = (int)(1000.0 * NB / SR + 0.5), k2 = (int)(15000.0 * NB / SR + 0.5);
  for (uint32_t i = 0; i < NB; i++) {
    x[i] = (float)(0.35 * sin(2.0 * M_PI * k1 * (double)i / NB) +
                   0.35 * sin(2.0 * M_PI * k2 * (double)i / NB));
  }
  printf("  two-tone: %.0f Hz + %.0f Hz at 0.35 each, base rate %.0f kHz\n",
         (double)k1 * SR / NB, (double)k2 * SR / NB, SR / 1000.0);

  double worst[3];
  int fi = 0;
  for (int f = 1; f <= 4; f *= 2, fi++) {
    struct omx_drive p = atom(OMX_DRIVE_SOFT, 18.0, -1.0, 1.0, f);
    struct omx_drive_state *s = make_state(f);
    run_steady(&p, s, x, NB, y);
    double fund = bin_mag(y, NB, (double)k2 / NB);
    /* Enumerate the intermodulation products to order 9 as INTEGER bins, classify each as WANTED
     * (its TRUE frequency is below Nyquist) or ALIAS (it folded), and report the worst ALIAS
     * inside 20 kHz. A peak-picker is not used: every product's position is arithmetic. */
    double w = -999.0;
    double at = 0.0;
    for (int m = -9; m <= 9; m++) {
      for (int nn = -9; nn <= 9; nn++) {
        if (m == 0 && nn == 0) continue;
        if (abs(m) + abs(nn) > 9) continue;
        long true_k = (long)m * k1 + (long)nn * k2;
        if (true_k <= 0) continue;
        if (true_k < (long)NB / 2) continue; /* below Nyquist: a WANTED product, not an alias */
        /* Fold it into the base band the way the rate does. */
        long fold = true_k % (long)NB;
        if (fold > (long)NB / 2) fold = (long)NB - fold;
        if (fold <= 0) continue;
        if ((double)fold * SR / NB > 20000.0) continue; /* outside the audio band */
        double mag = db(bin_mag(y, NB, (double)fold / NB) / fund);
        if (mag > w) { w = mag; at = (double)fold * SR / NB; }
      }
    }
    worst[fi] = w;
    printf("  alias: factor %d -> worst folded product below 20 kHz is %.1f dBc (at %.0f Hz)\n", f,
           w, at);
    free(s);
  }
  if (worst[0] > -999.0)
    check_num(worst[0] > -90.0, "CONTROL: without oversampling the stage DOES alias in band",
              worst[0], -90.0);
  else
    printf("  alias: CONTROL NOT APPLICABLE at %.0f Hz: no product of order 9 folds below 20 kHz\n", SR);
  if (high_rate_claim())
    check_num(worst[2] < -110.0, "at 4x the worst in-band aliased product is below -110 dBc",
              worst[2], -110.0);
  else
    printf("  alias: NOT ASSERTED at %.0f Hz (the -110 dBc claim is stated at 88.2 kHz and above)\n", SR);
  printf("  alias: 4x buys %.1f dB in band over no oversampling\n", worst[0] - worst[2]);
}

/* ---- 7. the band split reconstructs exactly, and acts where it says ------------------- */

static void test_band(void) {
  static float x[NB], y[NB];
  int klo = (int)(100.0 * NB / SR + 0.5), khi = (int)(8000.0 * NB / SR + 0.5);
  for (uint32_t i = 0; i < NB; i++) {
    x[i] = (float)(0.45 * sin(2.0 * M_PI * klo * (double)i / NB) +
                   0.45 * sin(2.0 * M_PI * khi * (double)i / NB));
  }
  /* band LOW at 2 kHz: the 100 Hz tone is driven, the 8 kHz one is not. */
  struct omx_drive p = atom(OMX_DRIVE_SOFT, 24.0, -1.0, 1.0, 4);
  p.band = OMX_DRIVE_BAND_LOW;
  struct omx_drive_state *s = make_state(4);
  run_steady(&p, s, x, NB, y);
  double h3lo = db(bin_mag(y, NB, (double)(3 * klo) / NB) / bin_mag(y, NB, (double)klo / NB));
  double h3hi = db(bin_mag(y, NB, (double)(3 * khi) / NB) / bin_mag(y, NB, (double)khi / NB));
  printf("  band LOW: 100 Hz H3 %.1f dBc, 8 kHz H3 %.1f dBc\n", h3lo, h3hi);
  check_num(h3lo > -30.0, "band LOW drives the low tone", h3lo, -30.0);
  check_num(h3hi < h3lo - 20.0, "band LOW leaves the high tone far less driven", h3hi, h3lo - 20.0);
  free(s);

  /* band HIGH is the complement: the same split, the other half. */
  struct omx_drive q = atom(OMX_DRIVE_SOFT, 24.0, -1.0, 1.0, 4);
  q.band = OMX_DRIVE_BAND_HIGH;
  struct omx_drive_state *s2 = make_state(4);
  run_steady(&q, s2, x, NB, y);
  double g3lo = db(bin_mag(y, NB, (double)(3 * klo) / NB) / bin_mag(y, NB, (double)klo / NB));
  double g3hi = db(bin_mag(y, NB, (double)(3 * khi) / NB) / bin_mag(y, NB, (double)khi / NB));
  printf("  band HIGH: 100 Hz H3 %.1f dBc, 8 kHz H3 %.1f dBc\n", g3lo, g3hi);
  check_num(g3hi > g3lo + 20.0, "band HIGH drives the high tone and spares the low one", g3hi,
            g3lo + 20.0);
  free(s2);
}

/* ---- 8. auto-gain: MEASURED, and held on silence -------------------------------------- */

static void test_auto_gain(void) {
  static float x[NB], on[NB], off[NB];
  tone(x, NB, 1000.0, 0.5);
  struct omx_drive p = atom(OMX_DRIVE_TUBE, 24.0, -1.0, 1.0, 4);
  p.auto_gain = 0;
  struct omx_drive_state *s = make_state(4);
  /* Four passes: the auto-gain's window is ~50 ms and a block is ~170 ms at this length. */
  for (int i = 0; i < 4; i++) { memcpy(off, x, sizeof(x)); omx_drive_process(off, NULL, NB, &p, s); }
  free(s);
  p.auto_gain = 1;
  s = make_state(4);
  for (int i = 0; i < 4; i++) { memcpy(on, x, sizeof(x)); omx_drive_process(on, NULL, NB, &p, s); }

  double rms_in = 0.0, rms_off = 0.0, rms_on = 0.0;
  for (uint32_t i = 0; i < NB; i++) {
    rms_in += (double)x[i] * x[i];
    rms_off += (double)off[i] * off[i];
    rms_on += (double)on[i] * on[i];
  }
  rms_in = sqrt(rms_in / NB);
  rms_off = sqrt(rms_off / NB);
  rms_on = sqrt(rms_on / NB);
  printf("  auto-gain: input %.2f dBFS, off %.2f dBFS, on %.2f dBFS\n", db(rms_in), db(rms_off),
         db(rms_on));
  check_num(fabs(db(rms_off / rms_in)) > 1.0, "CONTROL: without auto-gain the level DOES move",
            db(rms_off / rms_in), 1.0);
  check_num(fabs(db(rms_on / rms_in)) < 0.5, "auto-gain matches the level to within 0.5 dB",
            db(rms_on / rms_in), 0.0);
  free(s);

  /*
   * AND AT THE BIASED END. §4b's bias costs small-signal gain — the slope at the operating point
   * is F'(b)/(1 + F(b)), which is 0.391 (soft), 0.345 (tape), 0.243 (tube) at character +1
   * (-8.2, -9.2 and -12.3 dB) — so turning CHARACTER up with auto-gain OFF drops the level. That is what the measured make-up
   * is for, and it is on by default; this arm proves it covers the biased end too, which the
   * previous version of this test (character -1 only) could not have seen.
   */
  for (int c = 0; c <= 2; c++) {
    /* driveDb 0, so the bias's slope loss is what the level reading is ABOUT. At 24 dB the drive
     * gain dominates and the stage comes out LOUDER (+4.5 dB measured) — a control that asserted
     * a drop there would have been asserting the wrong mechanism's sign. */
    struct omx_drive q = atom(c, 0.0, 1.0, 1.0, 4);
    q.auto_gain = 0;
    struct omx_drive_state *sq = make_state(4);
    for (int i = 0; i < 4; i++) { memcpy(off, x, sizeof(x)); omx_drive_process(off, NULL, NB, &q, sq); }
    free(sq);
    q.auto_gain = 1;
    sq = make_state(4);
    for (int i = 0; i < 4; i++) { memcpy(on, x, sizeof(x)); omx_drive_process(on, NULL, NB, &q, sq); }
    double r_off = 0.0, r_on = 0.0;
    for (uint32_t i = 0; i < NB; i++) { r_off += (double)off[i] * off[i]; r_on += (double)on[i] * on[i]; }
    r_off = sqrt(r_off / NB);
    r_on = sqrt(r_on / NB);
    printf("  auto-gain at character +1, 0 dB drive, curve %d: off %.2f dBFS, on %.2f dBFS "
           "(input %.2f)\n", c, db(r_off), db(r_on), db(rms_in));
    check_num(db(r_off / rms_in) < -1.0,
              "CONTROL: the bias COSTS small-signal level when auto-gain is off",
              db(r_off / rms_in), -1.0);
    check_num(fabs(db(r_on / rms_in)) < 0.5,
              "auto-gain matches the level at character +1 too, to within 0.5 dB",
              db(r_on / rms_in), 0.0);
    free(sq);
  }

  /* Silence: the gain HOLDS rather than dividing by nothing. */
  static float quiet[1024], out[1024];
  memset(quiet, 0, sizeof(quiet));
  s = make_state(4);
  for (int i = 0; i < 40; i++) { memcpy(out, quiet, sizeof(quiet)); omx_drive_process(out, NULL, 1024, &p, s); }
  int finite = 1;
  for (int i = 0; i < 1024; i++) if (!isfinite(out[i]) || out[i] != 0.0f) finite = 0;
  check(finite, "digital silence through an auto-gained stage stays silence — no ratio on nothing");
  free(s);
}

/* ---- 9. the exciter topology ---------------------------------------------------------- */

static void test_exciter(void) {
  static float x[NB], y[NB];
  int k = tone(x, NB, 4000.0, 0.4);
  struct omx_drive p = atom(OMX_DRIVE_EXCITER, 18.0, 0.0, 0.2, 4);
  p.band = OMX_DRIVE_BAND_HIGH;
  struct omx_drive_state *s = make_state(4);
  run_steady(&p, s, x, NB, y);
  /* The dry is the FULL signal, so the fundamental is NOT attenuated the way a 20 % crossfade
   * would attenuate it — that difference is the whole of "an enhancer, not a distortion". */
  double fund_in = bin_mag(x, NB, (double)k / NB);
  double fund_out = bin_mag(y, NB, (double)k / NB);
  double h3 = db(bin_mag(y, NB, (double)(3 * k) / NB) / fund_out);
  printf("  exciter: fundamental %.2f dB re input, H3 %.1f dBc at mix 20%%\n",
         db(fund_out / fund_in), h3);
  check_num(db(fund_out / fund_in) > -1.0, "the exciter keeps the FULL dry fundamental",
            db(fund_out / fund_in), -1.0);
  check_num(h3 > -60.0, "and still adds harmonics at a 20 % mix", h3, -60.0);
  free(s);

  /* A 20 % CROSSFADE on the same signal loses level the parallel sum does not. */
  struct omx_drive q = atom(OMX_DRIVE_SOFT, 18.0, 0.0, 0.2, 4);
  q.band = OMX_DRIVE_BAND_HIGH;
  struct omx_drive_state *s2 = make_state(4);
  run_steady(&q, s2, x, NB, y);
  double cross = bin_mag(y, NB, (double)k / NB);
  check(cross < fund_out, "the crossfade topology sits BELOW the parallel one at the same mix");
  free(s2);
}

/* ---- 9b. the Enhancer presets: closed form at every declared rate (drive spec §4c) ------ */

/* The three exciter seeds of drive spec §4c (filed as /stagePresets/drive rows by the presets
 * spec's increment 5). `band_hz`/`hf_hz` are the corners; hf_hz 0 is `hfRolloff: off`. */
struct exciter_preset {
  const char *id;
  int band;
  double band_hz, drive_db, character, mix_pct, hf_hz;
};
static const struct exciter_preset EXCITER_PRESETS[3] = {
  { "exciterAir", OMX_DRIVE_BAND_HIGH, 5000.0, 12.0, 0.0, 20.0, 16000.0 },
  { "exciterPresence", OMX_DRIVE_BAND_HIGH, 2500.0, 9.0, 0.0, 15.0, 12000.0 },
  { "bassEnhancer", OMX_DRIVE_BAND_LOW, 120.0, 12.0, 0.3, 25.0, 0.0 },
};

static struct omx_drive preset_atom(const struct exciter_preset *e, double sr) {
  struct omx_drive p = atom_at(OMX_DRIVE_EXCITER, e->drive_db, e->character, e->mix_pct / 100.0, 4, sr);
  p.band = e->band;
  rbj_lowpass(p.band_c, e->band_hz, 0.7071, sr);
  p.hf_on = e->hf_hz > 0.0;
  if (p.hf_on) rbj_lowpass(p.hf_c, e->hf_hz, 0.7071, sr);
  return p;
}

/* The complex response at `f` of the biquad the kernel runs ({b0,b1,b2,a1,a2}), in double. */
static void biquad_resp(const float c[5], double f, double sr, double *re, double *im) {
  double w = 2.0 * M_PI * f / sr;
  double nr = c[0] + c[1] * cos(w) + c[2] * cos(2.0 * w), ni = -c[1] * sin(w) - c[2] * sin(2.0 * w);
  double dr = 1.0 + c[3] * cos(w) + c[4] * cos(2.0 * w), di = -c[3] * sin(w) - c[4] * sin(2.0 * w);
  double dd = dr * dr + di * di;
  *re = (nr * dr + ni * di) / dd;
  *im = (ni * dr - nr * di) / dd;
}
/* |B(f)|: the band the curve sees — LP for LOW, the complement 1 − LP for HIGH. */
static double band_gain(const struct omx_drive *p, double f, double sr) {
  double re, im;
  biquad_resp(p->band_c, f, sr, &re, &im);
  if (p->band == OMX_DRIVE_BAND_HIGH) re = 1.0 - re, im = -im;
  return sqrt(re * re + im * im);
}
/* |D(f)| of the DC blocker (1 − z⁻¹)/(1 − c·z⁻¹), which runs whenever even content is asked. */
static double dc_gain(const struct omx_drive *p, double f, double sr) {
  if (p->even_w <= 0.0f) return 1.0;
  double w = 2.0 * M_PI * f / sr, c = p->dc_coeff;
  double num = 2.0 - 2.0 * cos(w), den = 1.0 - 2.0 * c * cos(w) + c * c;
  return sqrt(num / den);
}
static double hf_gain(const struct omx_drive *p, double f, double sr) {
  if (!p->hf_on) return 1.0;
  double re, im;
  biquad_resp(p->hf_c, f, sr, &re, &im);
  return sqrt(re * re + im * im);
}
/* |c_k| of the kernel's own shaper over one period of a sine of amplitude `a`: quadrature in
 * double (the trapezoid is spectrally exact for a periodic integrand). */
static double shaper_harmonic(const struct omx_drive *p, double a, int k) {
  const int M = 8192;
  const struct omx_drive_bias bias = omx_drive_bias_for(OMX_DRIVE_EXCITER, p->even_w);
  double sa = 0.0, ca = 0.0;
  for (int m = 0; m < M; m++) {
    double psi = 2.0 * M_PI * (double)m / (double)M;
    double y = (double)omx_drive_sample(OMX_DRIVE_EXCITER, (float)(a * sin(psi)), &bias);
    sa += y * sin((double)k * psi);
    ca += y * cos((double)k * psi);
  }
  return 2.0 * sqrt(sa * sa + ca * ca) / (double)M;
}

/* Periodic block, run until every pole (the DC blocker's 5 Hz is the slowest) has settled. */
static void run_settled(const struct omx_drive *p, struct omx_drive_state *s, const float *in,
                        uint32_t n, float *out, double sr) {
  int passes = (int)ceil(0.5 * sr / (double)n) + 1;
  for (int pass = 0; pass < passes; pass++) {
    memcpy(out, in, n * sizeof(float));
    omx_drive_process(out, NULL, n, p, s);
  }
}

static int tone_at(float *x, uint32_t n, double hz, double amp, double sr) {
  int k = (int)(hz * (double)n / sr + 0.5);
  for (uint32_t i = 0; i < n; i++) x[i] = (float)(amp * sin(2.0 * M_PI * (double)k * (double)i / (double)n));
  return k;
}

/* ARM A: every H2/H3 above −80 dB re the tone equals mix·|c_k(G·A·|B(f)|)|·|D(kf)|·|R(kf)|. */
static void test_enhancer_presets_closed_form(void) {
  static float x[NB], y[NB];
  const double amp = pow(10.0, -12.0 / 20.0), tones[3] = { 100.0, 1000.0, 6000.0 };
  const double tol_db = 0.1, floor_db = -80.0;
  double worst = 0.0;
  int asserted = 0;
  for (uint32_t r = 0; r < OMX_DECLARED_RATE_COUNT; r++) {
    const double sr = (double)OMX_DECLARED_RATES[r];
    for (int e = 0; e < 3; e++) {
      const struct omx_drive p = preset_atom(&EXCITER_PRESETS[e], sr);
      for (int t = 0; t < 3; t++) {
        int k = tone_at(x, NB, tones[t], amp, sr);
        double f = (double)k * sr / (double)NB;
        struct omx_drive_state *s = make_state(4);
        run_settled(&p, s, x, NB, y, sr);
        free(s);
        double a = (double)p.drive_lin * amp * band_gain(&p, f, sr);
        for (int h = 2; h <= 3; h++) {
          double want = (double)p.mix * shaper_harmonic(&p, a, h) * dc_gain(&p, h * f, sr) *
                        hf_gain(&p, h * f, sr);
          double want_db = db(want / amp);
          if (want_db < floor_db) continue;
          double got_db = db(bin_mag(y, NB, (double)(h * k) / NB) / amp);
          double err = fabs(got_db - want_db);
          if (err > worst) worst = err;
          asserted++;
          char what[160];
          snprintf(what, sizeof what, "arm A: %s %.0f Hz H%d at %.0f Hz matches the closed form",
                   EXCITER_PRESETS[e].id, f, h, sr);
          check_num(err <= tol_db, what, got_db, want_db);
        }
      }
    }
  }
  printf("  arm A: %d harmonics above %.0f dB re the tone at %u rates, worst error %.4f dB (tol %.2f)\n",
         asserted, floor_db, OMX_DECLARED_RATE_COUNT, worst, tol_db);
  check(asserted >= 3 * (int)OMX_DECLARED_RATE_COUNT, "arm A asserted harmonics at every declared rate");
}

/* ARM B: exciterAir leaves a 100 Hz tone within mix·G·|HP(100 Hz)| of dry, + 1 dB. */
static void test_enhancer_untouched_band(void) {
  static float x[NB], y[NB];
  const double amp = pow(10.0, -12.0 / 20.0);
  for (uint32_t r = 0; r < OMX_DECLARED_RATE_COUNT; r++) {
    const double sr = (double)OMX_DECLARED_RATES[r];
    const struct omx_drive p = preset_atom(&EXCITER_PRESETS[0], sr);
    int k = tone_at(x, NB, 100.0, amp, sr);
    double f = (double)k * sr / (double)NB;
    struct omx_drive_state *s = make_state(4);
    run_settled(&p, s, x, NB, y, sr);
    free(s);
    const uint32_t lat = omx_oversampler_latency_for(4u);
    double e2 = 0.0, x2 = 0.0;
    for (uint32_t i = 0; i < NB; i++) {
      double d = (double)y[i] - (double)x[(i + NB - lat) % NB];
      e2 += d * d;
      x2 += (double)x[i] * (double)x[i];
    }
    double got_db = db(sqrt(e2 / x2));
    double bound_db = db((double)p.mix * (double)p.drive_lin * band_gain(&p, f, sr)) + 1.0;
    printf("  arm B: exciterAir at %.0f Hz, %.1f Hz: added %.1f dB re dry, bound %.1f dB\n", sr, f,
           got_db, bound_db);
    check_num(got_db <= bound_db, "arm B: the untouched band stays under the closed-form bound",
              got_db, bound_db);
    check_num(got_db > bound_db - 20.0, "arm B: the added term is really there (the probe landed)",
              got_db, bound_db - 20.0);
  }
}

/* ---- 10. stereo: LINKED means one gain path, and the proof is the mono comparison ---- */

/*
 * The first version of this test compared the L/R BALANCE before and after and expected it to
 * survive. It does not, and should not: a waveshaper is nonlinear, so a leg 12 dB quieter is
 * distorted far less and the pair's balance genuinely compresses. That is the curve's doing, not
 * the link's, and asserting it would have been asserting the wrong mechanism.
 *
 * What `stereo_link` actually decides is WHOSE energy the auto-gain measures. So the test is:
 * process one leg ALONE, then process the pair. Unlinked, the leg must come out the same as it
 * did alone (it measured only itself). Linked, it must come out DIFFERENT (it measured the pair).
 */
static void test_stereo_link(void) {
  static float l[NB], r[NB], mono[NB], ul[NB], ur[NB], ll[NB], rr[NB];
  tone(l, NB, 1000.0, 0.5);
  for (uint32_t i = 0; i < NB; i++) r[i] = l[i] * 0.25f; /* 12 dB quieter */

  struct omx_drive p = atom(OMX_DRIVE_SOFT, 18.0, -1.0, 1.0, 4);
  p.auto_gain = 1;

  struct omx_drive_state *sm = make_state(4);
  for (int pass = 0; pass < 4; pass++) { memcpy(mono, l, sizeof(l)); omx_drive_process(mono, NULL, NB, &p, sm); }
  free(sm);

  struct omx_drive q = p;
  q.stereo_link = 0;
  struct omx_drive_state *su = make_state(4);
  for (int pass = 0; pass < 4; pass++) {
    memcpy(ul, l, sizeof(l));
    memcpy(ur, r, sizeof(r));
    omx_drive_process(ul, ur, NB, &q, su);
  }
  free(su);

  struct omx_drive_state *sl = make_state(4);
  for (int pass = 0; pass < 4; pass++) {
    memcpy(ll, l, sizeof(l));
    memcpy(rr, r, sizeof(r));
    omx_drive_process(ll, rr, NB, &p, sl);
  }
  free(sl);

  double em = 0.0, eu = 0.0, el = 0.0;
  for (uint32_t i = NB / 2u; i < NB; i++) {
    em += (double)mono[i] * mono[i];
    eu += (double)ul[i] * ul[i];
    el += (double)ll[i] * ll[i];
  }
  double d_unlinked = db(sqrt(eu / em)), d_linked = db(sqrt(el / em));
  printf("  stereo: L leg vs the same leg alone — unlinked %+.3f dB, linked %+.3f dB\n",
         d_unlinked, d_linked);
  check_num(fabs(d_unlinked) < 0.05,
            "UNLINKED: each leg measures and compensates ITSELF, so it is unchanged by its partner",
            d_unlinked, 0.0);
  check_num(fabs(d_linked) > 0.5,
            "LINKED: the gain comes from the PAIR's energy, so the quiet partner moves this leg",
            fabs(d_linked), 0.5);
  /* And under the link the two legs are scaled by ONE number, so their ratio is the shaper's
   * alone — the property the link exists for. */
  double erl = 0.0, err = 0.0, eur = 0.0;
  for (uint32_t i = NB / 2u; i < NB; i++) {
    erl += (double)ll[i] * ll[i];
    err += (double)rr[i] * rr[i];
    eur += (double)ur[i] * ur[i];
  }
  printf("  stereo: pair balance linked %.2f dB, unlinked %.2f dB\n", db(sqrt(erl / err)),
         db(sqrt(eu / eur)));
  check_num(fabs(db(sqrt(erl / err)) - db(sqrt(eu / eur))) > 0.2,
            "and the two linkings really do give different balances", 
            fabs(db(sqrt(erl / err)) - db(sqrt(eu / eur))), 0.2);
}

/* ---- 11. chunking and latency --------------------------------------------------------- */

static void test_chunking_and_latency(void) {
  static float x[4096], one[4096], many[4096];
  for (int i = 0; i < 4096; i++) x[i] = 0.6f * (float)sin(0.13 * i) + 0.25f * (float)sin(1.9 * i);
  struct omx_drive p = atom(OMX_DRIVE_TAPE, 18.0, 0.3, 0.8, 4);
  struct omx_drive_state *a = make_state(4), *b = make_state(4);
  memcpy(one, x, sizeof(x));
  omx_drive_process(one, NULL, 4096, &p, a);
  memcpy(many, x, sizeof(x));
  const uint32_t chunks[6] = { 1u, 7u, 64u, 200u, 1024u, 2800u };
  uint32_t off = 0;
  for (int c = 0; c < 6; c++) { omx_drive_process(many + off, NULL, chunks[c], &p, b); off += chunks[c]; }
  check(off == 4096u, "the chunk sizes cover the signal");
  int same = 1;
  for (int i = 0; i < 4096; i++) if (fabsf(one[i] - many[i]) > 1e-6f) same = 0;
  check(same, "block size does not change the result — the stage is STREAMING");
  free(a);
  free(b);

  /* The declared latency, measured: an impulse through the stage at mix 1 peaks where
   * omx_drive_latency says. */
  for (int f = 1; f <= 4; f *= 2) {
    struct omx_drive q = atom(OMX_DRIVE_SOFT, 0.0, -1.0, 1.0, f);
    struct omx_drive_state *s = make_state(f);
    static float imp[2048];
    memset(imp, 0, sizeof(imp));
    imp[0] = 0.5f;
    omx_drive_process(imp, NULL, 2048, &q, s);
    int peak = 0;
    float best = 0.0f;
    for (int i = 0; i < 2048; i++) if (fabsf(imp[i]) > best) { best = fabsf(imp[i]); peak = i; }
    check_num(peak == omx_drive_latency(&q), "the stage's MEASURED latency is what it declares",
              (double)peak, (double)omx_drive_latency(&q));
    struct omx_drive offq = q;
    offq.enabled = 0;
    check(omx_drive_latency(&offq) == 0, "a disengaged stage declares ZERO latency");
    free(s);
  }
}

/* ---- 12. cost ------------------------------------------------------------------------- */

static void report_cost(void) {
  static float l[1024], r[1024];
  for (int i = 0; i < 1024; i++) { l[i] = 0.5f * (float)sin(0.07 * i); r[i] = l[i]; }
  for (int f = 1; f <= 4; f *= 2) {
    struct omx_drive p = atom(OMX_DRIVE_SOFT, 12.0, 0.0, 1.0, f);
    p.auto_gain = 1;
    struct omx_drive_state *s = make_state(f);
    const int reps = 3000;
    clock_t t0 = clock();
    for (int i = 0; i < reps; i++) omx_drive_process(l, r, 1024, &p, s);
    double secs = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    /* Per second of audio PER CHANNEL: the block is stereo, so two channels' worth per rep. */
    double per = secs / ((double)reps * 1024.0 / SR) * 1e6 / 2.0;
    printf("  cost: factor %d -> %.0f us per second of audio per channel at %.0f kHz\n", f, per,
           SR / 1000.0);
    free(s);
  }
  printf("  state: %zu bytes per engaged strip, allocated on first enable and never on RT\n",
         sizeof(struct omx_drive_state));
}

int main(void) {
  omx_fx_require_rate_floor();
  printf("fx/drive oracle — chunk %u, %u declared rates\n", OMX_DRIVE_CHUNK, OMX_DECLARED_RATE_COUNT);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = (double)OMX_DECLARED_RATES[ri];
    printf(" rate %.0f Hz\n", SR);
    test_control_analyser();
    test_bypass_is_untouched();
    test_mix_zero_is_dry();
    test_curve_shapes();
    test_thd_per_curve();
    test_character_even_odd();
    test_character_is_bounded();
    test_alias_floor();
    test_band();
    test_auto_gain();
    test_exciter();
    test_stereo_link();
    test_chunking_and_latency();
  }
  test_enhancer_presets_closed_form();
  test_enhancer_untouched_band();
  g_sr = 96000.0; /* the cost is quoted at the rate the rig runs */
  report_cost();
  printf("fx/drive: %d checks, %d failures (%u rates)\n", g_checks, g_fail, OMX_DECLARED_RATE_COUNT);
  return g_fail == 0 ? 0 : 1;
}
