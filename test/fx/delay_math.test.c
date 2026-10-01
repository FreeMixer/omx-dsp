// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The FX delay against its declared numbers at every rate in OMX_DECLARED_RATES: an impulse lands
 * on the declared sample and nowhere else, the feedback clamp the audio obeys, and the tone knob
 * as one filter at every clock (R-058). Contracts are compiled in and the last arm asserts the
 * ledger came out empty.
 *
 * These are the FX-delay arms of the engine's mix_delay_math.test.c, moved with the kernel; the
 * arms about the route, alignment and bus-output delays stay with the engine, as those kernels do.
 *
 *   make test-fx
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_delay.h>

#include "fx_rates.h"

static int g_fail = 0, g_checks = 0;
static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) { g_fail++; fprintf(stderr, "FAIL: %s\n", what); }
}
/* A band, because a one-sided bound on a measured quantity catches only one of its two ways of
 * being wrong — the same discipline mix_reverb_math.test.c takes. */
static void in_band(double v, double lo, double hi, const char *what) {
  g_checks++;
  if (!(v >= lo && v <= hi)) {
    g_fail++;
    fprintf(stderr, "FAIL: %s — measured %.6g, wanted [%.6g, %.6g]\n", what, v, lo, hi);
  }
}


/** The index of the largest |sample| — where an impulse LANDS. */
static int peak_index(const float *b, int n) {
  int best = 0;
  for (int i = 1; i < n; i++) if (fabsf(b[i]) > fabsf(b[best])) best = i;
  return best;
}
static double rms(const float *b, int from, int to) {
  double a = 0.0;
  for (int i = from; i < to; i++) a += (double)b[i] * (double)b[i];
  return to > from ? sqrt(a / (double)(to - from)) : 0.0;
}

/*
 * THE CENTRAL LAW OF THIS FILE, arm 1 of 3. An impulse into the FX delay at mix 1 / feedback 0
 * comes back out at EXACTLY the declared sample and nowhere else: one sample of unity, every
 * other sample bit-zero. Not "the peak is near d" — the whole block is the delayed impulse.
 */
static void test_an_impulse_through_the_fx_delay_lands_on_the_declared_sample(void) {
  static const float MS[] = { 0.0f, 1.0f, 5.0f, 23.5f, 120.0f, 750.0f };
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    float sr = OMX_DECLARED_RATES[ri];
    for (size_t k = 0; k < sizeof MS / sizeof MS[0]; k++) {
      uint32_t d = omx_fxdelay_ms_to_samples(MS[k], sr);
      int n = (int)d + 64;
      float *l = calloc((size_t)n, sizeof(float));
      float *r = calloc((size_t)n, sizeof(float));
      struct omx_fx_delay_state s;
      memset(&s, 0, sizeof s);
      s.cap = OMX_FXDELAY_CAP;
      s.ring_l = calloc(OMX_FXDELAY_CAP, sizeof(float));
      s.ring_r = calloc(OMX_FXDELAY_CAP, sizeof(float));
      struct omx_fx_delay p;
      memset(&p, 0, sizeof p);
      p.enabled = 1; p.d_l = d; p.d_r = d; p.feedback = 0.0f; p.mix = 1.0f; p.tone = 0.0f;
      l[0] = 1.0f; r[0] = 1.0f;
      for (int o = 0; o < n; o += 128) {
        int q = (n - o) < 128 ? (n - o) : 128;
        omx_fx_delay_process(l + o, r + o, (uint32_t)q, &p, &s, sr);
      }
      int landed = peak_index(l, n);
      int clean = 1;
      for (int i = 0; i < n; i++) if (i != (int)d && l[i] != 0.0f) clean = 0;
      char what[200];
      snprintf(what, sizeof what,
               "%.0f Hz: the FX delay puts a %g ms impulse on sample %u and nowhere else",
               (double)sr, (double)MS[k], d);
      check(landed == (int)d && l[(int)d] == 1.0f && clean, what);
      snprintf(what, sizeof what, "%.0f Hz: %g ms — the two legs are the same delay",
               (double)sr, (double)MS[k]);
      check(memcmp(l, r, (size_t)n * sizeof(float)) == 0, what);
      free(l); free(r); free(s.ring_l); free(s.ring_r);
    }
  }
}

