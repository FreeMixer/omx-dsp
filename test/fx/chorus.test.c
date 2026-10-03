// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * chorus.test.c — the chorus against the closed form of its own swept comb.
 *
 * Spec: docs/design/specs/2026-09-22-chorus-and-flanger.md §2 and §9, at the four basic rates
 * (44.1 / 48 / 96 / 192 kHz — the operator's 2026-09-20 ruling, memory
 * `feedback_dsp_oracles_measure_four_rates_minimum`).
 *
 * ## How a moving filter is measured without a moving ruler
 *
 * A chorus is LINEAR and, while its oscillator is FROZEN (`inc = 0`), time-invariant. So at each
 * LFO phase the stage has an exact transfer function, and it is read out of an IMPULSE RESPONSE:
 * one run per phase, then a DFT at any frequency — no window, no leakage, no bin grid. The
 * oscillator is then let go for arm D, which is the only arm that needs it to move.
 *
 * The closed form is the spec's:  H(w) = (1 − mix) + (mix/N)·Σ_k R(w, d_k),  with R the LINE's
 * own response (Newton divided differences, `test-support/fdelay_oracle.h`) and NOT the ideal
 * e^{-jwd} — so no arm compares the kernel with itself, and none of them is loosened to absorb
 * the interpolator's own 0.5 dB at 20 kHz.
 *
 * ## The arms
 *
 *   A — THE DELAY FOLLOWS THE LFO. The wet-only impulse response's CENTROID is the delay the
 *       stage is reading at (an order-N Lagrange kernel reproduces a linear function exactly, so
 *       its coefficients' first moment IS the read point). Sixteen phases across the turn, four
 *       rates, against `base + depth·(1 + s(φ))/2`. A positive control requires the same measure
 *       to REJECT a prediction half a sample away, so "it follows" is not a claim a blunt ruler
 *       would also make.
 *   B — THE SWEPT COMB. |H(f)| across the band against the closed form, complex, at eight phases
 *       and four rates; then the NOTCH FREQUENCY ITSELF, found by scanning the measured response
 *       for its minimum and compared against (2m+1)/(2d(φ)) — the notch MOVES with the phase, and
 *       the arm measures where it moved to rather than that it is deep where it was predicted.
 *   C — THE VOICES. N = 1..4: the same complex comparison, which is what proves the sum is at
 *       1/N and the offsets are k/N turns; plus the ensemble's delays read a third (a quarter) of
 *       a turn apart, and unity at DC for every N.
 *   D — AGAINST TIME. The oscillator RUNNING: a 1 kHz tone, a short-window Goertzel envelope, and
 *       the closed form at each window's own phase. The positive control is the same comparison
 *       against a quarter-turn-shifted phase, which must fail.
 *   E — NO CLICK, at the four rates, the fdelay arm C derivation: the tone's own step plus the
 *       interpolator's error, with a NEAREST-SAMPLE modulation as the control that must break it.
 *   F — BYPASS IDENTITY. `enabled = 0` and `mix = 0`, memcmp-identical, over a block that carries
 *       words below the flush floor.
 *   G — NO GAIN ADDED, and no denormal state: a full-scale input at every voice count never
 *       leaves unity, and silence after a tone reaches exact zero.
 *   H — THE GOLDEN IDENTITY of the cost lane (lane/kernel-cost-chorus): `omx_chorus_process` is
 *       memcmp-identical, output AND state, to the kernel as it stood before the lane
 *       (`chorus_ref_process` below, the pre-lane body verbatim: two `omx_fdelay_read`s per voice,
 *       the offset divided per sample) at four rates, N = 1..4, three depths, three LFO rates,
 *       block sizes that cross every boundary, distinct legs. The positive control flips ONE
 *       input LSB and requires the comparison to see it.
 *
 * Pure C, `-lm`, no PipeWire: safe beside the live rig. Built with -DOMX_CONTRACTS.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <complex.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/fx/omx_chorus.h>
#include "fdelay_oracle.h"

#include "fx_rates.h"

/* ---- the harness ------------------------------------------------------------------------- */

static int g_checks = 0, g_failed = 0;
static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s — measured %.9g, limit %.9g\n", what, measured, limit);
  }
}

static void drain_violations(const char *where) {
  uint32_t seen = omx_contract_log.count;
  uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++)
    printf("VIOLATION [%s] %s %s (frame %u)\n", omx_contract_log.rec[i].stage,
           omx_contract_log.rec[i].kind, omx_contract_log.rec[i].token,
           omx_contract_log.rec[i].frame);
  ok(seen == 0u, where, (double)seen, 0.0);
  omx_contract_log.count = 0u;
}

static const double RATES[4] = {44100.0, 48000.0, 96000.0, 192000.0};

/* The rings, once: the stage allocates nothing, and neither does this file per arm. */
static float g_ring_l[OMX_CHORUS_CAP], g_ring_r[OMX_CHORUS_CAP];

#define IRLEN 4096u
static float g_l[IRLEN], g_r[IRLEN];

/** A control atom in the row's own units, converted at `sr` the way the resolve will. */
static struct omx_chorus atom(double sr, double base_ms, double depth_ms, int voices, double mix) {
  struct omx_chorus p;
  p.enabled = 1;
  p.voices = voices;
  p.base_samples = (float)(base_ms * sr / 1000.0);
  p.depth_samples = (float)(depth_ms * sr / 1000.0);
  p.lfo_inc = 0.0f; /* FROZEN by default: every arm but D measures an LTI stage */
  p.mix = (float)mix;
  p.spread = 0.0f; /* both legs one phase: every arm but H measures the stage without it */
  return p;
}

