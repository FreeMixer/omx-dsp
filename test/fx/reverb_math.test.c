// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The reverb family (omx_reverb.h) against its MATH, at the four floor rates fx_rates.h requires.
 *
 *   make test-fx
 *
 * reverb.test.c proves the voices exist and differ, at every declared rate. This file proves the NUMBERS:
 * the decay each configuration declares, how it moves with the sample rate, and the laws a
 * reverb has to keep between blocks. It is the measured half of
 * docs/design/notes/2026-09-17-reverb-math-review.md, and every tolerance below is a measured
 * figure from that note with a band around it — never a round number chosen to pass.
 *
 * Three of these arms carried a DIVERGENCE until the operator ruled on them (2026-09-17, note
 * SS9): the damping pole was a per-sample constant so a damped decay drifted with the rate
 * (R-1), the plate's tank was unmodulated so it rang at its own allpass loop (R-2), and GATED
 * re-armed in one sample (R-7). All three are now LAWS and the arms assert them: the reference
 * rate is 96 000 Hz and every other rate must match its figures within 1 %, the plate's late tail
 * must not be periodic, and the arming may add no step the ROOM reference does not already
 * carry. The 96 kHz responses themselves are pinned sample for sample against a fixture cut
 * before the rulings landed, because "the desk's rate does not move" is an identity claim.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_reverb.h>
#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_eq_design.h>

#include "fx_rates.h"
#include "fixtures/reverb_96k_render.h"
#include "fixtures/reverb_96k_reference.h"
#include "fixtures/reverb_preset_seeds.h"

static int g_fail = 0, g_checks = 0;
static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) { g_fail++; fprintf(stderr, "FAIL: %s\n", what); }
}
/* A band, because a single-sided bound on a measured quantity only catches one of its two ways
 * of going wrong. */
static void in_band(double v, double lo, double hi, const char *what) {
  g_checks++;
  if (!(v >= lo && v <= hi)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s — measured %.6g, wanted [%.6g, %.6g]\n", what, v, lo, hi);
  }
}

#define NRATES 4
/* Arm A's band on T30 / RT60_DC, set below from the measured spread. */
static const float RATE[NRATES] = { 44100.0f, 48000.0f, 96000.0f, 192000.0f };
static const char *ANAME[5] = { "room", "plate", "hall", "reverse", "gated" };

static struct omx_reverb_state *mk(float sr) {
  struct omx_reverb_state *s = calloc(1, sizeof *s);
  float *pool = calloc(OMX_REVERB_POOL_FLOATS, sizeof(float));
  omx_reverb_state_layout(s, pool, OMX_REVERB_POOL_FLOATS, sr);
  return s;
}
static void rel(struct omx_reverb_state *s) { free(s->_pool); free(s); }
/* Always in blocks: a kernel that only works when handed the whole take is not an insert. */
static void run(struct omx_reverb_state *s, struct omx_reverb *p, float *l, float *r, int n, float sr) {
  for (int o = 0; o < n; o += 256) {
    int k = (n - o) < 256 ? (n - o) : 256;
    omx_reverb_process(l + o, r + o, k, p, s, sr);
  }
}
static struct omx_reverb params(int algo, float size, float damping) {
  struct omx_reverb p;
  memset(&p, 0, sizeof p);
  p.enabled = 1; p.algorithm = algo; p.size = size; p.damping = damping;
  p.predelay_ms = 0.0f; p.width = 1.0f; p.mix = 1.0f; p.lowcut = 0.0f; p.highcut = 0.0f;
  p.reverse_ms = 300.0f; p.hold_ms = 120.0f; p.release_ms = 20.0f; p.gate_threshold_db = -40.0f;
  p.plate_mod_depth = OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT;
  return p;
}
static double energy(const float *l, const float *r, int a, int b) {
  double e = 0.0;
  for (int i = a; i < b; i++) e += (double)l[i] * l[i] + (double)r[i] * r[i];
  return e;
}
/* RT60 as the least-squares slope of 20 ms-window RMS in dB — the definition the note states. */
static double rt60_ms(const float *x, int n, float sr, double t0, double t1) {
  int w = (int)(0.02f * sr);
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  int m = 0;
  for (int i = (int)(t0 * sr); i + w < (int)(t1 * sr) && i + w < n; i += w) {
    double e = 0.0;
    for (int k = 0; k < w; k++) e += (double)x[i + k] * x[i + k];
    double rms = sqrt(e / w);
    if (rms < 1e-12) continue;
    double t = (double)i / sr, y = 20.0 * log10(rms);
    sx += t; sy += y; sxx += t * t; sxy += t * y; m++;
  }
  if (m < 4) return -1.0;
  double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
  if (slope >= -1e-6) return -1.0;
  return -60.0 / slope * 1000.0;
}
/* One-pole split, so a band claim is made on a band and not on a whole signal. */
static void split(const float *x, float *lo, float *hi, int n, float sr, float f) {
  float c = 1.0f - expf(-2.0f * 3.14159265f * f / sr), st = 0.0f;
  for (int i = 0; i < n; i++) { st += c * (x[i] - st); lo[i] = st; hi[i] = x[i] - st; }
}
static unsigned g_rng = 1u;
static float noise(void) {
  g_rng = g_rng * 1103515245u + 12345u;
  return ((float)((g_rng >> 9) & 0x7fffff) / 4194304.0f) - 1.0f;
}

/* ---- 1. the lengths ------------------------------------------------------------------------- */

/* Every line is `ms -> samples` at the rate, so the LOOP TIME is the rate-invariant quantity and
 * the sample count is not. And no two of the sixteen room lines may land on the same length: a
 * collision is two combs with one mode, which is modal density silently lost. */
static void test_line_geometry_is_rate_invariant(void) {
  double ms[NRATES];
  for (int ri = 0; ri < NRATES; ri++) {
    struct omx_reverb_state *s = mk(RATE[ri]);
    ms[ri] = (double)s->comb_len_r[7] / RATE[ri] * 1000.0;
    uint32_t v[16];
    for (int i = 0; i < 8; i++) { v[i] = s->comb_len_l[i]; v[8 + i] = s->comb_len_r[i]; }
    int collisions = 0;
    for (int i = 0; i < 16; i++)
      for (int j = i + 1; j < 16; j++)
        if (v[i] == v[j]) collisions++;
    char what[96];
    snprintf(what, sizeof what, "%.0f Hz: no two of room's 16 comb lines are the same length", (double)RATE[ri]);
    check(collisions == 0, what);
    /* HALL and PLATE keep their own lines beside room's, all laid out at once. */
    check(s->comb_len_l_hall[7] > s->comb_len_l[7], "hall's longest comb outruns room's at every rate");
    check(s->plate_tank_len[3] > s->plate_tank_len[2], "the plate's tank keeps its long/short pair");
    rel(s);
  }
  double spread = ms[NRATES - 1] / ms[0];
  in_band(spread, 0.999, 1.001, "the longest room loop is the same TIME at 44.1 k and 192 k");
  in_band(ms[0], 37.0, 37.4, "the longest room loop measures ~37.2 ms");
}

