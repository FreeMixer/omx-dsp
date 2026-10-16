// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/fx/omx_delay.h>
#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/fx/omx_reverb.h>

#include "../fx/fx_rates.h"

/* ---- one-pole: the convention oracle, moved from openmixer's mix_onepole.test.c -------
 * The parameter is the POLE: `y = x(1-p) + yp`. The arms below are the engine's convention oracle,
 * closed forms and tolerances unchanged; the rate-dependent ones run at the nine RME rates
 * (OMX_FX_RME_RATES), each rate's ledger drained by omx_fx_drain_ledger (a `rate-is-declared` PRE at
 * 32, 64 or 128 kHz is the contract saying so; any other violation counts). */

/* Bit equality, not near equality: `==` would let a signed zero through and a tolerance would let
 * the whole question through. */
static int onepole_same_bits(float a, float b) { return memcmp(&a, &b, sizeof a) == 0; }

static void onepole_drain(float sr) {
#ifdef OMX_CONTRACTS
  const uint32_t unexplained = omx_fx_drain_ledger(sr);
  ok(unexplained == 0u, "no contract violation the rate does not explain", unexplained, 0.0);
#else
  (void)sr;
#endif
}

/* UNITY DC GAIN AT EVERY POLE, as a LIMIT (run to convergence), both kernels. `y = x(1-p) + yp` is a
 * convex combination, so a constant converges to that constant whatever p < 1 is. The tolerance is a
 * CLOSED FORM: a float integrator stalls when `(1-p)(x-s) < ulp(x)`, leaving `s` short of `x` by up to
 * `ulp(x)/(1-p)` — tol = 1e-6 + 4·2^-24/(1-p) for x = 0.625 in [0.5, 1). Rate-independent (a pole). */
static void onepole_dc_gain(void) {
  const float X = 0.625f;
  const double ULP = 5.96046448e-8; /* 2^-24, the ULP of a float in [0.5, 1) */
  const float POLE[7] = { 0.0f, 0.1f, 0.5f, 0.9f, 0.98f, 0.999f, 0.99999f };
  for (int i = 0; i < 7; i++) {
    const double tol = 1e-6 + 4.0 * ULP / (1.0 - (double)POLE[i]);
    float s = 0.0f, t = 0.0f;
    for (int k = 0; k < 4000000; k++) omx_onepole(&s, X, POLE[i]);
    for (int k = 0; k < 4000000; k++) omx_onepole_toward(&t, X, POLE[i]);
    ok(fabs(s - X) <= tol, "omx_onepole passes DC at unity, to the float stall bound", s, X);
    ok(fabs(t - X) <= tol, "omx_onepole_toward passes DC at unity, to the same bound", t, X);
  }
  /* The convention stated as the thing it replaced: the COEFFICIENT spelling `y += c(x - y)` at c = 0
   * never moves (the reverb plate with no tail); the POLE spelling at the same knob value is a wire. */
  float wrong = 0.0f, right = 0.0f;
  const float damping = 0.0f; /* the knob at its OPEN end */
  for (int k = 0; k < 100000; k++) wrong += damping * (0.625f - wrong); /* THE DEFECT THAT SHIPPED */
  for (int k = 0; k < 100000; k++) omx_onepole(&right, 0.625f, damping);
  ok(wrong == 0.0f, "the COEFFICIENT spelling at damping 0 has DC gain 0 — a dead loop", wrong, 0.0);
  ok(right == 0.625f, "the POLE spelling at the same knob value passes DC — unity, exactly", right, 0.625);
  /* pole 0 is a WIRE, exactly, in one sample and in both kernels */
  float s = 3.0f, t = 3.0f;
  ok(onepole_same_bits(omx_onepole(&s, -1.25f, 0.0f), -1.25f), "pole 0 is a wire (convex form), exactly", s, -1.25);
  ok(onepole_same_bits(omx_onepole_toward(&t, -1.25f, 0.0f), -1.25f), "pole 0 is a wire (increment form), exactly", t,
     -1.25);
}

/* THE TIME CONSTANT IS THE TIME CONSTANT: the step through omx_pole_from_time_ms(ms, sr) covers
 * 1 - 1/e after round(ms·sr/1000) samples (±2e-3), for 0.5/5/50/250 ms; and the rate cancels — the
 * tau recovered from the pole, -1/(ln(p)·sr), is the 50 ms it was made from (±1e-4 s). */