/** Arm a state over the shared rings, with the oscillator FROZEN at `phase`. */
static void freeze(struct omx_chorus_state *s, double phase) {
  memset(g_ring_l, 0, sizeof g_ring_l);
  memset(g_ring_r, 0, sizeof g_ring_r);
  enum omx_fdelay_code c = omx_chorus_state_init(s, g_ring_l, g_ring_r, OMX_CHORUS_CAP);
  ok(c == OMX_FDELAY_OK, "the chorus arms over its rings", (double)c, 0.0);
  s->lfo.phase = (float)phase;
  s->lfo.inc = 0.0f;
}

/** One impulse response of the frozen stage, into `g_l`. */
static void impulse_response(const struct omx_chorus *p, double phase) {
  struct omx_chorus_state s;
  freeze(&s, phase);
  memset(g_l, 0, sizeof g_l);
  memset(g_r, 0, sizeof g_r);
  g_l[0] = 1.0f;
  g_r[0] = 1.0f;
  omx_chorus_process(g_l, g_r, IRLEN, p, &s);
}

/** The closed form of §2 at angular frequency `w`, for a stage frozen at `phase`. */
static double complex closed_form(const struct omx_chorus *p, double phase, double w) {
  struct omx_lfo lfo = {(float)phase, 0.0f, 0.0f};
  double complex wet = 0.0;
  for (int k = 0; k < p->voices; k++) {
    const double d = (double)omx_chorus_voice_delay(p, &lfo, k, p->voices);
    wet += omx_oracle_fdelay(OMX_CHORUS_ORDER, d, w);
  }
  return (1.0 - (double)p->mix) + ((double)p->mix / (double)p->voices) * wet;
}

/** The delay voice 0 reads at `phase` — the spec's `base + depth·(1 + s(φ))/2`, in samples. */
static double voice0_delay(const struct omx_chorus *p, double phase) {
  struct omx_lfo lfo = {(float)phase, 0.0f, 0.0f};
  return (double)omx_chorus_voice_delay(p, &lfo, 0, p->voices);
}

/* ---- arm A: the delay follows the LFO ------------------------------------------------------ */

static void arm_a_delay_follows(void) {
  double worst = 0.0, worst_control = 1e9;
  for (int ri = 0; ri < 4; ri++) {
    const double sr = RATES[ri];
    /* The vocal template of spec §3a, wet only so the centroid is the WET path's alone. */
    const struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, 4.0, 1, 1.0);
    for (int i = 0; i < 16; i++) {
      const double phase = (double)i / 16.0;
      impulse_response(&p, phase);
      double num = 0.0, den = 0.0;
      for (uint32_t k = 0; k < IRLEN; k++) {
        num += (double)k * (double)g_l[k];
        den += (double)g_l[k];
      }
      const double centroid = num / den;
      const double want = voice0_delay(&p, phase);
      const double e = fabs(centroid - want);
      if (e > worst) worst = e;
      /* THE POSITIVE CONTROL: the same ruler against a delay half a sample away must MISS. */
      const double c = fabs(centroid - (want + 0.5));
      if (c < worst_control) worst_control = c;
      ok(fabs(den - 1.0) <= 1e-5, "arm A: the wet path's impulse response sums to unity", den, 1.0);
    }
    printf("mix_chorus arm A, %.1f kHz: 16 phases, worst centroid error %.2e samples (delay "
           "%.1f..%.1f)\n",
           sr / 1000.0, worst, (double)p.base_samples,
           (double)(p.base_samples + p.depth_samples));
  }
  ok(worst <= 2e-3, "arm A: the read delay IS the LFO's sweep, at every phase and rate", worst,
     2e-3);
  ok(worst_control >= 0.4,
     "arm A control: the same measure rejects a delay half a sample away", worst_control, 0.4);
  drain_violations("arm A: no contract violation");
}

/* ---- arm B: the swept comb, and where its notches are -------------------------------------- */