/* ---- 2. RT60, declared against measured ------------------------------------------------------ */

static double declared_rt60_ms(struct omx_reverb_state *s, int algo, float size, float sr) {
  if (algo == OMX_REVERB_ROOM) {
    float fb = 0.28f + 0.7f * size;
    return (double)s->comb_len_r[7] / sr * 1000.0 * log(0.001) / log(fb);
  }
  if (algo == OMX_REVERB_HALL) {
    float fb = 0.90f + 0.09f * size;
    return (double)s->comb_len_r_hall[7] / sr * 1000.0 * log(0.001) / log(fb);
  }
  float g = OMX_REVERB_PLATE_DECAY_FLOOR + OMX_REVERB_PLATE_DECAY_SPAN * size;
  double loop = (double)(s->plate_tank_len[2] + s->plate_tank_len[3]) / sr * 1000.0;
  return loop * log(0.001) / log(g);
}

/* The tail's length is the longest loop's, by the closed form the spec states. ROOM measures 4.5 %
 * short of it (the seven shorter combs pull the fitted slope down). PLATE measured 19.4 % LONG of
 * it until R-2 modulated the tank — and that excess WAS the ring: an unmodulated allpass
 * recirculating at 0.7 kept re-feeding the tank past its own loop gain, which is the same energy
 * the 0.709 autocorrelation peak was reporting. With the tank modulated the plate measures 2.6 to
 * 4.1 % SHORT of the form (the modulated read's mean delay is the line minus one excursion), so
 * the declared number is now what the operator hears instead of a floor the voice ran past.
 * All three ratios must hold across the rate span, which is the actual claim: a reverb's room does
 * not change size when the clock does. */
static void test_rt60_matches_its_declared_form_at_every_rate(void) {
  double ratio[3][NRATES];
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = (int)(4.0f * sr);
    float *l = calloc(n, 4), *r = calloc(n, 4), *lo = calloc(n, 4), *hi = calloc(n, 4);
    for (int ai = 0; ai < 3; ai++) {
      int algo = ai == 0 ? OMX_REVERB_ROOM : (ai == 1 ? OMX_REVERB_PLATE : OMX_REVERB_HALL);
      struct omx_reverb_state *s = mk(sr);
      struct omx_reverb p = params(algo, 0.5f, 0.0f);
      memset(l, 0, (size_t)n * 4); memset(r, 0, (size_t)n * 4);
      l[0] = 1.0f; r[0] = 1.0f;
      run(s, &p, l, r, n, sr);
      split(l, lo, hi, n, sr, 1000.0f); /* below the damping corner, so only the loop can speak */
      double meas = rt60_ms(lo, n, sr, 0.2, 2.5);
      ratio[ai][ri] = meas / declared_rt60_ms(s, algo, 0.5f, sr);
      char what[128];
      snprintf(what, sizeof what, "%.0f Hz %s: measured RT60 against its declared form", (double)sr, ANAME[algo]);
      if (algo == OMX_REVERB_ROOM) in_band(ratio[ai][ri], 0.93, 0.98, what);
      else if (algo == OMX_REVERB_PLATE) in_band(ratio[ai][ri], 0.94, 0.99, what);
      else in_band(ratio[ai][ri], 0.76, 0.81, what); /* hall held its rate by R-1's reference pole */
      rel(s);
    }
    free(l); free(r); free(lo); free(hi);
  }
  in_band(ratio[0][3] / ratio[0][0], 0.995, 1.005, "room's RT60 is the same fraction of its form at 44.1 k and 192 k");
  /* The plate's band is wider than room's since R-2: a modulated tank's fitted slope depends on
   * where the LFO's per-sample increment lands inside the loop, and that is a function of the
   * rate. 1.5 % measured across a 4.35x rate span, against 0.5 % for the unmodulated combs. */
  in_band(ratio[1][3] / ratio[1][0], 0.98, 1.02, "plate's RT60 is the same fraction of its form at 44.1 k and 192 k");
  /* HALL's tail used to grow 8.3 % across the rate span, because its damping FLOOR of 0.35 filters
   * even at damping 0 and the pole was a per-sample constant (R-1). With the pole quoted at the
   * 96 kHz reference it holds — to 2 %, which is the band the fitted slope of a nine-second tail
   * can be read to, against room's 0.5 %. */
  in_band(ratio[2][3] / ratio[2][0], 0.98, 1.02, "hall's RT60 is the same fraction of its form at 44.1 k and 192 k");
}

/* ---- 3. R-1's identity half: at the REFERENCE RATE nothing moved ------------------------------ */

/* Distance in representable floats, so a report can say "1 ulp" rather than "different". */
static int ulps_apart(float a, float b) {
  int32_t ia, ib;
  memcpy(&ia, &a, sizeof ia); memcpy(&ib, &b, sizeof ib);
  if (ia < 0) ia = (int32_t)0x80000000 - ia;
  if (ib < 0) ib = (int32_t)0x80000000 - ib;
  long d = (long)ia - (long)ib;
  return (int)(d < 0 ? -d : d);
}

/* R-1 normalises the damping pole to a reference rate of 96 000 Hz — the rate the desk runs
 * (ruling 2026-09-14) — precisely so the desk's sound does not move: the exponent there is
 * exactly 1 and the pole is the number it always was. That is an IDENTITY, and an identity is
 * checked against the old output, never re-derived: `test/fixtures/reverb_96k_reference.h` was
 * cut before the ruling touched the kernel. Fifteen rows (five configurations x three damping
 * settings), each an FNV-1a hash over the whole 2 x 32 768-sample response plus 64 decimated taps
 * per leg so a divergence can be localised and measured in ulps.
 *
 * The plate's three rows were deliberately re-cut afterwards, because R-2 modulates its tank and
 * that response was MEANT to change. ROOM, HALL, REVERSE and GATED still carry their pre-ruling
 * rows: R-1 reaches all four through the damping filter and left every sample where it was.
 *
 * WHAT THESE ROWS DO NOT COVER. The reference excitation keys GATED continuously from before the
 * network's first wet sample, so its envelope is already at 1 by the time there is anything to
 * multiply — the gated rows pin the NETWORK UNDER the gate, never R-7's arming ramp. A green row
 * here is not a statement about the arming; `test_gate_rearm_adds_no_step_the_reference_does_not`
 * is, at all four rates, with a hit that lands mid-release. */