static void onepole_time_constant_rme(void) {
  const float MS[4] = { 0.5f, 5.0f, 50.0f, 250.0f };
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    for (int m = 0; m < 4; m++) {
      const float pole = omx_pole_from_time_ms(MS[m], sr);
      const uint32_t n = (uint32_t)(MS[m] * 0.001f * sr + 0.5f);
      float s = 0.0f;
      for (uint32_t k = 0; k < n; k++) omx_onepole(&s, 1.0f, pole);
      ok(fabs(s - (1.0 - 1.0 / M_E)) <= 2e-3, "the step is 1-1/e after one time constant", s, 1.0 - 1.0 / M_E);
    }
    const float p50 = omx_pole_from_time_ms(50.0f, sr);
    const double tau = -1.0 / (logf(p50) * sr);
    ok(fabs(tau - 0.05) <= 1e-4, "the pole's tau, recovered from the pole, is the 50 ms it was made from", tau, 0.05);
    ok(onepole_same_bits(omx_pole_from_time_ms(0.0f, sr), 0.0f), "0 ms is a wire, not a divide by zero",
       omx_pole_from_time_ms(0.0f, sr), 0.0);
    ok(onepole_same_bits(omx_pole_from_time_ms(-3.0f, sr), 0.0f), "a negative time is a wire, refused quietly",
       omx_pole_from_time_ms(-3.0f, sr), 0.0);
    ok(onepole_same_bits(omx_pole_from_cutoff_hz(0.0f, sr), 0.0f), "a 0 Hz corner is a wire, not a filter",
       omx_pole_from_cutoff_hz(0.0f, sr), 0.0);
    onepole_drain(sr);
  }
}

/* The settled peak amplitude of a sine at `hz` through a one-pole of `pole` at `sr` (second half of 0.2 s). */
static double onepole_settled_gain(float hz, float pole, float sr) {
  float s = 0.0f;
  double peak = 0.0;
  const uint32_t n = (uint32_t)(sr * 0.2f);
  for (uint32_t k = 0; k < n; k++) {
    const float y = omx_onepole(&s, sinf(2.0f * (float)M_PI * hz * (float)k / sr), pole);
    if (k > n / 2u && fabs((double)y) > peak) peak = fabs((double)y);
  }
  return peak;
}

/* THE CORNER IS THE CORNER where the impulse-invariant mapping p = exp(-2π·fc/sr) holds (fc/sr <= 1/20):
 * the measured response at fc is -3 dB ±0.12; and the corner recovered by -ln(p)·sr/2π is 1000 Hz ±1e-2.
 * THE WARP, as a number: at 44.1 kHz ONLY, because that is the rig rate where the reverb's high cut can
 * legitimately be asked for 16 kHz (fc/sr 0.36); a 4/8/16 kHz corner answers between -3.02 and -1.0 dB
 * there. The mapping is not to be fixed here: changing it moves every shipped wet-cut coefficient. */
static void onepole_corner_rme(void) {
  const float FC[3] = { 200.0f, 1000.0f, 2000.0f };
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    for (int f = 0; f < 3; f++) {
      if (FC[f] / sr > 0.05f) continue;
      const double db = 20.0 * log10(onepole_settled_gain(FC[f], omx_pole_from_cutoff_hz(FC[f], sr), sr));
      ok(fabs(db + 3.0) <= 0.12, "the measured response at the corner is -3 dB", db, -3.0);
    }
    const float p = omx_pole_from_cutoff_hz(1000.0f, sr);
    const double fc = -logf(p) * sr / (2.0f * (float)M_PI);
    ok(fabs(fc - 1000.0) <= 1e-2, "the corner recovered from the pole is the corner it was made from", fc, 1000.0);
    onepole_drain(sr);
  }
  const float WARP_FC[3] = { 4000.0f, 8000.0f, 16000.0f };
  for (int f = 0; f < 3; f++) {
    const double g = 20.0 * log10(onepole_settled_gain(WARP_FC[f], omx_pole_from_cutoff_hz(WARP_FC[f], 44100.0f), 44100.0f));
    printf("omxdsp_suite onepole: corner warp, %g Hz asked at 44100 Hz answers %.3f dB (nominal -3.010)\n",
           (double)WARP_FC[f], g);
    ok(g > -3.02 && g < -1.0, "the warp at 44100 Hz is bounded: between -3.02 and -1.0 dB", g, -1.0);
  }
}