static void arm_b_swept_comb(void) {
  double worst = 0.0, worst_notch_rel = 0.0;
  for (int ri = 0; ri < 4; ri++) {
    const double sr = RATES[ri];
    const struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, 4.0, 1, 0.5);
    for (int i = 0; i < 8; i++) {
      const double phase = (double)i / 8.0;
      impulse_response(&p, phase);
      /* The complex response, against the closed form, across the band. */
      static const double TONES[7] = {100.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 18000.0};
      for (int t = 0; t < 7; t++) {
        if (TONES[t] * 2.0 >= sr) continue; /* nothing above Nyquist is a tone */
        const double w = 2.0 * M_PI * TONES[t] / sr;
        const double complex got = omx_oracle_dft(g_l, IRLEN, w);
        const double complex want = closed_form(&p, phase, w);
        const double e = cabs(got - want);
        if (e > worst) worst = e;
      }
      /* THE NOTCH FREQUENCY, MEASURED. The comb's m-th notch is predicted at (2m+1)/(2d); the
       * scan finds where the response actually falls to its minimum, and the two are compared as
       * a RELATIVE frequency error. m is chosen so the notch sits around 2 kHz, where the scan
       * has resolution to spare and the interpolator's own loss is still negligible. */
      const double d = voice0_delay(&p, phase) / sr; /* seconds */
      const long m = (long)(2000.0 * d - 0.5);
      const double predicted = (double)(2 * m + 1) / (2.0 * d);
      double best_f = 0.0, best_mag = 1e9;
      for (int k = -200; k <= 200; k++) {
        const double f = predicted * (1.0 + 0.002 * (double)k / 2.0); /* ±20%, 0.1% steps */
        const double mag = cabs(omx_oracle_dft(g_l, IRLEN, 2.0 * M_PI * f / sr));
        if (mag < best_mag) {
          best_mag = mag;
          best_f = f;
        }
      }
      const double rel = fabs(best_f - predicted) / predicted;
      if (rel > worst_notch_rel) worst_notch_rel = rel;
      if (i == 2 || i == 6)
        printf("mix_chorus arm B, %.1f kHz, phase %.3f: d %.3f ms, comb spacing %.1f Hz (first "
               "notch %.1f Hz), notch #%ld measured %.1f Hz vs predicted %.1f Hz (%.3f%%), depth "
               "%.1f dB\n",
               sr / 1000.0, phase, d * 1000.0, 1.0 / d, 1.0 / (2.0 * d), m, best_f, predicted,
               100.0 * rel, 20.0 * log10(best_mag < 1e-12 ? 1e-12 : best_mag));
    }
  }
  ok(worst <= 2e-4, "arm B: the comb IS the closed form, complex, at every phase and rate", worst,
     2e-4);
  ok(worst_notch_rel <= 0.004,
     "arm B: and the measured NOTCH sits where the LFO's delay puts it, within 0.4%",
     worst_notch_rel, 0.004);
  drain_violations("arm B: no contract violation");
}

/* ---- arm C: the voices --------------------------------------------------------------------- */

static void arm_c_voices(void) {
  const double sr = 96000.0;
  double worst = 0.0;
  for (int v = 1; v <= OMX_CHORUS_MAX_VOICES; v++) {
    const struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, 6.0, v, 0.5);
    for (int i = 0; i < 4; i++) {
      const double phase = (double)i / 4.0 + 0.03;
      impulse_response(&p, phase);
      for (int t = 200; t <= 12000; t += 1300) {
        const double w = 2.0 * M_PI * (double)t / sr;
        const double e = cabs(omx_oracle_dft(g_l, IRLEN, w) - closed_form(&p, phase, w));
        if (e > worst) worst = e;
      }
      /* UNITY AT DC, whatever the voice count: the dry and the mean of the voices are a convex
       * combination, so a stage that lost or double-counted a voice fails here. */
      const double dc = cabs(omx_oracle_dft(g_l, IRLEN, 0.0));
      ok(fabs(dc - 1.0) <= 1e-4, "arm C: unity at DC for every voice count", dc, 1.0);
      /* The ensemble reads the ONE oscillator at k/N turns apart. */
      struct omx_lfo lfo = {(float)phase, 0.0f, 0.0f};
      for (int k = 0; k < v; k++) {
        const double want = (double)omx_lfo_sweep(
            p.base_samples, p.depth_samples, omx_lfo_at(&lfo, (float)k / (float)v));
        const double got = (double)omx_chorus_voice_delay(&p, &lfo, k, v);
        ok(got == want, "arm C: voice k reads the oscillator k/N turns ahead", got, want);
      }
    }
  }
  ok(worst <= 2e-4, "arm C: 1 to 4 voices, summed at 1/N, ARE the closed form", worst, 2e-4);
  drain_violations("arm C: no contract violation");
}

/* ---- arm D: against time -------------------------------------------------------------------- */

#define DLEN 262144u
static float g_tl[DLEN], g_tr[DLEN];