static void test_the_reference_rate_response_is_unchanged(void) {
  int n = OMX_REF96_N;
  float *l = calloc(n, 4), *r = calloc(n, 4);
  int worst = 0;
  const char *worst_at = "none";
  for (int row = 0; row < OMX_REF96_ROWS; row++) {
    struct omx_reverb_state *s = mk(OMX_REF96_SR);
    struct omx_reverb p = params(OMX_REF96[row].algo, 0.6f, OMX_REF96[row].damping);
    p.predelay_ms = 7.0f; p.reverse_ms = 100.0f;
    omx_ref96_excite(l, r, n);
    run(s, &p, l, r, n, OMX_REF96_SR);
    int differing = 0, first = -1;
    for (int t = 0; t < OMX_REF96_TAPS; t++) {
      float refl, refr;
      memcpy(&refl, &OMX_REF96[row].l[t], sizeof refl);
      memcpy(&refr, &OMX_REF96[row].r[t], sizeof refr);
      int u = ulps_apart(l[t * OMX_REF96_DECIMATE], refl);
      int v = ulps_apart(r[t * OMX_REF96_DECIMATE], refr);
      int m = u > v ? u : v;
      if (m > 0) { differing++; if (first < 0) first = t; }
      if (m > worst) { worst = m; worst_at = OMX_REF96[row].name; }
    }
    char what[144];
    snprintf(what, sizeof what, "96 kHz %s: the whole response still hashes to the reference",
             OMX_REF96[row].name);
    check(omx_ref96_hash(l, n) == OMX_REF96[row].hash_l &&
          omx_ref96_hash(r, n) == OMX_REF96[row].hash_r, what);
    if (differing)
      fprintf(stderr, "  96 kHz %s: %d of %d taps differ, first at sample %d\n",
              OMX_REF96[row].name, differing, OMX_REF96_TAPS, first * OMX_REF96_DECIMATE);
    rel(s);
  }
  free(l); free(r);
  printf("  R-1 identity: 96 kHz matches the reference to %d ulp (worst row: %s)\n", worst, worst_at);
}

/* ---- 4. R-1: the damping pole is normalised to the reference rate --------------------------- */

/* A one-pole's corner is -ln(p)*sr/2pi, so a pole used raw is a DIFFERENT FILTER at every rate:
 * damping 0.6 cornered at 3 585 Hz at 44.1 kHz and at 15 610 Hz at 192 kHz, which measured as a
 * ROOM HF decay 10.6 % longer and a HALL tail 8.3 % longer at 192 k than at 44.1 k (note R-1).
 * Ruling 2026-09-17: the pole is raised to REF/sr, so the corner is the one the 96 kHz reference
 * has at every rate. The tail below the corner was always rate-invariant — the unity-DC law of
 * Amendment 2026-09-17 SSA — and stays so; what this arm now demands is that the HF decay and
 * HALL's own tail match their 96 kHz figures within 1 %. */
static void test_damped_decay_matches_the_reference_rate_within_1_percent(void) {
  double hf[NRATES], lf[NRATES], hall[NRATES];
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = (int)(3.0f * sr);
    float *l = calloc(n, 4), *r = calloc(n, 4), *lo = calloc(n, 4), *hi = calloc(n, 4);
    float *lo8 = calloc(n, 4), *band = calloc(n, 4);
    struct omx_reverb_state *s = mk(sr);
    struct omx_reverb p = params(OMX_REVERB_ROOM, 0.5f, 0.6f);
    memset(l, 0, (size_t)n * 4); memset(r, 0, (size_t)n * 4);
    l[0] = 1.0f; r[0] = 1.0f;
    run(s, &p, l, r, n, sr);
    split(l, lo, hi, n, sr, 4000.0f);
    lf[ri] = rt60_ms(lo, n, sr, 0.2, 2.0);
    /* THE HF BAND IS 4-8 kHz, NOT "everything above 4 kHz". A one-sided high band runs to
     * Nyquist, so at 192 kHz it carries 88 kHz of fast-decaying air that 44.1 kHz does not have
     * at all, and the fitted decay would differ by the BANDWIDTH even over an identical filter —
     * a measurement that cannot be compared across rates cannot judge a rate law. Both edges are
     * inside every declared rate's band, so the four columns describe the same air. */
    split(l, lo8, hi, n, sr, 8000.0f);
    for (int i = 0; i < n; i++) band[i] = lo8[i] - lo[i];
    hf[ri] = rt60_ms(band, n, sr, 0.05, 0.6);
    rel(s);
    struct omx_reverb_state *sh = mk(sr);
    struct omx_reverb ph = params(OMX_REVERB_HALL, 0.5f, 0.0f);
    memset(l, 0, (size_t)n * 4); memset(r, 0, (size_t)n * 4);
    l[0] = 1.0f; r[0] = 1.0f;
    run(sh, &ph, l, r, n, sr);
    split(l, lo, hi, n, sr, 1000.0f);
    hall[ri] = rt60_ms(lo, n, sr, 0.2, 2.5);
    rel(sh);
    free(l); free(r); free(lo); free(hi); free(lo8); free(band);
    char what[112];
    snprintf(what, sizeof what, "%.0f Hz: damping 0.6 shortens the 4-8 kHz decay below the low band's", (double)sr);
    check(hf[ri] < lf[ri] * 0.85, what);
  }
  printf("  R-1 rates: room 4-8 kHz decay %.1f / %.1f / %.1f / %.1f ms, hall tail %.1f / %.1f / %.1f / %.1f ms\n",
         hf[0], hf[1], hf[2], hf[3], hall[0], hall[1], hall[2], hall[3]);
  in_band(lf[3] / lf[0], 0.99, 1.02, "the LOW band's decay is rate-invariant (damping never shortens the tail)");
  /* REF is index 2 — the 96 kHz column is the reference every other rate is judged against. */
  for (int ri = 0; ri < NRATES; ri++) {
    char what[144];
    snprintf(what, sizeof what, "R-1: %.0f Hz room 4-8 kHz decay is within 1 %% of the 96 kHz reference", (double)RATE[ri]);
    in_band(hf[ri] / hf[2], 0.99, 1.01, what);
    snprintf(what, sizeof what, "R-1: %.0f Hz hall tail is within 1 %% of the 96 kHz reference", (double)RATE[ri]);
    in_band(hall[ri] / hall[2], 0.99, 1.01, what);
  }
}

/* ---- 5. R-2: the plate's tank is modulated ----------------------------------------------------- */

static double autocorr_peak(const float *x, int a, int len, int lag_lo, int lag_hi, int *at) {
  double e0 = 0.0;
  for (int i = 0; i < len; i++) e0 += (double)x[a + i] * x[a + i];
  double best = 0.0;
  for (int lag = lag_lo; lag < lag_hi; lag++) {
    double c = 0.0;
    for (int i = 0; i < len - lag; i++) c += (double)x[a + i] * x[a + i + lag];
    c = fabs(c) / e0;
    if (c > best) { best = c; *at = lag; }
  }
  return best;
}
/* A late tail must not be periodic: a peak in its autocorrelation is a flutter, heard as metal.
 * ROOM and HALL diffuse theirs away. The PLATE did not — its tank carried no modulation and its
 * first tank allpass recirculates at 0.7, so it rang at 0.709 at 22.6 ms, its own tank length,
 * against ROOM's 0.26 (note R-2). Ruling 2026-09-17: the tank's first allpass in EACH half is
 * modulated per Dattorro, and the peak must now sit below 0.40 — ROOM's own neighbourhood — at
 * every rate and on BOTH legs, because each half feeds one leg and modulating only one of them
 * leaves the other ringing and the mono sum brings it back. */