/* WHY THERE ARE TWO KERNELS. The increment form is EXACT at its fixed point (`target - x` is exactly 0,
 * so `+= 0` cannot move the state, whatever the pole); the convex form recomputes `x(1-p) + xp`, which
 * for some (x, pole) is not exactly x. A SWEEP, not one pair: 400 x (0.00713 .. 2.852) × 4 poles × 64
 * steps at every RME rate. Were convex_moved ever 0 the two kernels could be merged. They are still the
 * SAME FILTER: from a cold start on a sine, 20000 samples at the 5 ms pole, they agree within 1e-5. */
static void onepole_fixed_point(void) {
  int toward_moved = 0, convex_moved = 0, pairs = 0;
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    const float POLE[4] = { omx_pole_from_time_ms(50.0f, sr), omx_pole_from_time_ms(0.5f, sr),
                            omx_pole_from_cutoff_hz(200.0f, sr), 0.75f };
    for (int pi = 0; pi < 4; pi++) {
      for (int xi = 1; xi <= 400; xi++) {
        const float x = (float)xi * 0.00713f; /* nothing round */
        float t = x, s = x;
        for (int k = 0; k < 64; k++) { omx_onepole_toward(&t, x, POLE[pi]); omx_onepole(&s, x, POLE[pi]); }
        pairs++;
        if (!onepole_same_bits(t, x)) toward_moved++;
        if (!onepole_same_bits(s, x)) convex_moved++;
      }
    }
    const float pole = omx_pole_from_time_ms(5.0f, sr);
    float a = 0.0f, b = 0.0f, worst = 0.0f;
    for (int k = 0; k < 20000; k++) {
      const float x = sinf((float)k * 0.01f) * 0.5f;
      const float d = fabsf(omx_onepole(&a, x, pole) - omx_onepole_toward(&b, x, pole));
      if (d > worst) worst = d;
    }
    ok(worst <= 1e-5f, "the two kernels are the same filter to float precision", worst, 1e-5);
    onepole_drain(sr);
  }
  printf("omxdsp_suite onepole: fixed point over %d (x, pole) pairs: increment form moved %d, convex form moved %d\n",
         pairs, toward_moved, convex_moved);
  ok(toward_moved == 0, "omx_onepole_toward is EXACT at its fixed point for EVERY (x, pole)", toward_moved, 0.0);
  ok(convex_moved > 0, "omx_onepole is not — which is why the drive's auto-gain uses the other one", convex_moved, 1.0);
}

/* THE 2026-09-17 FOLD MOVED NO COEFFICIENT. Each folded site's PRE-FOLD expression, re-spelled verbatim,
 * against what the tier's kernel computes now: BIT equality at every RME rate. */