static void arm_d_against_time(void) {
  const double sr = 96000.0, f = 1000.0, A = 0.5, rate = 2.0;
  struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, 4.0, 1, 0.5);
  struct omx_chorus_state s;
  freeze(&s, 0.0);
  p.lfo_inc = omx_lfo_inc((float)rate, (float)sr);
  const double w = 2.0 * M_PI * f / sr;
  for (uint32_t i = 0; i < DLEN; i++) {
    g_tl[i] = (float)(A * sin(w * (double)i));
    g_tr[i] = g_tl[i];
  }
  /* WARM FIRST: a line reading its own zeroed ring is not the stage under test. */
  omx_chorus_process(g_tl, g_tr, 8192u, &p, &s);
  const uint32_t win = 512u;
  double worst = 0.0, worst_control = 0.0;
  uint32_t windows = 0;
  for (uint32_t start = 8192u; start + win <= DLEN; start += win) {
    const double phase = (double)s.lfo.phase; /* the phase this window BEGINS at */
    omx_chorus_process(&g_tl[start], &g_tr[start], win, &p, &s);
    /* Goertzel at the tone over the window — the measured envelope. */
    double complex acc = 0.0;
    for (uint32_t i = 0; i < win; i++) acc += (double)g_tl[start + i] * cexp(-I * w * (double)i);
    const double got = 2.0 * cabs(acc) / (double)win;
    /* The closed form AVERAGED over the window's own phases: a swept comb read by one Goertzel
     * window is the mean of its complex response across the window, not its value at the
     * midpoint — the midpoint alone misses by 1.03 dB beside a notch (−25.9 dB, just above the
     * gate), which is the window's smear, not the stage's. */
    double complex mean = 0.0;
    for (uint32_t i = 0; i < win; i++)
      mean += closed_form(&p, (double)omx_lfo_wrap((float)(phase + (double)s.lfo.inc * i)), w);
    const double want = A * cabs(mean / (double)win);
    const double mid = (double)omx_lfo_wrap((float)(phase + (double)s.lfo.inc * win * 0.5));
    const double off = (double)omx_lfo_wrap((float)(mid + 0.25));
    const double wrong = A * cabs(closed_form(&p, off, w));
    /* A level gate: within a few dB of a null the window's own smearing dominates, and a
     * comparison there would be measuring the window rather than the stage. The Goertzel's own
     * floor here is ≈ −47 dB re the tone (0.75 dB of error at −25.6 dB, measured 2026-09-27 with
     * the compensated LFO), so the gate sits at −20 dB, 27 dB above it. */
    if (want > 0.1 * A) {
      const double e = fabs(20.0 * log10(got / want));
      if (e > worst) worst = e;
      /* THE CONTROL IS A WORST CASE, NOT A BEST ONE. A quarter-turn-late phase coincides with the
       * true one somewhere in every sweep — that is what a periodic sweep does — so the minimum
       * over windows is near zero for a working comparison as well and would prove nothing. What
       * the wrong phase cannot do is stay within the tolerance ACROSS the sweep. */
      const double c = fabs(20.0 * log10(wrong / want));
      if (c > worst_control) worst_control = c;
      windows++;
    }
  }
  ok(windows >= 200, "arm D: the sweep was measured over many windows", (double)windows, 200.0);
  ok(worst <= 0.6, "arm D: the running comb tracks the closed form over time, within 0.6 dB",
     worst, 0.6);
  ok(worst_control >= 5.0,
     "arm D control: the same comparison against a quarter-turn-late phase does NOT hold",
     worst_control, 5.0);
  printf("mix_chorus arm D: 96 kHz, 2 Hz sweep, %u windows, worst error %.3f dB (a quarter turn "
         "off is out by up to %.3f dB)\n",
         windows, worst, worst_control);
  drain_violations("arm D: no contract violation");
}

/* ---- arm E: no click ------------------------------------------------------------------------ */

#define CLEN 8192u
static float g_cl[CLEN], g_cr[CLEN], g_zoh[CLEN];

static void arm_e_no_click(void) {
  const double f = 1000.0, A = 0.5;
  for (int ri = 0; ri < 4; ri++) {
    const double sr = RATES[ri];
    const double wt = 2.0 * M_PI * f / sr;
    /* The deepest, fastest modulation the row's travel allows (spec §3a) — the worst case for a
     * click by a long way. */
    struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, OMX_CHORUS_MAX_DEPTH_MS, 1, 1.0);
    struct omx_chorus_state s;
    freeze(&s, 0.0);
    p.lfo_inc = omx_lfo_inc(8.0f, (float)sr);

    /* THE BOUND, the fdelay arm C derivation, restated in ω and the oracle's own response so it
     * carries no rate constant: a delay slewing by `dd` per sample is a resampling, whose ideal
     * output is the same tone at ω(1 − dd) and whose largest step is 2A·sin(ω(1−dd)/2); on top of
     * it each sample may be off by A·ε, ε the interpolator's worst deviation over the delays the
     * sweep crosses, and two consecutive samples may be off in opposite directions.
     *
     * ONE TERM DIFFERS FROM THE FDELAY ARM, AND IT IS THE SIGN. That arm slews the delay in ONE
     * direction, so its ideal frequency is ω(1 − dd) and its step is the smaller for it. An LFO
     * sweeps BOTH ways: on the falling half the delay shortens, the tone is resampled UP to
     * ω(1 + dd), and the largest step in the block comes from there. Bounding with (1 − dd) here
     * measured 0.098 against a bound of 0.044 — the arm was right and the bound was half a
     * derivation, which is exactly the shape of a test that would have been "fixed" by loosening
     * it. The sweep's peak slope is depth/2 × max|ds/dφ| × inc, and max|ds/dφ| is 8 for the
     * parabola (4 in t, doubled by t = 2u − 1). */
    const double dd = 4.0 * (double)p.depth_samples * (double)p.lfo_inc; /* max |d'(n)| */
    double eps = 0.0;
    for (int i = 0; i <= 200; i++) {
      const double d = (double)p.base_samples + (double)p.depth_samples * (double)i / 200.0;
      const double e = cabs(omx_oracle_fdelay(OMX_CHORUS_ORDER, d, wt) * cexp(I * wt * d) - 1.0);
      if (e > eps) eps = e;
    }
    const double ideal_step = 2.0 * A * sin(wt * (1.0 + dd) / 2.0);
    const double bound = ideal_step + 2.0 * A * (eps + 8.0 * (double)FLT_EPSILON);

    for (uint32_t i = 0; i < CLEN; i++) {
      g_cl[i] = (float)(A * sin(wt * (double)i));
      g_cr[i] = g_cl[i];
    }
    omx_chorus_process(g_cl, g_cr, 2048u, &p, &s); /* warm the ring */
    for (uint32_t i = 0; i < CLEN; i++) {
      g_cl[i] = (float)(A * sin(wt * (double)(i + 2048u)));
      g_cr[i] = g_cl[i];
    }
    omx_chorus_process(g_cl, g_cr, CLEN, &p, &s);
    double worst = 0.0;
    for (uint32_t i = 1; i < CLEN; i++) {
      const double step = fabs((double)g_cl[i] - (double)g_cl[i - 1]);
      if (step > worst) worst = step;
    }
    char what[200];
    snprintf(what, sizeof what,
             "arm E, %.1f kHz: the deepest, fastest sweep steps no more than the tone does "
             "(bound %.6f)",
             sr / 1000.0, bound);
    ok(worst <= bound, what, worst, bound);

    /* THE POSITIVE CONTROL: the same modulation read at the NEAREST sample — what the fractional
     * line exists not to be. The measure must SEE that click. */
    struct omx_lfo z = {0.0f, p.lfo_inc, 0.0f};
    double zworst = 0.0;
    for (uint32_t i = 0; i < CLEN; i++) {
      const double d = (double)omx_lfo_sweep(p.base_samples, p.depth_samples, omx_lfo_at(&z, 0.0f));
      const double nearest = floor(d + 0.5);
      g_zoh[i] = (float)(A * sin(wt * ((double)(i + 2048u) - nearest)));
      omx_lfo_advance(&z);
      if (i > 0) {
        const double step = fabs((double)g_zoh[i] - (double)g_zoh[i - 1]);
        if (step > zworst) zworst = step;
      }
    }
    snprintf(what, sizeof what, "arm E control, %.1f kHz: a NEAREST-SAMPLE sweep breaks the bound",
             sr / 1000.0);
    ok(zworst > bound, what, zworst, bound);
    printf("mix_chorus arm E, %.1f kHz: worst step %.6f, bound %.6f (nearest-sample control "
           "%.6f)\n",
           sr / 1000.0, worst, bound, zworst);
  }
  drain_violations("arm E: no contract violation");
}