static void test_late_tail_periodicity(void) {
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = (int)(3.0f * sr);
    float *l = calloc(n, 4), *r = calloc(n, 4);
    for (int ai = 0; ai < 3; ai++) {
      int algo = ai == 0 ? OMX_REVERB_ROOM : (ai == 1 ? OMX_REVERB_PLATE : OMX_REVERB_HALL);
      struct omx_reverb_state *s = mk(sr);
      struct omx_reverb p = params(algo, 0.5f, 0.2f);
      memset(l, 0, (size_t)n * 4); memset(r, 0, (size_t)n * 4);
      l[0] = 1.0f; r[0] = 1.0f;
      run(s, &p, l, r, n, sr);
      int at = 0, atr = 0;
      double pk = autocorr_peak(l, (int)(0.5f * sr), (int)(0.5f * sr),
                                (int)(0.001f * sr), (int)(0.12f * sr), &at);
      double pkr = autocorr_peak(r, (int)(0.5f * sr), (int)(0.5f * sr),
                                 (int)(0.001f * sr), (int)(0.12f * sr), &atr);
      char what[144];
      snprintf(what, sizeof what, "%.0f Hz %s: late-tail autocorrelation peak", (double)sr, ANAME[algo]);
      if (algo == OMX_REVERB_PLATE) {
        printf("  R-2 %.0f Hz plate: peak %.3f @ %.1f ms (L), %.3f @ %.1f ms (R)\n", (double)sr,
               pk, (double)at / sr * 1000.0, pkr, (double)atr / sr * 1000.0);
        in_band(pk, 0.0, 0.40, what);
        snprintf(what, sizeof what, "%.0f Hz plate: the RIGHT leg's tank does not ring either", (double)sr);
        in_band(pkr, 0.0, 0.40, what);
      } else if (algo == OMX_REVERB_ROOM) {
        in_band(pk, 0.15, 0.35, what);
      } else {
        in_band(pk, 0.05, 0.25, what);
      }
      rel(s);
    }
    free(l); free(r);
  }
}

/* ---- 5b. plateModDepth: the depth is a field ---------------------------------------------------
 * 2026-09-26-native-fx-catalogue.md §2 (Reverb), SMALL EXTENSION. Three claims:
 *   IDENTITY: at the declared come-up (100 %) the plate renders EXACTLY what it rendered before
 *   the field existed. The four hashes are FNV-1a over both legs of a 1 s impulse through
 *   params(PLATE, 0.5, 0.2), 256-frame blocks, taken from the kernel at 1947986df, before the
 *   field — a byte-for-byte comparison with the pre-field render, not with itself.
 *   CLOSED FORM: the excursion is depth/100 · 8 · sr/29761 samples at every declared rate.
 *   THE AUDIO MOVES: depth 0 is the unmodulated tank, and R-2's ring comes back on BOTH legs —
 *   above the 0.40 ceiling the modulation holds, and at least 1.25x the come-up's own peak (R-2
 *   measured 0.709 left, 0.53 right); the come-up keeps it < 0.40. */
static uint64_t fnv1a(uint64_t h, const float *x, int n) {
  const unsigned char *b = (const unsigned char *)x;
  for (size_t i = 0; i < (size_t)n * sizeof(float); i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}
static const uint64_t PLATE_PRE_FIELD_HASH[NRATES] = {
  0x53514955905f3f42ull, 0x21bfa6bf5c5f3ac9ull, 0x0ee3e56e55398f4dull, 0x1d668a86f3822907ull,
};
static void test_plate_mod_depth_is_a_field(void) {
  static const float DEPTHS[4] = { 0.0f, 50.0f, 100.0f, 400.0f };
  for (int ri = 0; ri < NRATES; ri++) {
    const float sr = RATE[ri];
    char what[160];
    struct omx_reverb_state *s = mk(sr);
    const float shortest = (float)(s->plate_tank_len[0] < s->plate_tank_len[2] ? s->plate_tank_len[0]
                                                                              : s->plate_tank_len[2]);
    for (int di = 0; di < 4; di++) {
      const double want = (double)DEPTHS[di] / 100.0 * 8.0 * (double)sr / 29761.0;
      snprintf(what, sizeof what, "%.0f Hz: excursion at depth %.0f %% (closed form %.4f samples)",
               (double)sr, (double)DEPTHS[di], want);
      in_band(omx_reverb_plate_excursion(DEPTHS[di], sr, shortest), want * (1.0 - 1e-6), want * (1.0 + 1e-6) + 1e-9, what);
    }
    snprintf(what, sizeof what, "%.0f Hz: a depth past the line's length swings as wide as the line holds", (double)sr);
    in_band(omx_reverb_plate_excursion(1.0e6f, sr, shortest), (shortest - 2.0f) * 0.5f, (shortest - 2.0f) * 0.5f, what);
    snprintf(what, sizeof what, "%.0f Hz: a NaN depth is no modulation", (double)sr);
    in_band(omx_reverb_plate_excursion(NAN, sr, shortest), 0.0, 0.0, what);
    rel(s);

    const int n = (int)sr;
    float *l = calloc((size_t)n, 4), *r = calloc((size_t)n, 4);
    s = mk(sr);
    struct omx_reverb p = params(OMX_REVERB_PLATE, 0.5f, 0.2f);
    l[0] = 1.0f; r[0] = 1.0f;
    run(s, &p, l, r, n, sr);
    const uint64_t h = fnv1a(fnv1a(1469598103934665603ull, l, n), r, n);
    snprintf(what, sizeof what, "%.0f Hz: plate at the come-up depth is byte-identical to the pre-field render", (double)sr);
    in_band(h == PLATE_PRE_FIELD_HASH[ri] ? 1.0 : 0.0, 1.0, 1.0, what);
    rel(s); free(l); free(r);

    const int m = (int)(3.0f * sr);
    l = calloc((size_t)m, 4); r = calloc((size_t)m, 4);
    double pk[2][2];
    static const float PEAK_DEPTH[2] = { 0.0f, 100.0f };
    for (int di = 0; di < 2; di++) {
      s = mk(sr);
      p = params(OMX_REVERB_PLATE, 0.5f, 0.2f);
      p.plate_mod_depth = PEAK_DEPTH[di];
      memset(l, 0, (size_t)m * 4); memset(r, 0, (size_t)m * 4);
      l[0] = 1.0f; r[0] = 1.0f;
      run(s, &p, l, r, m, sr);
      int at = 0;
      pk[di][0] = autocorr_peak(l, (int)(0.5f * sr), (int)(0.5f * sr), (int)(0.001f * sr), (int)(0.12f * sr), &at);
      pk[di][1] = autocorr_peak(r, (int)(0.5f * sr), (int)(0.5f * sr), (int)(0.001f * sr), (int)(0.12f * sr), &at);
      rel(s);
    }
    printf("  5b %.0f Hz plate: late-tail peak L/R depth 0 %.3f/%.3f, depth 100 %.3f/%.3f\n", (double)sr,
           pk[0][0], pk[0][1], pk[1][0], pk[1][1]);
    for (int leg = 0; leg < 2; leg++) {
      snprintf(what, sizeof what, "%.0f Hz plate %s leg: depth 0 rings again (R-2's unmodulated tank)", (double)sr, leg ? "right" : "left");
      in_band(pk[0][leg], 0.40, 1.0, what);
      snprintf(what, sizeof what, "%.0f Hz plate %s leg: depth 0's ring over the come-up's (ratio)", (double)sr, leg ? "right" : "left");
      in_band(pk[0][leg] / pk[1][leg], 1.25, 100.0, what);
      snprintf(what, sizeof what, "%.0f Hz plate %s leg: the come-up depth does not ring", (double)sr, leg ? "right" : "left");
      in_band(pk[1][leg], 0.0, 0.40, what);
    }
    free(l); free(r);
  }
}

/* ---- 6. the laws a reverb keeps between blocks ------------------------------------------------ */

/* A recursive network with unity-DC damping has DC gain 1/(1-feedback) per comb: bounded, but
 * only if nothing rectifies. 60 s of noise is the test, because a build-up is a SLOW failure that
 * a one-second oracle cannot see. */
static void test_no_dc_build_up_over_60_s(void) {
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    long total = (long)(60.0f * sr), wlen = (long)sr, wn = 0;
    struct omx_reverb_state *s = mk(sr);
    struct omx_reverb p = params(OMX_REVERB_ROOM, 0.8f, 0.3f);
    float l[256], r[256];
    double sum = 0.0, wsum = 0.0, wmax = 0.0;
    g_rng = 999u;
    for (long done = 0; done < total; done += 256) {
      for (int i = 0; i < 256; i++) { l[i] = 0.2f * noise(); r[i] = 0.2f * noise(); }
      omx_reverb_process(l, r, 256u, &p, s, sr);
      for (int i = 0; i < 256; i++) { sum += l[i]; wsum += l[i]; }
      wn += 256;
      if (wn >= wlen) { double m = wsum / (double)wn; if (fabs(m) > wmax) wmax = fabs(m); wsum = 0.0; wn = 0; }
    }
    char what[112];
    snprintf(what, sizeof what, "%.0f Hz: 60 s of noise leaves no DC in the wet", (double)sr);
    check(fabs(sum / (double)total) < 1e-3, what);
    snprintf(what, sizeof what, "%.0f Hz: no 1 s window of those 60 drifts off zero", (double)sr);
    check(wmax < 5e-3, what);
    rel(s);
  }
}