static void onepole_folded_sites(void) {
  int env_ok = 1, ag_ok = 1, dyn_ok = 1, agp_ok = 1, cut_ok = 1, rev_ok = 1, flush_seen = 0, tone_ok = 1, damp_ok = 1;
  const float MS[5] = { 0.1f, 1.0f, 12.5f, 100.0f, 1000.0f };
  const float FC[5] = { 20.0f, 200.0f, 1000.0f, 8000.0f, 16000.0f };
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    /* (a) omx_env_step: `stage[s] = coeff*stage[s] + (1-coeff)*d` */
    const float ac = omx_pole_from_time_ms(3.0f, sr);
    float env_was = 0.0f, env_now = 0.0f;
    for (int k = 0; k < 20000; k++) {
      const float d = fabsf(sinf((float)k * 0.013f)) * (k % 977 == 0 ? 4.0f : 1.0f);
      env_was = ac * env_was + (1.0f - ac) * d; /* THE FORMER SPELLING */
      omx_onepole(&env_now, d, ac);
      if (!onepole_same_bits(env_was, env_now)) env_ok = 0;
    }
    /* (b) the drive's auto-gain detectors: `x += (1-a)*(target - x)`, a = the 50 ms pole ^ 128 */
    const float a = powf(omx_pole_from_time_ms(OMX_DRIVE_AG_WINDOW_MS, sr), 128.0f);
    float was = 0.0f, now = 0.0f;
    for (int k = 0; k < 20000; k++) {
      const float tgt = 0.5f + 0.25f * sinf((float)k * 0.0007f);
      was += (1.0f - a) * (tgt - was); /* THE FORMER SPELLING */
      omx_onepole_toward(&now, tgt, a);
      if (!onepole_same_bits(was, now)) ag_ok = 0;
    }
    /* (c) resolve_dyn's attack/release: `a_ms <= 0 ? 0 : expf(-1/(a_ms*0.001f*sr))` */
    for (int m = 0; m < 5; m++) {
      const float p = MS[m] <= 0.0f ? 0.0f : expf(-1.0f / (MS[m] * 0.001f * sr));
      if (!onepole_same_bits(p, omx_pole_from_time_ms(MS[m], sr))) dyn_ok = 0;
    }
    /* (d) resolve_drive's auto-gain pole: `expf(-1/(0.05f*sr))`, the window then in SECONDS as a bare
     * literal, now OMX_DRIVE_AG_WINDOW_MS; `50.0f*0.001f` and `0.05f` are measured, not assumed, to be
     * the same float — through the constructor and through the site that now owns it
     * (omx_drive_time_constants). */
    struct omx_drive dr;
    memset(&dr, 0, sizeof dr);
    omx_drive_time_constants(&dr, sr);
    const float agp = expf(-1.0f / (0.05f * sr)); /* THE FORMER SPELLING */
    if (!onepole_same_bits(agp, omx_pole_from_time_ms(OMX_DRIVE_AG_WINDOW_MS, sr))) agp_ok = 0;
    if (!onepole_same_bits(agp, dr.ag_coeff)) agp_ok = 0;
    /* (e) the reverb's wet-path cuts: `expf(-2.0f*3.14159265f*fc/sr)` (3.14159265f is (float)M_PI) */
    for (int f = 0; f < 5; f++) {
      const float p = expf(-2.0f * 3.14159265f * FC[f] / sr); /* THE FORMER SPELLING */
      if (!onepole_same_bits(p, omx_pole_from_cutoff_hz(FC[f], sr))) cut_ok = 0;
    }
    /* (f) the reverb's per-sample filter: `omx_flush(x*(1-p) + s*p)` — the flush MUST survive
     * delegating the arithmetic to the tier's kernel; an impulse, then a decay into the subnormals. */
    const float pole = omx_reverb_damp_pole(0.6f, sr);
    float rw = 1.0f, rn = 1.0f;
    for (int k = 0; k < 400000; k++) {
      const float x = (k < 64) ? 1.0f : 0.0f;
      rw = omx_flush(x * (1.0f - pole) + rw * pole); /* THE FORMER SPELLING */
      omx_onepole_flush(&rn, x, pole);
      if (!onepole_same_bits(rw, rn)) rev_ok = 0;
      if (rw == 0.0f && k > 64) flush_seen = 1;
    }
    /* (g) the delay's tone pole and the reverb's damping pole, folded into omx_pole_rescale: each
     * PRE-FOLD expression, after its own clamp, over the knob's travel (0 .. 1 by 1/64, and 0.98) */
    for (int k = 0; k <= 65; k++) {
      const float knob = k == 65 ? 0.98f : (float)k / 64.0f;
      const float REF = (float)OMX_DSP_KNOB_REFERENCE_RATE;
      const float t = knob; /* inside [0, 1]: the delay's clamp is the identity */
      const float tone_was = (t <= 0.0f || t >= 1.0f || sr <= 0.0f || sr == REF) ? t : powf(t, REF / sr); /* FORMER */
      if (!onepole_same_bits(tone_was, omx_fxdelay_tone_pole(knob, sr))) tone_ok = 0;
      const float d = knob > OMX_REVERB_DAMP_POLE_MAX ? OMX_REVERB_DAMP_POLE_MAX : knob;
      const float damp_was = (d <= 0.0f || sr == REF) ? d : powf(d, REF / sr); /* THE FORMER SPELLING */
      if (!onepole_same_bits(damp_was, omx_reverb_damp_pole(knob, sr))) damp_ok = 0;
    }
    onepole_drain(sr);
  }
  ok(tone_ok, "the delay's tone pole is bit-identical to its pre-fold spelling", tone_ok, 1.0);
  ok(damp_ok, "the reverb's damping pole is bit-identical to its pre-fold spelling", damp_ok, 1.0);
  ok(env_ok, "the dynamics detector's envelope is bit-identical to its pre-fold spelling", env_ok, 1.0);
  ok(ag_ok, "the drive's auto-gain detectors are bit-identical to their pre-fold spelling", ag_ok, 1.0);
  ok(dyn_ok, "attack/release poles are bit-identical to resolve_dyn's pre-fold expression", dyn_ok, 1.0);
  ok(agp_ok, "the auto-gain pole is bit-identical to resolve_drive's pre-fold 0.05f expression", agp_ok, 1.0);
  ok(cut_ok, "the reverb's low/high-cut poles are bit-identical to their pre-fold expression", cut_ok, 1.0);
  ok(rev_ok, "omx_onepole_flush is bit-identical to its pre-fold spelling, flush included", rev_ok, 1.0);
  ok(flush_seen, "and the decay actually REACHED the flush threshold (the arm observes the flush)", flush_seen, 1.0);
}