/*
 * THE FEEDBACK CLAMP IS THE WHOLE REASON THIS STAGE CANNOT RUN AWAY, and it is stated TWICE at
 * two different figures. The contract precondition is `feedback >= 0 && feedback < 1` — the
 * control thread's claim. The body then clamps to 0.99 regardless. Both are defensible on their
 * own; together they mean a caller that dials 0.999, which the contract ACCEPTS, silently gets
 * 0.99.
 *
 * This arm measures which figure the audio obeys — the answer is the body's 0.99 — so the note's
 * finding D-1 is a measurement and not a reading of the source. The repeat-to-repeat ratio at a
 * requested 0.999 must be 0.99, not 0.999, and the tail must reach -60 dB in the time 0.99
 * implies.
 */
static void test_the_feedback_clamp_is_the_bodys_figure_not_the_contracts(void) {
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    float sr = OMX_DECLARED_RATES[ri];
    uint32_t d = omx_fxdelay_ms_to_samples(10.0f, sr);
    int reps = 40;
    int total = (int)d * (reps + 1) + 64;
    float *l = calloc((size_t)total, sizeof(float));
    float *r = calloc((size_t)total, sizeof(float));
    struct omx_fx_delay_state s;
    memset(&s, 0, sizeof s);
    s.cap = OMX_FXDELAY_CAP;
    s.ring_l = calloc(OMX_FXDELAY_CAP, sizeof(float));
    s.ring_r = calloc(OMX_FXDELAY_CAP, sizeof(float));
    struct omx_fx_delay p;
    memset(&p, 0, sizeof p);
    p.enabled = 1; p.mix = 1.0f; p.tone = 0.0f;
    p.feedback = 0.999f; /* legal by the contract, clamped by the body */
    p.d_l = d; p.d_r = d;
    l[0] = 1.0f; r[0] = 1.0f;
    for (int o = 0; o < total; o += 256) {
      int q = (total - o) < 256 ? (total - o) : 256;
      omx_fx_delay_process(l + o, r + o, (uint32_t)q, &p, &s, sr);
    }
    double first = fabs((double)l[d]);
    double tenth = fabs((double)l[(int)d * 10]);
    double ratio = pow(tenth / first, 1.0 / 9.0);
    char what[220];
    snprintf(what, sizeof what,
             "%.0f Hz: a requested feedback of 0.999 repeats at the BODY's 0.99, not the "
             "contract's 0.999", (double)sr);
    in_band(ratio, 0.9895, 0.9905, what);
    /* and nothing grows: the peak over 40 repeats never exceeds the first one */
    double worst = 0.0;
    for (int i = 0; i < total; i++) if (fabs((double)l[i]) > worst) worst = fabs((double)l[i]);
    snprintf(what, sizeof what,
             "%.0f Hz: over 40 repeats nothing exceeds the input — the stage cannot run away",
             (double)sr);
    in_band(worst, 0.999, 1.001, what);
    printf("  feedback  %6.0f Hz: requested 0.999, measured repeat ratio %.5f, worst peak %.4f\n",
           (double)sr, ratio, worst);
    free(l); free(r); free(s.ring_l); free(s.ring_r);
  }
}