/* Summing the legs must not comb-cancel: two uncorrelated legs give -3 dB and that is the target,
 * not silence. A mono PA is what most of this console's tails end up on. */
static void test_mono_compatible(void) {
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = (int)(2.5f * sr);
    float *l = calloc(n, 4), *r = calloc(n, 4);
    for (int ai = 0; ai < 3; ai++) {
      int algo = ai == 0 ? OMX_REVERB_ROOM : (ai == 1 ? OMX_REVERB_PLATE : OMX_REVERB_HALL);
      struct omx_reverb_state *s = mk(sr);
      struct omx_reverb p = params(algo, 0.5f, 0.2f);
      memset(l, 0, (size_t)n * 4); memset(r, 0, (size_t)n * 4);
      l[0] = 1.0f; r[0] = 1.0f;
      run(s, &p, l, r, n, sr);
      int a = (int)(0.2f * sr), b = (int)(2.0f * sr);
      double el = 0, er = 0, es = 0;
      for (int i = a; i < b; i++) {
        el += (double)l[i] * l[i]; er += (double)r[i] * r[i];
        double m = 0.5 * (l[i] + r[i]); es += m * m;
      }
      double db = 20.0 * log10(sqrt(es / (b - a)) / sqrt(0.5 * (el + er) / (b - a)));
      char what[128];
      snprintf(what, sizeof what, "%.0f Hz %s: the mono sum of the tail does not comb-cancel", (double)sr, ANAME[algo]);
      in_band(db, -4.5, -1.5, what);
      rel(s);
    }
    free(l); free(r);
  }
}

/* Parameters move on BLOCK boundaries; the question is whether that grid is audible. Measured
 * against a 200 Hz sine, whose own largest step is the yardstick: a ramp that never steps further
 * than the signal does cannot zipper. */
static void test_parameter_ramps_do_not_zipper(void) {
  static const char *WHICH[4] = { "mix", "size", "damping", "width" };
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = (int)(1.0f * sr);
    for (int w = 0; w < 4; w++) {
      float *l = calloc(n, 4), *r = calloc(n, 4);
      for (int i = 0; i < n; i++) l[i] = r[i] = 0.5f * sinf(2.0f * 3.14159265f * 200.0f * i / sr);
      struct omx_reverb_state *s = mk(sr);
      struct omx_reverb p = params(OMX_REVERB_ROOM, 0.5f, 0.3f);
      p.mix = 0.5f;
      for (int o = 0; o < n; o += 256) {
        int k = (n - o) < 256 ? (n - o) : 256;
        float u = (float)o / (float)n;
        if (w == 0) p.mix = 0.2f + 0.6f * u;
        else if (w == 1) p.size = 0.2f + 0.7f * u;
        else if (w == 2) p.damping = 0.9f * u;
        else p.width = u;
        omx_reverb_process(l + o, r + o, (uint32_t)k, &p, s, sr);
      }
      double mx = 0.0;
      for (int i = 1; i < n; i++) { double d = fabs(l[i] - l[i - 1]); if (d > mx) mx = d; }
      double own = 2.0 * 3.14159265 * 200.0 / sr * 0.5;
      char what[128];
      snprintf(what, sizeof what, "%.0f Hz: a 1 s %s ramp never steps further than the sine itself",
               (double)sr, WHICH[w]);
      check(mx <= own, what);
      rel(s); free(l); free(r);
    }
  }
}

/* Bypass is the identity, both spellings, every configuration, every rate — bit for bit, because
 * "almost the same" on a dry path is a defect you find on a null test six months later. */
static void test_bypass_is_the_identity(void) {
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int differ_off = 0, differ_mix0 = 0;
    for (int a = 0; a < 5; a++) {
      float l[1024], r[1024], l0[1024], r0[1024];
      g_rng = 7u;
      for (int i = 0; i < 1024; i++) { l[i] = l0[i] = noise(); r[i] = r0[i] = noise(); }
      struct omx_reverb_state *s = mk(sr);
      struct omx_reverb p = params(a, 0.7f, 0.3f);
      p.enabled = 0;
      run(s, &p, l, r, 1024, sr);
      for (int i = 0; i < 1024; i++) if (l[i] != l0[i] || r[i] != r0[i]) differ_off++;
      p.enabled = 1; p.mix = 0.0f;
      run(s, &p, l, r, 1024, sr);
      for (int i = 0; i < 1024; i++) if (l[i] != l0[i] || r[i] != r0[i]) differ_mix0++;
      rel(s);
    }
    char what[112];
    snprintf(what, sizeof what, "%.0f Hz: a disabled reverb is the identity on all five configurations", (double)sr);
    check(differ_off == 0, what);
    snprintf(what, sizeof what, "%.0f Hz: mix = 0 is bit-identical dry on all five configurations", (double)sr);
    check(differ_mix0 == 0, what);
  }
}

/* ---- 7. the two output stages ---------------------------------------------------------------- */