/* A POLE QUOTED AT A REFERENCE RATE KEEPS ITS TIME AND ITS CORNER AT EVERY RATE (omx_pole_rescale). The
 * reference is the knob's own (OMX_DSP_KNOB_REFERENCE_RATE). Closed forms, in double: the time constant
 * recovered from the rescaled pole, -1/(sr·ln q), is the one the reference pole names, -1/(ref·ln p), to the
 * float's own bound 2·2^-24/(1-q) + 2·2^-24/(1-p) — for a 5 ms and a 250 ms time; the corner -ln(q)·sr/2π
 * is the 1 kHz the reference pole was made from, to 0.1 Hz; and the MEASURED settled response at that
 * corner is -3 dB at every rate where the corner sits below sr/20. Then the four fixed points, bit for
 * bit: 0, 1, the reference rate itself and a rate not above 0 (0, negative, NaN). */
static void onepole_rescale_rme(void) {
  const float REF = (float)OMX_DSP_KNOB_REFERENCE_RATE;
  const float MS[2] = { 5.0f, 250.0f };
  const float SWEEP[5] = { 0.0f, 0.25f, 0.6f, 0.98f, 1.0f };
  const double ULP = 5.96046448e-8; /* 2^-24, the ULP of a float in [0.5, 1) */
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    for (int m = 0; m < 2; m++) {
      const float p = omx_pole_from_time_ms(MS[m], REF);
      const float q = omx_pole_rescale(p, REF, sr);
      const double tau_q = -1.0 / ((double)sr * log((double)q)), tau_p = -1.0 / ((double)REF * log((double)p));
      /* the bound is the float's, in closed form: a pole near 1 stores ln(p) ≈ p - 1 to an ulp of p, so each
       * pole read back as a time carries up to 2·2^-24/(1-p) of relative error (its rounding and powf's) */
      const double tol = 1e-6 + 2.0 * ULP / (1.0 - (double)q) + 2.0 * ULP / (1.0 - (double)p);
      ok(fabs(tau_q / tau_p - 1.0) <= tol, "a rescaled pole keeps its time constant in seconds",
         tau_q / tau_p - 1.0, tol);
    }
    const float pc = omx_pole_from_cutoff_hz(1000.0f, REF);
    const float qc = omx_pole_rescale(pc, REF, sr);
    const double fc = -log((double)qc) * sr / (2.0 * M_PI);
    ok(fabs(fc - 1000.0) <= 0.1, "a rescaled pole keeps its corner in Hz", fc, 1000.0);
    if (1000.0f / sr <= 0.05f) {
      const double db = 20.0 * log10(onepole_settled_gain(1000.0f, qc, sr));
      ok(fabs(db + 3.0) <= 0.12, "the measured response of the rescaled pole at that corner is -3 dB", db, -3.0);
    }
    int stable = 1;
    for (int k = 0; k < 5; k++) {
      const float q = omx_pole_rescale(SWEEP[k], REF, sr);
      if (!(q >= 0.0f && q <= 1.0f)) stable = 0;
    }
    ok(stable, "every pole in [0, 1] stays in [0, 1]", stable, 1.0);
    ok(onepole_same_bits(omx_pole_rescale(0.0f, REF, sr), 0.0f) &&
           onepole_same_bits(omx_pole_rescale(1.0f, REF, sr), 1.0f),
       "0 and 1 are fixed points", omx_pole_rescale(1.0f, REF, sr), 1.0);
    ok(onepole_same_bits(omx_pole_rescale(0.6f, sr, sr), 0.6f), "the reference rate is a fixed point, bit for bit",
       omx_pole_rescale(0.6f, sr, sr), 0.6);
    ok(onepole_same_bits(omx_pole_rescale(0.6f, REF, 0.0f), 0.6f) &&
           onepole_same_bits(omx_pole_rescale(0.6f, REF, -sr), 0.6f) &&
           onepole_same_bits(omx_pole_rescale(0.6f, REF, NAN), 0.6f),
       "a rate not above 0 (0, negative, NaN) leaves the pole as it is", omx_pole_rescale(0.6f, REF, 0.0f), 0.6);
    onepole_drain(sr);
  }
}