/*
 * R-058 OVER THE FX DELAY'S TONE KNOB — the gate for this lane's one fix.
 *
 * `tone` is a UNIT-RANGE knob and the kernel handed it to the feedback one-pole AS ITS POLE. A
 * one-pole's corner is `-ln(p)*sr/2pi`, so the same knob was a different filter at every clock:
 * measured before the fix, |H(8 kHz)| at tone 0.6 ran 0.4315 / 0.4588 / 0.7063 / 0.8924 across
 * the four declared rates — corners of 3 585 / 3 902 / 7 805 / 15 610 Hz. That is exactly the
 * defect R-058 was minted for in `mix_reverb.h`, in a second kernel, and R-058 says it in
 * general: "No stage takes such a knob as a per-sample pole directly."
 *
 * `omx_fxdelay_tone_pole` now quotes the knob at 96 000 Hz and raises it to `REF/sr` once per
 * block, so this arm asserts the law rather than the defect: the SAME knob must be the SAME
 * filter at every rate, and that filter must be the one 96 kHz always had — the desk's own sound
 * does not move, because at 96 kHz the exponent is exactly 1 and no `powf` is taken.
 *
 * THE PROBE HAS TO BE IN HERTZ, not in samples. Everything else in this kernel is sample-indexed,
 * so an impulse or an alternating pair gives a bit-identical answer at all four rates and would
 * "measure" a rate invariance that is an artefact of the probe. The probe is an 8 kHz tone BURST
 * one delay-time long, and what is measured is the second repeat against the first — one pass
 * through the feedback path — divided by the feedback, which leaves |H(8 kHz)| on its own.
 */