/* REVERSE's claim is that the energy a hit makes arrives AFTER it and RISES into a peak a window
 * away. The shipped swell arm measures two half-window buckets with the hit at sample 0, which is
 * hop phase 0 — and the same comparison inverts at other phases (note R-6). The phase-independent
 * statement is where the ENVELOPE PEAKS, and it is the same at every phase and every rate. */
static void test_reverse_peaks_a_window_after_the_hit_at_every_phase(void) {
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = (int)(3.0f * sr);
    for (int ph = 0; ph < 5; ph++) {
      double hit_s = 1.0 + ph * 0.030;
      int h = (int)(hit_s * sr);
      int peak[2] = { -1, -1 };
      for (int algo = 0; algo < 2; algo++) {
        float *l = calloc(n, 4), *r = calloc(n, 4);
        l[h] = 1.0f; r[h] = 1.0f;
        struct omx_reverb_state *s = mk(sr);
        struct omx_reverb p = params(algo == 0 ? OMX_REVERB_REVERSE : OMX_REVERB_ROOM, 0.7f, 0.3f);
        run(s, &p, l, r, n, sr);
        double best = 0.0;
        for (int sl = 0; sl < 20; sl++) {
          int a = h + (int)(sl * 0.05 * sr), b = h + (int)((sl + 1) * 0.05 * sr);
          double v = sqrt(energy(l, r, a, b) / (b - a));
          if (v > best) { best = v; peak[algo] = sl; }
        }
        if (algo == 0) {
          double pre = energy(l, r, 0, h);
          char what[128];
          snprintf(what, sizeof what, "%.0f Hz phase %d: reverse puts EXACTLY nothing before the hit", (double)sr, ph);
          check(pre == 0.0, what);
        }
        rel(s); free(l); free(r);
      }
      char what[144];
      snprintf(what, sizeof what, "%.0f Hz phase %d: reverse's envelope peaks >= 200 ms after the hit", (double)sr, ph);
      check(peak[0] >= 4, what);
      snprintf(what, sizeof what, "%.0f Hz phase %d: room's peaks at 50 ms, and reverse's is at least 3x later",
               (double)sr, ph);
      check(peak[1] == 1 && peak[0] >= 3 * peak[1], what);
    }
  }
}

/* GATED's envelope IS `gated / room`, sample by sample, because the gate is the last thing in the
 * wet chain. The hold and the release are declared in ms and must measure in ms at every rate;
 * the CLOSING must add no step of its own, which is what makes it click-free. */
static void test_gate_holds_and_closes_without_a_step(void) {
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = (int)(1.0f * sr);
    float *gl = calloc(n, 4), *gr = calloc(n, 4), *rl = calloc(n, 4), *rr = calloc(n, 4);
    for (int i = 0; i < (int)(0.05f * sr); i++) {
      float v = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * i / sr);
      gl[i] = rl[i] = v; gr[i] = rr[i] = v;
    }
    struct omx_reverb_state *sg = mk(sr), *sr2 = mk(sr);
    struct omx_reverb pg = params(OMX_REVERB_GATED, 0.7f, 0.3f), pr = params(OMX_REVERB_ROOM, 0.7f, 0.3f);
    run(sg, &pg, gl, gr, n, sr);
    run(sr2, &pr, rl, rr, n, sr);
    char what[128];
    double e100 = gl[(int)(0.100 * sr)] / rl[(int)(0.100 * sr)];
    double e165 = gl[(int)(0.165 * sr)] / rl[(int)(0.165 * sr)];
    double e180 = gl[(int)(0.180 * sr)] / rl[(int)(0.180 * sr)];
    double e188 = gl[(int)(0.188 * sr)] / rl[(int)(0.188 * sr)];
    snprintf(what, sizeof what, "%.0f Hz: the hold is open at 100 ms", (double)sr);
    in_band(e100, 0.999, 1.001, what);
    snprintf(what, sizeof what, "%.0f Hz: the hold is still open at 165 ms (hold 120 after a 50 ms key)", (double)sr);
    in_band(e165, 0.999, 1.001, what);
    snprintf(what, sizeof what, "%.0f Hz: halfway down the 20 ms linear release at 180 ms", (double)sr);
    in_band(e180, 0.45, 0.55, what);
    snprintf(what, sizeof what, "%.0f Hz: nearly shut at 188 ms", (double)sr);
    in_band(e188, 0.05, 0.15, what);
    int zero = 1;
    for (int i = (int)(0.195f * sr); i < n; i++) if (gl[i] != 0.0f || gr[i] != 0.0f) zero = 0;
    snprintf(what, sizeof what, "%.0f Hz: the wet is EXACTLY zero past the release, while room still rings", (double)sr);
    check(zero && fabsf(rl[(int)(0.3f * sr)]) > 0.0f, what);
    double gmax = 0.0, rmax = 0.0;
    for (int i = (int)(0.16f * sr) + 1; i < (int)(0.20f * sr); i++) {
      double d = fabs(gl[i] - gl[i - 1]); if (d > gmax) gmax = d;
      double e = fabs(rl[i] - rl[i - 1]); if (e > rmax) rmax = e;
    }
    snprintf(what, sizeof what, "%.0f Hz: closing adds no step of its own (<= room's own largest)", (double)sr);
    check(gmax <= rmax * 1.001, what);
    rel(sg); rel(sr2); free(gl); free(gr); free(rl); free(rr);
  }
}

/* R-7, ruled 2026-09-17: the ARMING is a RAMP. A one-sample arming (`gate_env` to 1.0 in a
 * single step) multiplies the wet straight back up on a hit arriving mid-release, and
 * because the signal's own step shrinks with the rate while the jump does not, the ratio grew
 * with the clock: 0.38 / 1.19 / 2.24 / 4.68 times the wet's own largest steady-state step at the
 * four rates (note R-7). The arming now carries OMX_REVERB_GATE_ATTACK_MS of linear ramp,
 * rate-derived like the release, and the law it must keep is the CLOSING's own: measured against
 * a ROOM reference fed the same input, the gate adds no step ROOM does not already carry. That
 * comparison is the honest one — a bare ratio against the tail's earlier body also counts the
 * fresh energy the second hit injects, which is the signal's doing and not the gate's. */