/* THE ONE SITE THE FOLD DID NOT TOUCH: the drive's DC blocker takes its pole as the first-order
 * approximation `1 - 2π·5/sr` rather than exp(-2π·5/sr) — same CONVENTION, different VALUE. Its corner,
 * -ln(p)·sr/2π, is within 0.01 Hz of 5 Hz at every rate; the approximation is what
 * omx_drive_time_constants computes, bit for bit; and it is a DIFFERENT float from the tier constructor's
 * at the low rates and the SAME float at the high ones (folding it would move a rig-rate coefficient — a
 * finding for a ruling, not a fold). */
static void onepole_dc_blocker(void) {
  int first_same = -1, monotone = 1, rig_differs = 1;
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    const float approx = 1.0f - 2.0f * 3.14159265f * 5.0f / sr;
    const float exact = omx_pole_from_cutoff_hz(5.0f, sr);
    const double c_approx = -log((double)approx) * (double)sr / (2.0 * M_PI);
    struct omx_drive dr;
    memset(&dr, 0, sizeof dr);
    omx_drive_time_constants(&dr, sr);
    printf("omxdsp_suite onepole: DC blocker pole at %6g Hz: approx %.9f exact %.9f %s (corner %.6f Hz)\n",
           (double)sr, (double)approx, (double)exact, onepole_same_bits(approx, exact) ? "SAME float" : "differs",
           c_approx);
    ok(onepole_same_bits(dr.dc_coeff, approx), "omx_drive_time_constants's DC pole is the approximation", dr.dc_coeff,
       approx);
    ok(fabs(c_approx - 5.0) <= 0.01, "the DC blocker's approximated corner is within 0.01 Hz of 5 Hz", c_approx, 5.0);
    const int differs = !onepole_same_bits(approx, exact);
    if (!differs && first_same < 0) first_same = (int)ri;
    if (differs && first_same >= 0) monotone = 0; /* OMX_FX_RME_RATES ascends */
    if (differs == 0 && (sr == 44100.0f || sr == 48000.0f || sr == 96000.0f)) rig_differs = 0;
    onepole_drain(sr);
  }
  /* The engine's predicate, verbatim and at the engine's rates (OMX_DECLARED_RATES): the flip sits at half
   * the fastest DECLARED rate. It is a fit to the declared set, not a closed form: at the nine RME rates
   * 128 kHz (above half of 192 kHz) still differs and the first agreeing rate is 176.4 kHz, so on the
   * RME set the arm asserts what does carry over — the floats differ at every rate below some flip and
   * agree at every rate above it, and the rig rates 44.1 / 48 / 96 kHz are on the differing side. */
  float fastest = OMX_DECLARED_RATES[0];
  for (uint32_t ri = 1; ri < OMX_DECLARED_RATE_COUNT; ri++)
    if (OMX_DECLARED_RATES[ri] > fastest) fastest = OMX_DECLARED_RATES[ri];
  const float flip = 0.5f * fastest;
  int below = 0, above = 0, wrong = 0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    const int differs = !onepole_same_bits(1.0f - 2.0f * 3.14159265f * 5.0f / sr, omx_pole_from_cutoff_hz(5.0f, sr));
    if (sr <= flip) { below++; if (!differs) wrong++; }
    else { above++; if (differs) wrong++; }
  }
  ok(below > 0 && above > 0 && wrong == 0,
     "on the declared set the approximation differs up to half the fastest rate and agrees above it", wrong, 0.0);
  ok(first_same > 0 && monotone, "on the RME set it differs below one flip and agrees at every rate above it",
     first_same, 0.0);
  ok(rig_differs, "and at the rig rates 44.1 / 48 / 96 kHz it is a different float (folding moves a value)",
     rig_differs, 1.0);
}