/* ---- arm F: bypass identity ----------------------------------------------------------------- */

#define BLEN 1024u
static float g_bl[BLEN], g_br[BLEN], g_ref_l[BLEN], g_ref_r[BLEN];

static void arm_f_bypass(void) {
  for (uint32_t i = 0; i < BLEN; i++) {
    /* Words below the flush floor on purpose: bit-exactness is the stronger law, and the dry path
     * must not round, flush or multiply them. */
    g_ref_l[i] = (float)((i % 7u == 0u) ? 1e-40 : sin((double)i * 0.1) * 0.7);
    g_ref_r[i] = (float)((i % 5u == 0u) ? -3e-41 : cos((double)i * 0.07) * 0.6);
  }
  struct omx_chorus_state s;
  freeze(&s, 0.3);

  struct omx_chorus p = atom(96000.0, OMX_CHORUS_BASE_MS, 4.0, 3, 0.5);
  p.lfo_inc = omx_lfo_inc(1.0f, 96000.0f);
  p.enabled = 0;
  memcpy(g_bl, g_ref_l, sizeof g_bl);
  memcpy(g_br, g_ref_r, sizeof g_br);
  omx_chorus_process(g_bl, g_br, BLEN, &p, &s);
  ok(memcmp(g_bl, g_ref_l, sizeof g_bl) == 0 && memcmp(g_br, g_ref_r, sizeof g_br) == 0,
     "arm F: a disabled chorus is the input, bit for bit", 0.0, 0.0);
  ok(s.lfo.phase == 0.3f, "arm F: and does not move a state word", (double)s.lfo.phase, 0.3);

  p.enabled = 1;
  p.mix = 0.0f;
  memcpy(g_bl, g_ref_l, sizeof g_bl);
  memcpy(g_br, g_ref_r, sizeof g_br);
  omx_chorus_process(g_bl, g_br, BLEN, &p, &s);
  ok(memcmp(g_bl, g_ref_l, sizeof g_bl) == 0 && memcmp(g_br, g_ref_r, sizeof g_br) == 0,
     "arm F: mix 0 is the input, bit for bit, below the flush floor included", 0.0, 0.0);

  /* THE POSITIVE CONTROL: the smallest mix the row's travel can express must NOT be identity. */
  p.mix = 0.01f;
  memcpy(g_bl, g_ref_l, sizeof g_bl);
  memcpy(g_br, g_ref_r, sizeof g_br);
  omx_chorus_process(g_bl, g_br, BLEN, &p, &s);
  ok(memcmp(g_bl, g_ref_l, sizeof g_bl) != 0,
     "arm F control: a 1% mix is NOT the input — the comparison can see a difference", 0.0, 0.0);
  drain_violations("arm F: no contract violation");
}

/* ---- arm G: no gain added, no denormal state ------------------------------------------------ */