static void test_gate_rearm_adds_no_step_the_reference_does_not(void) {
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    int n = (int)(0.5f * sr);
    float *gl = calloc(n, 4), *gr = calloc(n, 4), *rl = calloc(n, 4), *rr = calloc(n, 4);
    int h2 = (int)(0.180f * sr);
    for (int i = 0; i < (int)(0.05f * sr); i++) {
      float v = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * i / sr);
      gl[i] = rl[i] = v; gr[i] = rr[i] = v;
    }
    for (int i = h2; i < h2 + (int)(0.02f * sr); i++) {
      float v = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (i - h2) / sr);
      gl[i] = rl[i] = v; gr[i] = rr[i] = v;
    }
    struct omx_reverb_state *sg = mk(sr), *s2 = mk(sr);
    struct omx_reverb pg = params(OMX_REVERB_GATED, 0.7f, 0.3f), pr = params(OMX_REVERB_ROOM, 0.7f, 0.3f);
    run(sg, &pg, gl, gr, n, sr);
    run(s2, &pr, rl, rr, n, sr);
    /* The window spans the arming and the whole attack ramp, plus a margin either side. */
    int a = h2 - 8, b = h2 + (int)((OMX_REVERB_GATE_ATTACK_MS * 2.0f + 1.0f) * 0.001f * sr);
    if (b > n) b = n;
    double jump = 0.0, refstep = 0.0, body = 0.0;
    for (int i = a; i < b; i++) {
      double d = fabs(gl[i] - gl[i - 1]); if (d > jump) jump = d;
      double e = fabs(rl[i] - rl[i - 1]); if (e > refstep) refstep = e;
    }
    for (int i = (int)(0.06f * sr); i < (int)(0.15f * sr); i++) {
      double d = fabs(gl[i] - gl[i - 1]); if (d > body) body = d;
    }
    printf("  R-7 %.0f Hz: re-arm step %.3g, room's own %.3g (x%.2f), against the tail's body x%.2f\n",
           (double)sr, jump, refstep, jump / refstep, jump / body);
    char what[160];
    snprintf(what, sizeof what, "%.0f Hz: R-7 the arming adds no step the ROOM reference does not carry", (double)sr);
    in_band(jump / refstep, 0.0, 1.001, what);
    rel(sg); rel(s2); free(gl); free(gr); free(rl); free(rr);
  }
}


/* ---- 8. the layout hands out SILENT memory --------------------------------------------------- */

/* R-4. `omx_reverb_state_layout` binds the pool and resets every position, damping state,
 * wet-path filter and gate — and its comment says it is "explicit for reuse". A re-layout is
 * exactly what a RATE CHANGE needs, and the lines it hands back must be silent, or the new room
 * opens playing the old one's tail: measured, before the fix, 3.22e-02 of residue energy into
 * silence at 44.1 -> 48 kHz. The clear belongs to the primitive that hands the memory out, so no
 * second caller can be the one that forgot. */
static void test_a_relayout_hands_back_silent_lines(void) {
  for (int ri = 0; ri + 1 < NRATES; ri++) {
    float from = RATE[ri], to = RATE[ri + 1];
    int n = (int)(0.2f * to);
    float *l = calloc(n, 4), *r = calloc(n, 4);
    struct omx_reverb_state *s = mk(from);
    struct omx_reverb p = params(OMX_REVERB_ROOM, 0.8f, 0.2f);
    l[0] = 1.0f; r[0] = 1.0f;
    run(s, &p, l, r, n, from);              /* build a tail at the old rate */
    omx_reverb_state_layout(s, s->_pool, s->pool_len, to); /* the rate moved */
    memset(l, 0, (size_t)n * 4); memset(r, 0, (size_t)n * 4);
    run(s, &p, l, r, n, to);                /* silence in */
    char what[144];
    snprintf(what, sizeof what, "%.0f -> %.0f Hz: a re-layout leaves not one sample of the old tail",
             (double)from, (double)to);
    check(energy(l, r, 0, n) == 0.0, what);
    rel(s); free(l); free(r);
  }
}

/* ---- 14. the declared seeds, measured (spec 2026-07-15 SSD, arms A and B) ------------------- */

/* The stage exactly as a seed sets it: every field from the GENERATED header, `mix` wet-only so
 * the tail is the measurement and not the dry. */
static struct omx_reverb seeded(const struct omx_reverb_preset_seed *d) {
  struct omx_reverb p = params(d->algorithm, d->size, d->damping);
  p.predelay_ms = d->predelay_ms; p.width = d->width; p.lowcut = d->lowcut; p.highcut = d->highcut;
  p.reverse_ms = d->reverse_ms; p.hold_ms = d->hold_ms; p.release_ms = d->release_ms;
  p.gate_threshold_db = d->gate_threshold_db;
  return p;
}
static const struct omx_reverb_preset_seed *seed_by_id(const char *id) {
  for (int i = 0; i < OMX_REVERB_PRESET_SEED_COUNT; i++)
    if (strcmp(OMX_REVERB_PRESET_SEEDS[i].id, id) == 0) return &OMX_REVERB_PRESET_SEEDS[i];
  return NULL;
}
/* The library's 24 dB/oct Butterworth low-pass (its Qs, its section design, its cascade) at
 * `hz`, in place — the DC band a comb's unity-DC damping cannot touch. */
static void butter4_lowpass(float *x, int n, float sr, double hz) {
  double q[2];
  uint32_t ns = omx_eq_butterworth_qs(1, q);
  float c[2][5], st[2][4];
  memset(st, 0, sizeof st);
  for (uint32_t i = 0; i < ns; i++) omx_eq_design_f(OMX_EQ_LOWPASS, hz, q[i], 0.0, sr, c[i]);
  omx_biquad_cascade(x, (uint32_t)n, ns, (const float (*)[5])c, NULL, st);
}
/* T30 by Schroeder: the backward-integrated energy of both legs, the least-squares slope between
 * -5 and -35 dB, extrapolated to 60 dB. Returns milliseconds, or -1 if the curve never falls 35 dB. */
static double t30_ms(const float *l, const float *r, int n, float sr) {
  double *edc = malloc((size_t)n * sizeof *edc), acc = 0.0;
  for (int i = n - 1; i >= 0; i--) { acc += (double)l[i] * l[i] + (double)r[i] * r[i]; edc[i] = acc; }
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  int m = 0, fell = 0;
  for (int i = 0; i < n; i++) {
    double db = 10.0 * log10(edc[i] / edc[0] + 1e-300);
    if (db > -5.0) continue;
    if (db < -35.0) { fell = 1; break; }
    double t = (double)i / sr;
    sx += t; sy += db; sxx += t * t; sxy += t * db; m++;
  }
  free(edc);
  if (!fell || m < 16) return -1.0;
  double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
  return slope < 0.0 ? -60.0 / slope * 1000.0 : -1.0;
}

/* Arm A: every ROOM/HALL/PLATE seed's measured DC T30 is its closed-form RT60_DC (the longest
 * line's, declared_rt60_ms) at every declared rate. The impulse is rendered 1.25 RT60 long, so the
 * truncated Schroeder integral is 75 dB down and cannot bend the -35 dB end of the fit. */