/* ---- one-pole ------------------------------------------------------------------------------- */

static void arm_onepole(void) {
  g_arm = "onepole";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    /* time constant: the step response covers 1 - 1/e after tau */
    const float tau_ms = 5.0f;
    const float p = omx_pole_from_time_ms(tau_ms, sr);
    ok(p > 0.0f && p < 1.0f, "a positive time gives a pole inside (0, 1)", p, 1.0);
    float y = 0.0f;
    const uint32_t n_tau = (uint32_t)(tau_ms * 0.001f * sr + 0.5f);
    for (uint32_t i = 0; i < n_tau; i++) omx_onepole(&y, 1.0f, p);
    ok(fabsf(y - (1.0f - expf(-1.0f))) < 2e-3f, "the step response covers 1-1/e after one time constant", y, 1.0 - exp(-1.0));
    /* corner: |H| at fc for the impulse-invariant pole is (1-p)/sqrt(1 - 2p cos w + p^2) */
    const float fc = 1000.0f;
    const float pc = omx_pole_from_cutoff_hz(fc, sr);
    const double w = 2.0 * M_PI * fc / sr;
    const double closed = (1.0 - pc) / sqrt(1.0 - 2.0 * pc * cos(w) + (double)pc * pc);
    float st = 0.0f;
    double re = 0.0, im = 0.0;
    const uint32_t settle = (uint32_t)(sr * 0.2f), meas = (uint32_t)(sr * 0.1f);
    for (uint32_t i = 0; i < settle + meas; i++) {
      const float x = sinf((float)(w * i));
      const float o = omx_onepole(&st, x, pc);
      if (i >= settle) { re += o * cos(w * i); im += o * sin(w * i); }
    }
    const double mag = 2.0 * sqrt(re * re + im * im) / meas;
    ok(fabs(mag - closed) < 2e-3, "the corner's magnitude matches the closed form", mag, closed);
    ok(fabs(20.0 * log10(closed) + 3.0) < 0.2, "the corner sits near -3 dB", 20.0 * log10(closed), -3.0);
    /* wires and exactness */
    ok(omx_pole_from_time_ms(0.0f, sr) == 0.0f, "ms <= 0 is a wire", omx_pole_from_time_ms(0.0f, sr), 0.0);
    ok(omx_pole_from_cutoff_hz(-1.0f, sr) == 0.0f, "hz <= 0 is a wire", omx_pole_from_cutoff_hz(-1.0f, sr), 0.0);
    float wire = 0.25f;
    ok(omx_onepole(&wire, 0.75f, 0.0f) == 0.75f, "pole 0 passes the input through", wire, 0.75);
    float held = 0.3125f;
    omx_onepole_toward(&held, 0.3125f, p);
    ok(held == 0.3125f, "the increment form holds a state equal to its target bit for bit", held, 0.3125);
    float conv = 0.0f, incr = 0.0f;
    for (int i = 0; i < 100; i++) { omx_onepole(&conv, 0.8f, p); omx_onepole_toward(&incr, 0.8f, p); }
    ok(fabsf(conv - incr) < 1e-5f, "both forms are the same filter", conv, incr);
    int same = 1;
    float a = 0.0f, b = 0.0f;
    for (uint32_t i = 0; i < 4096; i++) {
      const float x = (i & 256u) ? 0.5f : -0.25f;
      const float want = omx_flush(omx_onepole(&b, x, p));
      b = want;
      if (omx_onepole_flush(&a, x, p) != want || a != b) same = 0;
    }
    ok(same, "the flushed one-pole is omx_flush(omx_onepole()) bit for bit", same, 1.0);
    float tail = 1e-19f;
    for (uint32_t i = 0; i < 64; i++) omx_onepole_flush(&tail, 0.0f, 0.5f);
    ok(tail == 0.0f, "a decaying state flushes to exactly 0, never a subnormal", tail, 0.0);
  }
  expect_clean();
  onepole_dc_gain();
  onepole_time_constant_rme();
  onepole_corner_rme();
  onepole_fixed_point();
  onepole_folded_sites();
  onepole_rescale_rme();
  onepole_dc_blocker();
  expect_clean();
}