static void arm_g_gain_and_denormal(void) {
  const double sr = 96000.0;
  double worst_peak = 0.0;
  for (int v = 1; v <= OMX_CHORUS_MAX_VOICES; v++) {
    for (int mi = 0; mi <= 4; mi++) {
      struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, OMX_CHORUS_MAX_DEPTH_MS, v,
                                 0.25 * (double)mi);

      p.lfo_inc = omx_lfo_inc(3.0f, (float)sr);
      struct omx_chorus_state s;
      freeze(&s, 0.11);
      for (int pass = 0; pass < 6; pass++) {
        for (uint32_t i = 0; i < BLEN; i++) {
          /* Full scale, and not a single tone: a sum of tones is where a stage with a hidden
           * gain element shows itself. */
          const double t = (double)(i + (uint32_t)pass * BLEN);
          const double x = 0.45 * sin(2.0 * M_PI * 220.0 * t / sr) +
                           0.45 * sin(2.0 * M_PI * 1300.0 * t / sr) +
                           0.10 * sin(2.0 * M_PI * 7000.0 * t / sr);
          g_bl[i] = (float)x;
          g_br[i] = (float)x;
        }
        omx_chorus_process(g_bl, g_br, BLEN, &p, &s);
        for (uint32_t i = 0; i < BLEN; i++) {
          const double a = fabs((double)g_bl[i]);
          if (a > worst_peak) worst_peak = a;
        }
      }
    }
  }
  ok(worst_peak <= 1.0 + 1e-5,
     "arm G: no voice count and no mix adds gain — the peak never leaves unity", worst_peak, 1.0);
  printf("mix_chorus arm G: worst peak over 20 control sets at full scale %.6f (input 1.000)\n",
         worst_peak);

  /* Silence after a tone: the ring holds exact copies, so the output reaches EXACT zero rather
   * than a subnormal trickle, and no state word is left subnormal. */
  struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, 4.0, 3, 1.0);
  p.lfo_inc = omx_lfo_inc(0.6f, (float)sr);
  struct omx_chorus_state s;
  freeze(&s, 0.0);
  for (uint32_t i = 0; i < BLEN; i++) {
    g_bl[i] = (float)(0.5 * sin(2.0 * M_PI * 700.0 * (double)i / sr));
    g_br[i] = g_bl[i];
  }
  omx_chorus_process(g_bl, g_br, BLEN, &p, &s);
  int subnormal = 0, nonzero = 1;
  for (int pass = 0; pass < 8; pass++) {
    memset(g_bl, 0, sizeof g_bl);
    memset(g_br, 0, sizeof g_br);
    omx_chorus_process(g_bl, g_br, BLEN, &p, &s);
    nonzero = 0;
    for (uint32_t i = 0; i < BLEN; i++) {
      const float a = g_bl[i] < 0.0f ? -g_bl[i] : g_bl[i];
      if (a != 0.0f) nonzero = 1;
      if (a != 0.0f && a < FLT_MIN) subnormal = 1;
    }
  }
  ok(!subnormal, "arm G: no subnormal word ever leaves the stage", 0.0, 0.0);
  ok(!nonzero, "arm G: and silence in is EXACT silence out, once the ring has run through",
     (double)nonzero, 0.0);
  drain_violations("arm G: no contract violation");
}

/* ---- arm H: the golden identity against the pre-lane kernel ------------------------------- */

/** The chorus kernel exactly as it stood before lane/kernel-cost-chorus: per sample, per voice,
 *  the offset divided out and one full `omx_fdelay_read` PER LEG (split + Lagrange + taps twice).
 *  Frozen here as the oracle the faster composition must match bit for bit. */
static void chorus_ref_process(float *l, float *r, uint32_t n, const struct omx_chorus *p,
                               struct omx_chorus_state *s) {
  if (!p->enabled || n == 0u || p->mix <= 0.0f) return;
  if (s->line_l.order == 0 || s->line_r.order == 0) return;
  const int voices = p->voices < 1 ? 1
                     : p->voices > OMX_CHORUS_MAX_VOICES ? OMX_CHORUS_MAX_VOICES
                                                         : p->voices;
  const float mix = p->mix > 1.0f ? 1.0f : p->mix;
  const float dry = 1.0f - mix;
  const float per_voice = mix / (float)voices;
  s->lfo.inc = p->lfo_inc;
  for (uint32_t i = 0; i < n; i++) {
    const float xl = l[i], xr = r[i];
    omx_fdelay_write(&s->line_l, xl);
    omx_fdelay_write(&s->line_r, xr);
    float wl = 0.0f, wr = 0.0f;
    for (int k = 0; k < voices; k++) {
      const float off = (float)k / (float)voices;
      const float d = omx_lfo_sweep(p->base_samples, p->depth_samples, omx_lfo_at(&s->lfo, off));
      wl += omx_fdelay_read(&s->line_l, d);
      wr += omx_fdelay_read(&s->line_r, d);
    }
    l[i] = dry * xl + per_voice * wl;
    r[i] = dry * xr + per_voice * wr;
    omx_lfo_advance(&s->lfo);
  }
}

static float g_href_l[OMX_CHORUS_CAP], g_href_r[OMX_CHORUS_CAP];
#define HLEN 512u
static float g_hl[HLEN], g_hr[HLEN], g_hrl[HLEN], g_hrr[HLEN];

/** One run of both kernels over the same input; returns 1 when output and state stay identical.
 *  `flip` perturbs one input LSB of the reference's copy (the positive control). */