static void test_every_seed_decays_at_its_closed_form(void) {
  for (int k = 0; k < OMX_REVERB_PRESET_SEED_COUNT; k++) {
    const struct omx_reverb_preset_seed *d = &OMX_REVERB_PRESET_SEEDS[k];
    if (d->algorithm != OMX_REVERB_ROOM && d->algorithm != OMX_REVERB_HALL && d->algorithm != OMX_REVERB_PLATE)
      continue;
    double ratio[NRATES];
    for (int ri = 0; ri < NRATES; ri++) {
      float sr = RATE[ri];
      struct omx_reverb_state *s = mk(sr);
      double form = declared_rt60_ms(s, d->algorithm, d->size, sr);
      int n = (int)((1.25 * form * 0.001 + 0.1) * sr);
      float *l = calloc((size_t)n, 4), *r = calloc((size_t)n, 4);
      l[0] = 1.0f; r[0] = 1.0f;
      struct omx_reverb p = seeded(d);
      run(s, &p, l, r, n, sr);
      butter4_lowpass(l, n, sr, 200.0);
      butter4_lowpass(r, n, sr, 200.0);
      double meas = t30_ms(l, r, n, sr);
      ratio[ri] = meas / form;
      printf("  arm A %-12s %6.0f Hz: T30 %8.1f ms, RT60_DC %8.1f ms, ratio %.4f\n", d->id, (double)sr, meas, form, ratio[ri]);
      char what[128];
      snprintf(what, sizeof what, "%.0f Hz %s: DC T30 against its closed-form RT60_DC", (double)sr, d->id);
      if (d->algorithm == OMX_REVERB_ROOM) in_band(ratio[ri], 0.92, 1.13, what);
      else if (d->algorithm == OMX_REVERB_HALL) in_band(ratio[ri], 0.87, 0.91, what);
      else in_band(ratio[ri], 1.37, 1.48, what);
      free(l); free(r); rel(s);
    }
    double lo = ratio[0], hi = ratio[0];
    for (int ri = 1; ri < NRATES; ri++) { if (ratio[ri] < lo) lo = ratio[ri]; if (ratio[ri] > hi) hi = ratio[ri]; }
    char what[128];
    snprintf(what, sizeof what, "%s: the same DC decay at 44.1, 48, 96 and 192 kHz", d->id);
    in_band(hi / lo, 1.0, 1.01, what);
  }
}

/* Mean power of `x[a..b)` over 41 bins spaced 10 Hz about `hz`: a band, because one DTFT bin of a
 * noise-like tail is a sample of a random variable and not its level. */
static double band_power(const float *x, int a, int b, float sr, double hz) {
  double sum = 0.0;
  for (int j = -20; j <= 20; j++) {
    double w = 2.0 * M_PI * (hz + 10.0 * j) / sr, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0;
    for (int i = a; i < b; i++) { double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    sum += s1 * s1 + s2 * s2 - c * s1 * s2;
  }
  return sum / 41.0;
}
static double tail_tilt_db(const struct omx_reverb_preset_seed *d, float sr, double f_hi) {
  struct omx_reverb_state *s = mk(sr);
  int n = (int)(1.2f * sr), a = (int)(0.1f * sr);
  float *l = calloc((size_t)n, 4), *r = calloc((size_t)n, 4);
  l[0] = 1.0f; r[0] = 1.0f;
  struct omx_reverb p = seeded(d);
  run(s, &p, l, r, n, sr);
  double tilt = 10.0 * log10((band_power(l, a, n, sr, f_hi) + band_power(r, a, n, sr, f_hi)) /
                             (band_power(l, a, n, sr, 1000.0) + band_power(r, a, n, sr, 1000.0)));
  free(l); free(r); rel(s);
  return tilt;
}
/* Arm B: vintagePlate's tail is band-limited at least as hard as its highcut pole alone would
 * make it (the tank can only add loss), and ambience's tail is brighter than smallRoom's. */
static void test_the_family_seeds_hold_their_band(void) {
  const struct omx_reverb_preset_seed *vp = seed_by_id("vintagePlate");
  const struct omx_reverb_preset_seed *amb = seed_by_id("ambience");
  const struct omx_reverb_preset_seed *sm = seed_by_id("smallRoom");
  check(vp && amb && sm, "the family seeds are in the generated header");
  if (!vp || !amb || !sm) return;
  for (int ri = 0; ri < NRATES; ri++) {
    float sr = RATE[ri];
    double pole = (double)omx_pole_from_cutoff_hz(vp->highcut, sr);
    double h16 = (1.0 - pole) / cabs(1.0 - pole * cexp(-I * 2.0 * M_PI * 16000.0 / sr));
    double h1 = (1.0 - pole) / cabs(1.0 - pole * cexp(-I * 2.0 * M_PI * 1000.0 / sr));
    double bound = 20.0 * log10(h16 / h1);
    double vtilt = tail_tilt_db(vp, sr, 16000.0);
    double atilt = tail_tilt_db(amb, sr, 12000.0), stilt = tail_tilt_db(sm, sr, 12000.0);
    printf("  arm B %6.0f Hz: vintagePlate 16k/1k %.2f dB <= highcut %.2f dB (margin %.2f); "
           "ambience 12k/1k %.2f dB >= smallRoom %.2f dB (margin %.2f)\n",
           (double)sr, vtilt, bound, bound - vtilt, atilt, stilt, atilt - stilt);
    char what[128];
    snprintf(what, sizeof what, "%.0f Hz: vintagePlate's tail at 16 kHz is at or below its highcut's own loss", (double)sr);
    check(vtilt <= bound, what);
    snprintf(what, sizeof what, "%.0f Hz: ambience's tail is brighter at 12 kHz than smallRoom's", (double)sr);
    check(atilt >= stilt, what);
  }
}

/*
 * Every arm above ran with -DOMX_CONTRACTS, so every OMX_PRE / OMX_POST on the reverb path
 * evaluated on real audio. An empty ledger is the claim; a non-zero `checks` is what keeps the
 * empty ledger from being the false signal of a battery that never reached a contract.
 */
static void test_the_contract_ledger_came_out_empty(void) {
#ifdef OMX_CONTRACTS
  check(omx_contract_log.checks > 0u, "the contracts were REACHED (a zero ledger proves nothing "
                                      "unless something evaluated)");
  if (omx_contract_log.count != 0u) {
    for (uint32_t i = 0; i < omx_contract_log.count && i < OMX_CONTRACT_MAX; i++)
      fprintf(stderr, "  contract: %s/%s (%s) at frame %u\n", omx_contract_log.rec[i].stage,
              omx_contract_log.rec[i].token, omx_contract_log.rec[i].kind,
              omx_contract_log.rec[i].frame);
  }
  check(omx_contract_log.count == 0u, "no contract on the reverb path was violated");
  printf("fx/reverb_math: %u contracts evaluated, %u violated\n", omx_contract_log.checks,
         omx_contract_log.count);
#endif
}

int main(void) {
  omx_fx_require_rate_floor();
  test_line_geometry_is_rate_invariant();
  test_rt60_matches_its_declared_form_at_every_rate();
  test_the_reference_rate_response_is_unchanged();
  test_damped_decay_matches_the_reference_rate_within_1_percent();
  test_late_tail_periodicity();
  test_plate_mod_depth_is_a_field();
  test_no_dc_build_up_over_60_s();
  test_mono_compatible();
  test_parameter_ramps_do_not_zipper();
  test_bypass_is_the_identity();
  test_reverse_peaks_a_window_after_the_hit_at_every_phase();
  test_gate_holds_and_closes_without_a_step();
  test_gate_rearm_adds_no_step_the_reference_does_not();
  test_a_relayout_hands_back_silent_lines();
  test_every_seed_decays_at_its_closed_form();
  test_the_family_seeds_hold_their_band();
  test_the_contract_ledger_came_out_empty();
  printf("fx/reverb_math: %d checks, %d failures (44100/48000/96000/192000)\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
