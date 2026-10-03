// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * flanger.test.c — the flanger against the closed form of its RESONANT comb.
 *
 * Spec: docs/design/specs/2026-09-22-chorus-and-flanger.md §2 and §9, at the four basic rates
 * (44.1 / 48 / 96 / 192 kHz — the operator's 2026-09-20 ruling).
 *
 * Same method as `mix_chorus.test.c`: with its oscillator frozen the stage is LTI, so each arm
 * reads an exact transfer function out of an IMPULSE RESPONSE and compares it against the closed
 * form built on the LINE's own response (Newton divided differences), never on the kernel itself.
 * The one difference is that this stage is RECURSIVE, so its impulse response is infinite: the
 * capture runs until the ring has decayed past the comparison's own floor, which is a fact this
 * file asserts (arm D) rather than assumes.
 *
 *     H(w) = (1 − mix) + mix · R(w, d) / (1 − fb · R(w, d) · e^{-jw})
 *
 * THE e^{-jw} IS NOT DECORATION. The feedback multiplies the PREVIOUS wet sample, so the loop
 * carries one sample of its own and the resonances sit at f = m/(d + 1), not m/d — 2% away at the
 * short end of the sweep. `omx_flanger_loop_delay` publishes that number and this file measures
 * against it.
 *
 * ## The arms
 *
 *   A — THE DELAY FOLLOWS THE LFO, with the loop open (fb = 0): the impulse response's centroid
 *       against `base + depth·(1 + s(φ))/2`, 16 phases, four rates, with the same half-a-sample
 *       control the chorus arm uses.
 *   B — THE RESONANCE. Peak height, notch depth and the −3 dB WIDTH around f = m/(d+1), at
 *       fb = +0.6 and fb = −0.6 (which swaps peaks and notches — the hollow flange), against the
 *       closed form, at four rates. The peak height is also compared with the algebraic
 *       (1 − mix) + mix/(1 − fb), the IDEAL comb's tooth (the `output-le-input-plus` law's
 *       sample-peak bound carries the read kernel's Λ on top — spec §6, arm G).
 *   C — THE CLAMP. fb asked for ±1.5 is clamped to ±0.95, where `omx_flanger_gain_bound`
 *       claims NO finite bound (Λ·0.95 ≥ 1, spec §6) and the output stays finite; at fb 0.79,
 *       inside the proven range, the output stays inside the published bound over a long run at
 *       full scale; and the clamped stage is identical to one asked for ±0.95 in the first place.
 *   D — TAIL DECAYS. The input stops; the output falls below −100 dBFS within the loop count the
 *       feedback implies, and the feedback state word reaches EXACTLY zero rather than trickling
 *       down into subnormals.
 *   E — NO CLICK, four rates, the chorus arm's bound (ω(1 + dd), both directions of the sweep),
 *       with the nearest-sample control that must break it.
 *   F — BYPASS IDENTITY: `enabled = 0` and `mix = 0`, memcmp-identical, below the flush floor.
 *   G — `chorus_flanger_stay_inside_their_bound` (F6 of docs/audits/2026-09-25-native-fx-rt-
 *       review.md, spec §6): a full-scale fs/4 pattern read half a sample off — the Lagrange-3
 *       kernel's worst case, Σ|c| = Λ = OMX_FDELAY_READ_L1_NORM — stays inside
 *       `omx_chorus_gain_bound` and `omx_flanger_gain_bound`, four rates, with the whole-sample
 *       control; plus the evidence that the ruled Λ/(1 − |fb|) form is NOT a bound.
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
#include <omxdsp/fx/omx_flanger.h>
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

static float g_ring_l[OMX_FLANGER_CAP], g_ring_r[OMX_FLANGER_CAP];

/* Long enough for the recursion to decay past the comparison floor at fb = 0.95: the loop is
 * at most (6 ms + 1) samples at 192 kHz ≈ 1153, and 0.95^n reaches 1e-12 by n = 540 loops. */
#define IRLEN 65536u
static float g_l[IRLEN], g_r[IRLEN];

static struct omx_flanger atom(double sr, double base_ms, double depth_ms, double fb, double mix) {
  struct omx_flanger p;
  p.enabled = 1;
  p.base_samples = (float)(base_ms * sr / 1000.0);
  p.depth_samples = (float)(depth_ms * sr / 1000.0);
  p.feedback = (float)fb;
  p.lfo_inc = 0.0f; /* FROZEN by default: every arm but the click one measures an LTI stage */
  p.mix = (float)mix;
  return p;
}

static void freeze(struct omx_flanger_state *s, double phase) {
  memset(g_ring_l, 0, sizeof g_ring_l);
  memset(g_ring_r, 0, sizeof g_ring_r);
  enum omx_fdelay_code c = omx_flanger_state_init(s, g_ring_l, g_ring_r, OMX_FLANGER_CAP);
  ok(c == OMX_FDELAY_OK, "the flanger arms over its rings", (double)c, 0.0);
  s->lfo.phase = (float)phase;
  s->lfo.inc = 0.0f;
}

static void impulse_response(const struct omx_flanger *p, double phase) {
  struct omx_flanger_state s;
  freeze(&s, phase);
  memset(g_l, 0, sizeof g_l);
  memset(g_r, 0, sizeof g_r);
  g_l[0] = 1.0f;
  g_r[0] = 1.0f;
  omx_flanger_process(g_l, g_r, IRLEN, p, &s);
}

/** The delay the sweep is at, in samples, at `phase`. */
static double sweep_delay(const struct omx_flanger *p, double phase) {
  struct omx_lfo lfo = {(float)phase, 0.0f, 0.0f};
  return (double)omx_flanger_delay(p, &lfo);
}

/** §2's closed form, with the loop's own sample in it. */
static double complex closed_form(const struct omx_flanger *p, double phase, double w) {
  const double d = sweep_delay(p, phase);
  const double complex R = omx_oracle_fdelay(OMX_FLANGER_ORDER, d, w);
  const double fb = (double)omx_flanger_clamp_fb(p->feedback);
  const double mix = (double)p->mix;
  return (1.0 - mix) + mix * R / (1.0 - fb * R * cexp(-I * w));
}

/* ---- arm A: the delay follows the LFO, loop open ------------------------------------------- */

static void arm_a_delay_follows(void) {
  double worst = 0.0, worst_control = 1e9;
  for (int ri = 0; ri < 4; ri++) {
    const double sr = RATES[ri];
    const struct omx_flanger p = atom(sr, OMX_FLANGER_BASE_MS, 2.0, 0.0, 1.0);
    for (int i = 0; i < 16; i++) {
      const double phase = (double)i / 16.0;
      impulse_response(&p, phase);
      double num = 0.0, den = 0.0;
      for (uint32_t k = 0; k < IRLEN; k++) {
        num += (double)k * (double)g_l[k];
        den += (double)g_l[k];
      }
      const double centroid = num / den;
      const double want = sweep_delay(&p, phase);
      const double e = fabs(centroid - want);
      if (e > worst) worst = e;
      const double c = fabs(centroid - (want + 0.5));
      if (c < worst_control) worst_control = c;
    }
    const struct omx_flanger q = p;
    printf("mix_flanger arm A, %.1f kHz: 16 phases, worst centroid error %.2e samples (delay "
           "%.1f..%.1f, loop %.1f..%.1f)\n",
           sr / 1000.0, worst, (double)q.base_samples,
           (double)(q.base_samples + q.depth_samples),
           (double)omx_flanger_loop_delay(q.base_samples),
           (double)omx_flanger_loop_delay(q.base_samples + q.depth_samples));
  }
  ok(worst <= 2e-3, "arm A: with the loop open the read delay IS the LFO's sweep", worst, 2e-3);
  ok(worst_control >= 0.4, "arm A control: and the same measure rejects half a sample away",
     worst_control, 0.4);
  drain_violations("arm A: no contract violation");
}

/* ---- arm B: the resonance ------------------------------------------------------------------- */

/** The measured magnitude of the captured response at frequency `f` (Hz) and rate `sr`. */
static double mag_at(double f, double sr) {
  return cabs(omx_oracle_dft(g_l, IRLEN, 2.0 * M_PI * f / sr));
}

static void arm_b_resonance(void) {
  double worst = 0.0;
  for (int ri = 0; ri < 4; ri++) {
    const double sr = RATES[ri];
    for (int si = 0; si < 2; si++) {
      const double fb = si == 0 ? 0.6 : -0.6;
      const double phase = 0.75; /* the sweep's crest: the longest delay, the densest comb */

      /* THE RESONANCE IS MEASURED WET-ONLY, and the reason is arithmetic rather than taste. At
       * mix 0.5 a NEGATIVE feedback's resonance is a place where the wet path is loud and
       * OPPOSITE the dry, so |H| there is a local MINIMUM — 0.75 against 0.81 at the comb's other
       * teeth. Calling that a peak and measuring its width found a width of zero, correctly. The
       * loop's own gain is 1/(1 - |fb|) whichever sign it carries, and that is what wet-only
       * reads: the sign moves the resonance by half a comb tooth, it does not move the gain. */
      const struct omx_flanger wet = atom(sr, OMX_FLANGER_BASE_MS, 3.0, fb, 1.0);
      impulse_response(&wet, phase);
      const double loop = (double)omx_flanger_loop_delay((float)sweep_delay(&wet, phase));
      const double spacing = sr / loop; /* Hz between resonances */

      /* The closed form across the band pins every feature at once, before any of them is named. */
      for (double f = 100.0; f < 0.45 * sr; f *= 1.21) {
        const double w = 2.0 * M_PI * f / sr;
        const double e = cabs(omx_oracle_dft(g_l, IRLEN, w) - closed_form(&wet, phase, w));
        if (e > worst) worst = e;
      }

      /* The m-th tooth nearest 2 kHz. A POSITIVE feedback resonates where the loop comes back in
       * phase (theta = 2 pi m); a NEGATIVE one where it comes back inverted (theta = (2m+1) pi),
       * which is half a tooth away — the hollow flange, and the one thing the sign changes. */
      const long m = (long)(2000.0 / spacing + 0.5);
      const double f_res = (fb > 0.0 ? (double)m : (double)m + 0.5) * spacing;
      const double f_anti = (fb > 0.0 ? (double)m + 0.5 : (double)m) * spacing;
      const double peak = mag_at(f_res, sr), anti = mag_at(f_anti, sr);
      const double want_peak = 1.0 / (1.0 - fabs(fb));
      const double want_anti = 1.0 / (1.0 + fabs(fb));
      ok(fabs(20.0 * log10(peak / want_peak)) <= 0.2,
         "arm B: the loop's gain at its resonance IS 1/(1-|fb|)", 20.0 * log10(peak / want_peak),
         0.2);
      ok(fabs(20.0 * log10(anti / want_anti)) <= 0.2,
         "arm B: and 1/(1+|fb|) half a tooth away", 20.0 * log10(anti / want_anti), 0.2);
      ok(peak / anti >= 3.8, "arm B: so the resonant comb is (1+|fb|)/(1-|fb|) deep", peak / anti,
         3.8);

      /* The -3 dB WIDTH around the resonance, walked out of the measurement and out of the closed
       * form by the same walk — a Q comparison a peak height alone cannot make. */
      double w_meas = 0.0, w_closed = 0.0;
      const double closed_peak = cabs(closed_form(&wet, phase, 2.0 * M_PI * f_res / sr));
      for (double dfr = 0.0005; dfr < 0.5; dfr += 0.0005) {
        if (w_meas == 0.0 && mag_at(f_res + dfr * spacing, sr) < peak / M_SQRT2)
          w_meas = 2.0 * dfr * spacing;
        if (w_closed == 0.0 &&
            cabs(closed_form(&wet, phase, 2.0 * M_PI * (f_res + dfr * spacing) / sr)) <
                closed_peak / M_SQRT2)
          w_closed = 2.0 * dfr * spacing;
      }
      ok(w_meas > 0.0 && fabs(w_meas - w_closed) <= 0.05 * w_closed,
         "arm B: the resonance's -3 dB width is the closed form's", w_meas, w_closed);

      /* AND THE ROW'S OWN NUMBER: at the shipped mix the peak of |H| stands where the algebra
       * says, SIGN INCLUDED — (1-mix) + sgn(fb)*mix/(1-|fb|), which is what the
       * `output-le-input-plus` law publishes from the control. */
      const struct omx_flanger half = atom(sr, OMX_FLANGER_BASE_MS, 3.0, fb, 0.5);
      impulse_response(&half, phase);
      const double at_res = mag_at(f_res, sr);
      const double algebraic =
          fabs((1.0 - 0.5) + (fb > 0.0 ? 1.0 : -1.0) * 0.5 / (1.0 - fabs(fb)));
      /* 0.5 dB, and the slack is a MEASURED physical term rather than room for a bug: the loop's
       * extra sample rotates the wet path by w radians against the dry before they sum, so the
       * simple algebra is the small-w limit and the gap closes as the rate rises — 0.41 dB at
       * 44.1 kHz, 0.01 dB at 192 kHz, printed at every rate. The COMPLEX comparison above carries
       * that rotation and holds to 5e-4 at all four. */
      ok(fabs(20.0 * log10(at_res / algebraic)) <= 0.5,
         "arm B: at mix 0.5 the response at the resonance is the algebra's, sign included",
         20.0 * log10(at_res / algebraic), 0.5);
      printf("mix_flanger arm B, %.1f kHz, fb %+.2f: loop %.1f samples, comb %.1f Hz, wet-only "
             "resonance %+.2f dB at %.1f Hz (want %+.2f), %+.2f dB half a tooth away (want "
             "%+.2f), depth x%.2f, -3 dB width %.1f Hz (closed form %.1f); at mix 0.5 %+.2f dB "
             "(algebra %+.2f)\n",
             sr / 1000.0, fb, loop, spacing, 20.0 * log10(peak), f_res, 20.0 * log10(want_peak),
             20.0 * log10(anti), 20.0 * log10(want_anti), peak / anti, w_meas, w_closed,
             20.0 * log10(at_res), 20.0 * log10(algebraic));
    }
  }
  ok(worst <= 5e-4, "arm B: the resonant comb IS the closed form, complex, at every rate", worst,
     5e-4);
  drain_violations("arm B: no contract violation");
}

/* ---- arm C: the clamp ----------------------------------------------------------------------- */

#define BLEN 1024u
static float g_bl[BLEN], g_br[BLEN], g_cl[BLEN];

static void arm_c_clamp(void) {
  ok(omx_flanger_clamp_fb(1.5f) == OMX_FLANGER_FB_MAX, "arm C: +1.5 clamps to the declared bound",
     (double)omx_flanger_clamp_fb(1.5f), (double)OMX_FLANGER_FB_MAX);
  ok(omx_flanger_clamp_fb(-1.5f) == -OMX_FLANGER_FB_MAX, "arm C: and -1.5 to its negative",
     (double)omx_flanger_clamp_fb(-1.5f), (double)-OMX_FLANGER_FB_MAX);

  const double sr = 96000.0;
  struct omx_flanger over = atom(sr, OMX_FLANGER_BASE_MS, 3.0, 1.5, 1.0);
  struct omx_flanger at = atom(sr, OMX_FLANGER_BASE_MS, 3.0, (double)OMX_FLANGER_FB_MAX, 1.0);
  const double bound = (double)omx_flanger_gain_bound(&over);
  ok(isinf(bound) && bound > 0.0,
     "arm C: at |fb| 0.95 >= 1/Λ the published bound claims nothing (+inf, spec §6)", bound,
     INFINITY);
  {
    const struct omx_flanger half = atom(sr, OMX_FLANGER_BASE_MS, 3.0, -0.5, 1.0);
    const double want = (double)OMX_FDELAY_READ_L1_NORM / (1.0 - 0.5 * (double)OMX_FDELAY_READ_L1_NORM);
    ok(fabs((double)omx_flanger_gain_bound(&half) - want) <= 1e-5,
       "arm C: the published bound is Λ/(1-Λ|fb|) at mix 1, fb -0.5",
       (double)omx_flanger_gain_bound(&half), want);
  }

  struct omx_flanger_state so, sa;
  freeze(&so, 0.0);
  over.lfo_inc = omx_lfo_inc(0.25f, (float)sr);
  at.lfo_inc = over.lfo_inc;
  memcpy(&sa, &so, sizeof sa);
  /* Two states cannot share one ring; give the second its own by running them in turn. */
  double worst_peak = 0.0;
  int identical = 1;
  for (int pass = 0; pass < 64; pass++) {
    for (uint32_t i = 0; i < BLEN; i++) {
      const double t = (double)(i + (uint32_t)pass * BLEN);
      /* Full scale, and at the comb's own spacing so the resonance is EXCITED rather than
       * merely present — a bound tested off-resonance is a bound nothing pushed against. */
      const double x = 0.999 * sin(2.0 * M_PI * 1000.0 * t / sr);
      g_bl[i] = (float)x;
      g_br[i] = (float)x;
    }
    omx_flanger_process(g_bl, g_br, BLEN, &over, &so);
    for (uint32_t i = 0; i < BLEN; i++) {
      const double a = fabs((double)g_bl[i]);
      if (a > worst_peak) worst_peak = a;
    }
  }
  ok(isfinite(worst_peak), "arm C: an over-driven feedback stays finite", worst_peak, 0.0);
  printf("mix_flanger arm C: fb asked 1.5, clamped %.2f, peak %.4f over 64 blocks at full scale "
         "(no bound claimed above |fb| 1/Λ)\n",
         (double)omx_flanger_clamp_fb(over.feedback), worst_peak);
  {
    /* Inside the proven range: the same full-scale drive at fb 0.79 stays inside its bound. */
    struct omx_flanger in = atom(sr, OMX_FLANGER_BASE_MS, 3.0, 0.79, 1.0);
    in.lfo_inc = over.lfo_inc;
    const double b79 = (double)omx_flanger_gain_bound(&in);
    struct omx_flanger_state s79;
    freeze(&s79, 0.0);
    double pk79 = 0.0;
    for (int pass = 0; pass < 64; pass++) {
      for (uint32_t i = 0; i < BLEN; i++) {
        const double t = (double)(i + (uint32_t)pass * BLEN);
        g_bl[i] = g_br[i] = (float)(0.999 * sin(2.0 * M_PI * 1000.0 * t / sr));
      }
      omx_flanger_process(g_bl, g_br, BLEN, &in, &s79);
      for (uint32_t i = 0; i < BLEN; i++) {
        const double a = fabs((double)g_bl[i]);
        if (!(a <= pk79)) pk79 = a; /* not fmax: GCC 12.2 arm64, BUILDING.md */
      }
    }
    ok(isfinite(b79) && pk79 <= b79 * 0.999 + 1e-3,
       "arm C: fb 0.79 (proven range) stays inside the bound the row publishes", pk79, b79 * 0.999);
    printf("mix_flanger arm C: fb 0.79, peak %.4f over 64 blocks (published bound %.4f)\n", pk79, b79);
  }

  /* The clamped stage IS the stage asked for the bound: same state, same input, same output. */
  struct omx_flanger_state s1;
  freeze(&s1, 0.2);
  for (uint32_t i = 0; i < BLEN; i++) {
    g_bl[i] = (float)(0.5 * sin(2.0 * M_PI * 700.0 * (double)i / sr));
    g_br[i] = g_bl[i];
  }
  omx_flanger_process(g_bl, g_br, BLEN, &over, &s1);
  memcpy(g_cl, g_bl, sizeof g_cl);
  freeze(&s1, 0.2);
  for (uint32_t i = 0; i < BLEN; i++) {
    g_bl[i] = (float)(0.5 * sin(2.0 * M_PI * 700.0 * (double)i / sr));
    g_br[i] = g_bl[i];
  }
  omx_flanger_process(g_bl, g_br, BLEN, &at, &s1);
  identical = memcmp(g_bl, g_cl, sizeof g_cl) == 0;
  ok(identical, "arm C: a clamped stage is the stage asked for the clamp, bit for bit", 0.0, 0.0);
  drain_violations("arm C: no contract violation");
}

/* ---- arm D: the tail decays ----------------------------------------------------------------- */

static void arm_d_tail_decays(void) {
  const double sr = 96000.0, fb = 0.9;
  struct omx_flanger p = atom(sr, OMX_FLANGER_BASE_MS, 0.0, fb, 1.0);
  struct omx_flanger_state s;
  freeze(&s, 0.25); /* depth 0: a fixed comb, so the loop length is one number */
  const double loop = (double)omx_flanger_loop_delay((float)sweep_delay(&p, 0.25));
  for (uint32_t i = 0; i < BLEN; i++) {
    g_bl[i] = (float)(0.5 * sin(2.0 * M_PI * 1000.0 * (double)i / sr));
    g_br[i] = g_bl[i];
  }
  omx_flanger_process(g_bl, g_br, BLEN, &p, &s);

  /* THE ENVELOPE, NOT THE SAMPLE. A decaying sinusoid crosses zero every half cycle, so the
   * first sample under a threshold is a zero crossing and not a decay — measured: the first
   * such sample arrived at 53 loops and the signal was back at 1e-3 straight after. The peak
   * of each block is the envelope, and that is what has to fall and stay down. */
  /* THE STORED AMPLITUDE IS NOT THE INPUT'S. A resonant loop holds up to 1/(1-fb) times what
   * was fed into it — ten times, here — so a decay predicted from the input's own 0.5 is short
   * by the 24 loops it takes to come back down to it. Measured: 125 loops against a prediction
   * of 105 until this term was put in. */
  const double stored = 0.5 / (1.0 - fb);
  const double loops_to_silence = log(1e-5 / stored) / log(fb);
  const uint32_t need = (uint32_t)(loops_to_silence * loop) + (uint32_t)loop + BLEN;
  /* And the loops the FLUSH FLOOR implies, which is where the state word reaches exact zero:
   * 0.9^n below the tier's flush threshold, not below audibility. */
  const double loops_to_zero = log(1e-20 / stored) / log(fb);
  const uint32_t need_zero = (uint32_t)(loops_to_zero * loop) + 4u * (uint32_t)loop + BLEN;
  uint32_t silent_at = 0u, zero_at = 0u;
  int subnormal = 0, came_back = 0;
  for (uint32_t done = 0; done < need_zero + 8u * BLEN; done += BLEN) {
    memset(g_bl, 0, sizeof g_bl);
    memset(g_br, 0, sizeof g_br);
    omx_flanger_process(g_bl, g_br, BLEN, &p, &s);
    double block_peak = 0.0;
    for (uint32_t i = 0; i < BLEN; i++) {
      const float a = g_bl[i] < 0.0f ? -g_bl[i] : g_bl[i];
      if (a != 0.0f && a < FLT_MIN) subnormal = 1;
      if ((double)a > block_peak) block_peak = (double)a;
    }
    if (silent_at == 0u && block_peak <= 1e-5) silent_at = done + BLEN;
    if (silent_at != 0u && block_peak > 1e-5) came_back = 1;
    if (zero_at == 0u && s.fb_l == 0.0f && s.fb_r == 0.0f) zero_at = done + BLEN;
  }
  ok(silent_at > 0u && silent_at <= need,
     "arm D: the envelope falls below -100 dBFS within the loops the feedback implies",
     (double)silent_at, (double)need);
  ok(!came_back, "arm D: and never comes back up", 0.0, 0.0);
  ok(!subnormal, "arm D: no subnormal word ever leaves the stage", 0.0, 0.0);
  ok(zero_at > 0u && zero_at <= need_zero,
     "arm D: the feedback word reaches EXACTLY zero, at the flush floor the tier declares",
     (double)zero_at, (double)need_zero);
  printf("mix_flanger arm D: fb %.2f, loop %.1f samples - envelope below -100 dBFS after %u "
         "samples (%.0f loops, geometry needs %u), feedback word exactly zero after %u (%.0f "
         "loops, flush floor needs %u), no subnormal seen\n",
         fb, loop, silent_at, (double)silent_at / loop, need, zero_at, (double)zero_at / loop,
         need_zero);
  drain_violations("arm D: no contract violation");
}

/* ---- arm E: no click ------------------------------------------------------------------------- */

#define CLEN 8192u
static float g_el[CLEN], g_er[CLEN], g_zoh[CLEN];

static void arm_e_no_click(void) {
  const double f = 1000.0, A = 0.5;
  for (int ri = 0; ri < 4; ri++) {
    const double sr = RATES[ri];
    const double wt = 2.0 * M_PI * f / sr;
    /* The deepest, fastest sweep the row's travel allows, and NO feedback: a click is a property
     * of the modulated read, and the loop would only add the same click one loop later. */
    struct omx_flanger p = atom(sr, OMX_FLANGER_BASE_MS, OMX_FLANGER_MAX_DEPTH_MS, 0.0, 1.0);
    struct omx_flanger_state s;
    freeze(&s, 0.0);
    p.lfo_inc = omx_lfo_inc(5.0f, (float)sr);

    const double dd = 4.0 * (double)p.depth_samples * (double)p.lfo_inc;
    double eps = 0.0;
    for (int i = 0; i <= 200; i++) {
      const double d = (double)p.base_samples + (double)p.depth_samples * (double)i / 200.0;
      const double e = cabs(omx_oracle_fdelay(OMX_FLANGER_ORDER, d, wt) * cexp(I * wt * d) - 1.0);
      if (e > eps) eps = e;
    }
    /* ω(1 + dd): the sweep runs both ways and the falling half resamples the tone UP (the
     * chorus arm's correction to the fdelay derivation, same reason). */
    const double ideal_step = 2.0 * A * sin(wt * (1.0 + dd) / 2.0);
    const double bound = ideal_step + 2.0 * A * (eps + 8.0 * (double)FLT_EPSILON);

    for (uint32_t i = 0; i < CLEN; i++) {
      g_el[i] = (float)(A * sin(wt * (double)i));
      g_er[i] = g_el[i];
    }
    omx_flanger_process(g_el, g_er, 2048u, &p, &s);
    for (uint32_t i = 0; i < CLEN; i++) {
      g_el[i] = (float)(A * sin(wt * (double)(i + 2048u)));
      g_er[i] = g_el[i];
    }
    omx_flanger_process(g_el, g_er, CLEN, &p, &s);
    double worst = 0.0;
    for (uint32_t i = 1; i < CLEN; i++) {
      const double step = fabs((double)g_el[i] - (double)g_el[i - 1]);
      if (step > worst) worst = step;
    }
    char what[200];
    snprintf(what, sizeof what,
             "arm E, %.1f kHz: the deepest, fastest sweep steps no more than the tone does "
             "(bound %.6f)",
             sr / 1000.0, bound);
    ok(worst <= bound, what, worst, bound);

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
    printf("mix_flanger arm E, %.1f kHz: worst step %.6f, bound %.6f (nearest-sample control "
           "%.6f)\n",
           sr / 1000.0, worst, bound, zworst);
  }
  drain_violations("arm E: no contract violation");
}

/* ---- arm F: bypass identity ------------------------------------------------------------------ */

static float g_ref_l[BLEN], g_ref_r[BLEN];

static void arm_f_bypass(void) {
  for (uint32_t i = 0; i < BLEN; i++) {
    g_ref_l[i] = (float)((i % 7u == 0u) ? 1e-40 : sin((double)i * 0.1) * 0.7);
    g_ref_r[i] = (float)((i % 5u == 0u) ? -3e-41 : cos((double)i * 0.07) * 0.6);
  }
  struct omx_flanger_state s;
  freeze(&s, 0.4);
  struct omx_flanger p = atom(96000.0, OMX_FLANGER_BASE_MS, 2.0, 0.6, 0.5);
  p.lfo_inc = omx_lfo_inc(0.25f, 96000.0f);

  p.enabled = 0;
  memcpy(g_bl, g_ref_l, sizeof g_bl);
  memcpy(g_br, g_ref_r, sizeof g_br);
  omx_flanger_process(g_bl, g_br, BLEN, &p, &s);
  ok(memcmp(g_bl, g_ref_l, sizeof g_bl) == 0 && memcmp(g_br, g_ref_r, sizeof g_br) == 0,
     "arm F: a disabled flanger is the input, bit for bit", 0.0, 0.0);
  ok(s.lfo.phase == 0.4f && s.fb_l == 0.0f, "arm F: and does not move a state word",
     (double)s.lfo.phase, 0.4);

  p.enabled = 1;
  p.mix = 0.0f;
  memcpy(g_bl, g_ref_l, sizeof g_bl);
  memcpy(g_br, g_ref_r, sizeof g_br);
  omx_flanger_process(g_bl, g_br, BLEN, &p, &s);
  ok(memcmp(g_bl, g_ref_l, sizeof g_bl) == 0 && memcmp(g_br, g_ref_r, sizeof g_br) == 0,
     "arm F: mix 0 is the input, bit for bit, below the flush floor included", 0.0, 0.0);

  p.mix = 0.01f;
  memcpy(g_bl, g_ref_l, sizeof g_bl);
  memcpy(g_br, g_ref_r, sizeof g_br);
  omx_flanger_process(g_bl, g_br, BLEN, &p, &s);
  ok(memcmp(g_bl, g_ref_l, sizeof g_bl) != 0,
     "arm F control: a 1% mix is NOT the input — the comparison can see a difference", 0.0, 0.0);
  drain_violations("arm F: no contract violation");
}

/* ---- G: the gain laws carry the read kernel's overshoot (F6, spec §6) ---------------------- */

/*
 * The Lagrange-3 read is not convex: at a half-sample fraction its taps are
 * [-1/16, 9/16, 9/16, -1/16], Σ|c| = Λ = 1.25, so a full-scale pattern whose signs line up with
 * the taps' — [-1, 1, 1, -1], a tone at fs/4 — reads back at Λ (+1.9 dB) from a stream that never
 * leaves ±1. The laws (spec §6) carry Λ, read from the generated header: the chorus's
 * (1 − mix) + mix·Λ and the flanger's (1 − mix) + mix·Λ/(1 − Λ|fb|). Setting Λ to 1.0 in the
 * header reds this arm (the sabotage the F6 lane ran).
 */
static void chorus_flanger_stay_inside_their_bound(void) {
  static const float PAT[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
  enum { N = 8192 };
  static float l[N], r[N];
  static float ca[OMX_CHORUS_CAP], cb[OMX_CHORUS_CAP];
  for (int ri = 0; ri < 4; ri++) {
    const float sr = (float)RATES[ri];
    /* CHORUS: one voice, all wet, the oscillator frozen at the turn's zero so the sweep sits at
     * base + depth/2 — one sample of depth puts the read exactly half a sample off a tap. */
    for (int ctl = 0; ctl < 2; ctl++) {
      /* ctl 1 — CONTROL: depth 0 reads a WHOLE sample at 48/96/192 kHz (base = 480/960/1920),
       * the exact-copy branch. `* sr / 1000` and NOT the resolve's `* 0.001f * sr`: 0.001f is not
       * exact, so the resolve hands the line 480.00003 at 48 kHz (the audit's F6 aside). */
      if (ctl == 1 && ri == 0) continue;
      memset(ca, 0, sizeof ca);
      memset(cb, 0, sizeof cb);
      struct omx_chorus_state s;
      omx_chorus_state_init(&s, ca, cb, OMX_CHORUS_CAP);
      struct omx_chorus p = {1, 1,
                             ctl ? OMX_CHORUS_BASE_MS * sr / 1000.0f
                                 : OMX_CHORUS_BASE_MS * 0.001f * sr,
                             ctl ? 0.0f : 1.0f, 0.0f, 1.0f, 0.0f};
      for (int i = 0; i < N; i++) l[i] = r[i] = PAT[i & 3];
      omx_chorus_process(l, r, N, &p, &s);
      float pk = 0.0f;
      for (int i = N / 2; i < N; i++) pk = fmaxf(pk, fabsf(l[i]));
      const float bound = ctl ? 1.0f : omx_chorus_gain_bound(&p);
      char what[160];
      snprintf(what, sizeof what,
               ctl ? "arm G %.0f Hz: CONTROL a whole-sample chorus read adds no gain"
                   : "arm G %.0f Hz: chorus stays inside omx_chorus_gain_bound (peak out vs bound)",
               (double)sr);
      ok(pk <= bound + 1e-6f, what, pk, bound);
      if (!ctl && ri == 1)
        printf("mix_flanger arm G: chorus half-sample fs/4 peak %.6f, bound %.6f (Λ %.4f)\n",
               (double)pk, (double)bound, (double)OMX_FDELAY_READ_L1_NORM);
    }
    /* FLANGER: the same read, feedback 0 and -0.2. */
    for (int f = 0; f < 2; f++) {
      struct omx_flanger_state s;
      freeze(&s, 0.0);
      struct omx_flanger p = {1, OMX_FLANGER_BASE_MS * 0.001f * sr, 1.0f, 0.0f,
                              f == 0 ? 0.0f : -0.2f, 1.0f};
      for (int i = 0; i < N; i++) l[i] = r[i] = PAT[i & 3];
      omx_flanger_process(l, r, N, &p, &s);
      float pk = 0.0f;
      for (int i = N / 2; i < N; i++) pk = fmaxf(pk, fabsf(l[i]));
      const float bound = omx_flanger_gain_bound(&p);
      char what[160];
      snprintf(what, sizeof what,
               "arm G %.0f Hz: flanger fb %.1f stays inside omx_flanger_gain_bound (peak out vs bound)",
               (double)sr, (double)p.feedback);
      ok(pk <= bound + 1e-6f, what, pk, bound);
    }
  }
  /* EVIDENCE for the law's shape: a STATIC half-sample read at fb ±0.5 has an impulse-response
   * ℓ1 norm (the exact worst-case peak gain of an LTI stage) ABOVE the ruled Λ/(1 − |fb|), and
   * inside the proven Λ/(1 − Λ|fb|). */
  for (int sgn = -1; sgn <= 1; sgn += 2) {
    struct omx_flanger p = atom(48000.0, OMX_FLANGER_BASE_MS, 0.0, 0.5 * sgn, 1.0);
    p.depth_samples = 1.0f;
    impulse_response(&p, 0.0);
    double l1 = 0.0;
    for (uint32_t i = 0; i < IRLEN; i++) l1 += fabs((double)g_l[i]);
    const double ruled = (double)OMX_FDELAY_READ_L1_NORM / (1.0 - 0.5);
    char what[160];
    snprintf(what, sizeof what, "arm G: fb %+.1f static half-sample l1 exceeds the ruled Λ/(1-|fb|)",
             0.5 * sgn);
    ok(l1 > ruled, what, l1, ruled);
    snprintf(what, sizeof what, "arm G: fb %+.1f static half-sample l1 inside the published bound",
             0.5 * sgn);
    ok(l1 <= (double)omx_flanger_gain_bound(&p), what, l1, (double)omx_flanger_gain_bound(&p));
    printf("mix_flanger arm G: fb %+.1f static half-sample l1 %.6f (ruled %.4f, published %.4f)\n",
           0.5 * sgn, l1, ruled, (double)omx_flanger_gain_bound(&p));
  }
  drain_violations("arm G: no contract violation");
}

int main(void) {
  omx_fx_require_rate_floor();
  arm_a_delay_follows();
  arm_b_resonance();
  arm_c_clamp();
  arm_d_tail_decays();
  arm_e_no_click();
  arm_f_bypass();
  chorus_flanger_stay_inside_their_bound();
  printf("mix_flanger: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