static int golden_run(double sr, int voices, double depth_ms, double lfo_hz, double mix, int flip) {
  struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, depth_ms, voices, mix);
  p.lfo_inc = omx_lfo_inc((float)lfo_hz, (float)sr);
  struct omx_chorus_state s, sref;
  freeze(&s, 0.37);
  memset(g_href_l, 0, sizeof g_href_l);
  memset(g_href_r, 0, sizeof g_href_r);
  if (omx_chorus_state_init(&sref, g_href_l, g_href_r, OMX_CHORUS_CAP) != OMX_FDELAY_OK) return 0;
  sref.lfo.phase = 0.37f;
  static const uint32_t sizes[] = {1u, 7u, 64u, 512u, 333u, 128u, 2u, 511u};
  uint32_t seed = 0x9e3779b9u, t = 0u;
  int same = 1;
  for (int b = 0; b < 48; b++) {
    const uint32_t n = sizes[b % 8];
    for (uint32_t i = 0; i < n; i++, t++) {
      seed = seed * 1664525u + 1013904223u;
      const double noise = ((double)(seed >> 8) / 16777216.0 - 0.5) * 0.2;
      g_hl[i] = (float)(0.6 * sin(2.0 * M_PI * 997.0 * (double)t / sr) + noise);
      g_hr[i] = (float)(0.5 * sin(2.0 * M_PI * 313.0 * (double)t / sr) - noise);
    }
    memcpy(g_hrl, g_hl, n * sizeof(float));
    memcpy(g_hrr, g_hr, n * sizeof(float));
    if (flip && b == 20) {
      uint32_t w;
      memcpy(&w, &g_hrl[0], sizeof w);
      w ^= 1u;
      memcpy(&g_hrl[0], &w, sizeof w);
    }
    omx_chorus_process(g_hl, g_hr, n, &p, &s);
    chorus_ref_process(g_hrl, g_hrr, n, &p, &sref);
    if (memcmp(g_hl, g_hrl, n * sizeof(float)) != 0 || memcmp(g_hr, g_hrr, n * sizeof(float)) != 0)
      same = 0;
  }
  if (memcmp(&s.lfo, &sref.lfo, sizeof s.lfo) != 0 || s.line_l.wpos != sref.line_l.wpos ||
      memcmp(g_ring_l, g_href_l, sizeof g_ring_l) != 0 ||
      memcmp(g_ring_r, g_href_r, sizeof g_ring_r) != 0)
    same = 0;
  return same;
}

static void arm_h_golden_identity(void) {
  static const double rates[] = {44100.0, 48000.0, 96000.0, 192000.0};
  static const double depths[] = {0.0, 4.0, OMX_CHORUS_MAX_DEPTH_MS};
  static const double lfos[] = {0.1, 0.6, 5.0};
  int runs = 0, identical = 0;
  for (int ri = 0; ri < 4; ri++)
    for (int v = 1; v <= OMX_CHORUS_MAX_VOICES; v++)
      for (int di = 0; di < 3; di++)
        for (int li = 0; li < 3; li++) {
          runs++;
          identical += golden_run(rates[ri], v, depths[di], lfos[li], (li == 1) ? 0.35 : 1.0, 0);
        }
  ok(identical == runs,
     "arm H: the kernel is memcmp-identical (output and state) to the pre-lane kernel",
     (double)identical, (double)runs);
  printf("mix_chorus arm H: %d/%d control sets byte-identical to the pre-lane kernel\n", identical,
         runs);
  ok(!golden_run(96000.0, 3, 4.0, 0.6, 0.35, 1),
     "arm H control: one flipped input LSB IS seen by the comparison", 0.0, 0.0);
  drain_violations("arm H: no contract violation");
}

/* ---- arm I: spread — the right leg reads the one oscillator +spread turns ahead ------------- */

static float g_iring_l[OMX_CHORUS_CAP], g_iring_r[OMX_CHORUS_CAP];
static float g_il[BLEN], g_ir[BLEN];

/** Centroid of one leg's impulse response — the wet path's delay at mix 1, one voice. */
static double centroid(const float *h) {
  double num = 0.0, den = 0.0;
  for (uint32_t i = 0; i < IRLEN; i++) {
    num += (double)i * (double)h[i];
    den += (double)h[i];
  }
  return num / den;
}