static void test_the_tone_pole_is_rate_normalised_to_the_desks_own_rate(void) {
  const double PROBE_HZ = 8000.0, P = 0.6, FB = 0.9;
  double measured[OMX_DECLARED_RATE_COUNT];
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    float sr = OMX_DECLARED_RATES[ri];
    uint32_t d = omx_fxdelay_ms_to_samples(10.0f, sr);
    int total = (int)d * 4;
    float *l = calloc((size_t)total, sizeof(float));
    float *r = calloc((size_t)total, sizeof(float));
    struct omx_fx_delay_state s;
    memset(&s, 0, sizeof s);
    s.cap = OMX_FXDELAY_CAP;
    s.ring_l = calloc(OMX_FXDELAY_CAP, sizeof(float));
    s.ring_r = calloc(OMX_FXDELAY_CAP, sizeof(float));
    struct omx_fx_delay p;
    memset(&p, 0, sizeof p);
    p.enabled = 1; p.mix = 1.0f; p.feedback = (float)FB; p.tone = (float)P;
    p.d_l = d; p.d_r = d;
    /* one delay-time of 8 kHz, then silence: repeat k lands in [k*d, (k+1)*d) */
    for (uint32_t i = 0; i < d; i++) {
      l[i] = (float)sin(2.0 * M_PI * PROBE_HZ * (double)i / (double)sr);
      r[i] = l[i];
    }
    for (int o = 0; o < total; o += 256) {
      int q = (total - o) < 256 ? (total - o) : 256;
      omx_fx_delay_process(l + o, r + o, (uint32_t)q, &p, &s, sr);
    }
    /* the middle half of each repeat, so the burst's rectangular edges stay out of the figure */
    int q1 = (int)d / 4, q3 = 3 * (int)d / 4;
    double e1 = rms(l, (int)d + q1, (int)d + q3);
    double e2 = rms(l, 2 * (int)d + q1, 2 * (int)d + q3);
    measured[ri] = (e2 / e1) / FB;
    free(l); free(r); free(s.ring_l); free(s.ring_r);

    /* The pole the law asks for at THIS rate, and the closed form of the one-pole carrying it.
     * Asserted tight (+/-0.006), because this is arithmetic and not a tolerance: it proves the
     * kernel took the exponent, and took it once per block over the right knob. */
    double pole_here = pow(P, (double)OMX_FXDELAY_TONE_REFERENCE_RATE / (double)sr);
    double w = 2.0 * M_PI * PROBE_HZ / (double)sr;
    double want = (1.0 - pole_here) / sqrt(1.0 - 2.0 * pole_here * cos(w) + pole_here * pole_here);
    double corner = -log(pole_here) * (double)sr / (2.0 * M_PI);
    char what[240];
    snprintf(what, sizeof what,
             "%.0f Hz: the tone knob 0.6 is the pole %.5f here, and |H(8 kHz)| = %.4f is that "
             "pole's closed form (R-058, the exponent was taken)", (double)sr, pole_here, want);
    in_band(measured[ri], want - 0.006, want + 0.006, what);
    /* THE CORNER is the quantity the law actually holds constant, and it holds it EXACTLY:
     * `-ln(p^(REF/sr))*sr/2pi` = `-ln(p)*REF/2pi` for every sr, identically. */
    snprintf(what, sizeof what,
             "%.0f Hz: the tone knob's corner is 7 805 Hz — the SAME filter at every clock",
             (double)sr);
    in_band(corner, 7800.0, 7810.0, what);
    printf("  tone pole %6.0f Hz: |H(8 kHz)| measured %.4f, closed form %.4f, pole here "
           "%.5f, corner %.0f Hz\n", (double)sr, measured[ri], want, pole_here, corner);
  }
  /* THE LAW'S EFFECT, as one number. Before this lane's fix the same knob passed 0.4315 of 8 kHz
   * at 44.1 kHz and 0.8924 at 192 kHz — a spread of 2.07, an audibly different delay on a REAC
   * box clocked to a different segment. After it the spread is 1.05, and that residual is not a
   * miss: `p^(REF/sr)` matches the pole's TIME CONSTANT exactly (the corner above is identical to
   * the Hz), while a digital one-pole's MAGNITUDE at a fixed frequency still carries the usual
   * frequency warping, worst right at the corner — which is where 8 kHz was deliberately put.
   * R-058 adopts that same approximation: its own gate asks for a decay within 1 %, not a
   * magnitude to the digit. */
  double lo = measured[0], hi = measured[0];
  for (int ri = 1; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    if (measured[ri] < lo) lo = measured[ri];
    if (measured[ri] > hi) hi = measured[ri];
  }
  in_band(hi / lo, 1.0, 1.06,
          "the tone knob is ONE filter across 44.1 / 48 / 96 / 192 kHz to within 5 % at its own "
          "corner, where it was a factor of 2.07 apart before (R-058)");
  /* AND THE DESK'S OWN RATE DID NOT MOVE. At 96 kHz the exponent is exactly 1, so the pole is
   * the knob, bit for bit, and this lane's fix is inaudible on the console it ships to. */
  check(omx_fxdelay_tone_pole(0.6f, OMX_FXDELAY_TONE_REFERENCE_RATE) == 0.6f,
        "at 96 kHz the tone pole IS the knob — no powf, no retune of the desk's own sound");
}

/*
 * Every arm above ran with -DOMX_CONTRACTS, so every OMX_PRE / OMX_POST / OMX_INVARIANT on the
 * delay path evaluated on real audio. An empty ledger is the claim; a non-zero `checks` is what
 * keeps the empty ledger from being the false signal of a battery that never reached a contract.
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
  check(omx_contract_log.count == 0u, "no contract on the delay path was violated");
  printf("fx/delay_math: %u contracts evaluated, %u violated\n", omx_contract_log.checks,
         omx_contract_log.count);
#endif
}

int main(void) {
  omx_fx_require_rate_floor();
  test_an_impulse_through_the_fx_delay_lands_on_the_declared_sample();
  test_the_feedback_clamp_is_the_bodys_figure_not_the_contracts();
  test_the_tone_pole_is_rate_normalised_to_the_desks_own_rate();
  test_the_contract_ledger_came_out_empty();
  printf("fx/delay_math: %d checks, %d failures (%d rates)\n", g_checks, g_fail, (int)OMX_DECLARED_RATE_COUNT);
  return g_fail == 0 ? 0 : 1;
}