static void arm_i_spread(void) {
  int identity_ok = 1, control_differs = 1;
  double worst_cf = 0.0, worst_null = 0.0, worst_quad = 0.0, worst_opp = 0.0, worst_oppc = -2.0;
  for (int ri = 0; ri < 4; ri++) {
    const double sr = RATES[ri];

    /* I1 — identity at the default: the kernel at spread 0 against the frozen one-phase stage,
     * running LFO, every voice count, distinct legs with words below the flush floor, memcmp. */
    for (int v = 1; v <= OMX_CHORUS_MAX_VOICES; v++) {
      struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, 6.0, v, 0.6);
      p.lfo_inc = omx_lfo_inc(2.3f, (float)sr);
      struct omx_chorus_state a, b;
      freeze(&a, 0.37);
      memset(g_iring_l, 0, sizeof g_iring_l);
      memset(g_iring_r, 0, sizeof g_iring_r);
      (void)omx_chorus_state_init(&b, g_iring_l, g_iring_r, OMX_CHORUS_CAP);
      b.lfo.phase = 0.37f;
      for (int pass = 0; pass < 4; pass++) {
        for (uint32_t i = 0; i < BLEN; i++) {
          const double t = (double)(i + (uint32_t)pass * BLEN);
          g_bl[i] = (float)((i % 11u == 0u) ? 1e-40 : 0.6 * sin(2.0 * M_PI * 440.0 * t / sr));
          g_br[i] = (float)(0.5 * sin(2.0 * M_PI * 1210.0 * t / sr + 0.3));
        }
        memcpy(g_il, g_bl, sizeof g_il);
        memcpy(g_ir, g_br, sizeof g_ir);
        omx_chorus_process(g_bl, g_br, BLEN, &p, &a);
        chorus_ref_process(g_il, g_ir, BLEN, &p, &b);
        if (memcmp(g_bl, g_il, sizeof g_bl) != 0 || memcmp(g_br, g_ir, sizeof g_br) != 0)
          identity_ok = 0;
      }
      if (a.lfo.phase != b.lfo.phase) identity_ok = 0;
      /* THE POSITIVE CONTROL: the smallest spread the row can express must NOT be identity. */
      p.spread = 0.01f;
      for (uint32_t i = 0; i < BLEN; i++) {
        g_bl[i] = g_il[i] = (float)(0.6 * sin(2.0 * M_PI * 440.0 * (double)i / sr));
        g_br[i] = g_ir[i] = g_bl[i];
      }
      omx_chorus_process(g_bl, g_br, BLEN, &p, &a);
      chorus_ref_process(g_il, g_ir, BLEN, &p, &b);
      if (memcmp(g_br, g_ir, sizeof g_br) == 0) control_differs = 0;
    }

    /* I2 — the right leg IS the closed form at φ + spread: its response at a frozen phase φ equals
     * §2's H(w) evaluated at φ + spread, complex, across the band, 1..4 voices. */
    const double spreads[3] = {0.0, 0.25, 0.5};
    for (int si = 0; si < 3; si++) {
      for (int v = 1; v <= OMX_CHORUS_MAX_VOICES; v++) {
        struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, 6.0, v, 0.5);
        p.spread = (float)spreads[si];
        const double phase = 0.13;
        impulse_response(&p, phase);
        double ph = phase + spreads[si];
        if (ph >= 1.0) ph -= 1.0;
        for (int fi = 1; fi <= 24; fi++) {
          const double w = M_PI * (double)fi / 25.0;
          double complex got = 0.0;
          for (uint32_t i = 0; i < IRLEN; i++) got += (double)g_r[i] * cexp(-I * w * (double)i);
          const double e = cabs(got - closed_form(&p, ph, w));
          if (e > worst_cf) worst_cf = e;
        }
      }
    }

    /* I3 — one source on both legs, one voice, all wet: the two legs' delay deviations from the
     * sweep centre over 32 phases of the turn. Null at 0, orthogonal at 0.25, negated at 0.5. */
    double dev_l[3][32], dev_r[3][32];
    for (int si = 0; si < 3; si++) {
      struct omx_chorus p = atom(sr, OMX_CHORUS_BASE_MS, 6.0, 1, 1.0);
      p.spread = (float)spreads[si];
      const double centre = (double)p.base_samples + 0.5 * (double)p.depth_samples;
      for (int k = 0; k < 32; k++) {
        impulse_response(&p, (double)k / 32.0);
        dev_l[si][k] = centroid(g_l) - centre;
        dev_r[si][k] = centroid(g_r) - centre;
      }
    }
    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (int k = 0; k < 32; k++) {
      const double nul = fabs(dev_l[0][k] - dev_r[0][k]);
      if (nul > worst_null) worst_null = nul;
      ll += dev_l[1][k] * dev_l[1][k];
      rr += dev_r[1][k] * dev_r[1][k];
      lr += dev_l[1][k] * dev_r[1][k];
      const double opp = fabs(dev_l[2][k] + dev_r[2][k]);
      if (opp > worst_opp) worst_opp = opp;
    }
    const double quad = fabs(lr / sqrt(ll * rr));
    if (quad > worst_quad) worst_quad = quad;
    double pl = 0.0, pr = 0.0, plr = 0.0;
    for (int k = 0; k < 32; k++) {
      pl += dev_l[2][k] * dev_l[2][k];
      pr += dev_r[2][k] * dev_r[2][k];
      plr += dev_l[2][k] * dev_r[2][k];
    }
    const double oppc = plr / sqrt(pl * pr);
    if (oppc > worst_oppc) worst_oppc = oppc;
    printf("mix_chorus arm I, %.1f kHz: L-R null %.2e samples at 0, correlation %.4f at 0.25, "
           "%.4f at 0.5 (L+R %.2e samples), sweep %.2f samples p-p\n",
           sr / 1000.0, worst_null, lr / sqrt(ll * rr), oppc, worst_opp,
           (double)(6.0 * sr / 1000.0));
  }
  ok(identity_ok, "arm I: spread 0 IS the one-phase stage, memcmp, running LFO, four rates", 0.0, 0.0);
  ok(control_differs, "arm I control: spread 0.01 moves the right leg — the memcmp can see it", 0.0,
     0.0);
  ok(worst_cf <= 2e-4, "arm I: the right leg is §2's closed form at phase + spread", worst_cf, 2e-4);
  ok(worst_null == 0.0, "arm I: at spread 0 the two legs' delays are ONE delay (L-R null)",
     worst_null, 0.0);
  ok(worst_quad <= 0.02, "arm I: at spread 0.25 the legs sweep in QUADRATURE", worst_quad, 0.02);
  ok(worst_opp <= 4e-3 && worst_oppc <= -0.999,
     "arm I: at spread 0.5 the legs sweep in OPPOSITION (the Dimension)", worst_opp, 4e-3);
  drain_violations("arm I: no contract violation");
}

int main(void) {
  omx_fx_require_rate_floor();
  arm_a_delay_follows();
  arm_b_swept_comb();
  arm_c_voices();
  arm_d_against_time();
  arm_e_no_click();
  arm_f_bypass();
  arm_g_gain_and_denormal();
  arm_h_golden_identity();
  arm_i_spread();
  printf("mix_chorus: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
