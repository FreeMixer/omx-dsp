// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_flanger.h — the native FLANGER stage: one modulated fractional delay with FEEDBACK.
 *
 * Spec: docs/design/specs/2026-09-22-chorus-and-flanger.md (openmixer#860). The primitives are
 * `mix_fdelay.h` at order 3 and `mix_lfo.h`, the same two the chorus takes; the difference is the
 * feedback path and the delay region, and those are the declared facts that make this a stage of
 * its own rather than a mode of the chorus (spec §1, answer 3).
 *
 * Same contract as omx_delay.h: NO PipeWire, NO napi, NO allocation. The rings are allocated by
 * the CALLER on insert; controls are a word-atomic snapshot resolved once per block.
 *
 * ## What it IS
 *
 *     d(t) = base + depth·(1 + s(φ))/2                (omx_lfo_sweep, unipolar, upward)
 *     w[n] = line( x[n] + fb·w[n−1] )   at d(t)
 *     y    = (1 − mix)·x + mix·w
 *
 * The feedback turns the chorus's gentle notches into a RESONANCE: where the loop comes back in
 * phase the wet path reaches 1/(1 − fb), and the peak of |H| stands at (1 − mix) + mix/(1 − fb) —
 * +4.86 dB at the default mix 0.5 / fb 0.6, +26.0 dB at the clamp.
 *
 * ## THE LOOP DELAY IS d + 1, AND IT IS DECLARED
 *
 * The feedback multiplies the PREVIOUS wet sample, which is one sample old: the recursion is
 * w[n] = x[n−d] + fb·w[n−d−1], so the closed form carries a z^-1 in its denominator and the
 * resonances sit at f = m/(d + 1), not m/d. That extra sample is the state word this stage needs
 * in order to be causal at all, and every implementation of a feedback comb has it. It is stated
 * here, measured by the oracle against that exact closed form, and published through
 * `omx_flanger_loop_delay` so a row says the same number the kernel runs — rather than being a
 * quiet 2% error between a spec's formula and a desk's sound.
 *
 * ## The four things this file is careful about
 *
 *  1. SIGNED FEEDBACK, ONE CONTROL. Negative feedback swaps the comb's peaks and notches (the
 *     hollow flange). A separate "invert" switch would be a second spelling of the sign.
 *  2. THE CLAMP IS DECLARED. |fb| ≤ OMX_FLANGER_FB_MAX, so the loop's gain is strictly below one
 *     and the ring always DECAYS (`tail-decays`); the bound the output may exceed its input by is
 *     computed from the control (`omx_flanger_gain_bound`, carrying the read kernel's ℓ1 norm Λ,
 *     proven for |fb| < 1/Λ) rather than restated as a number.
 *  3. THE FEEDBACK WORD IS FLUSHED. It is the one state word that can walk down to a subnormal on
 *     silence and make the desk slower the quieter it gets — the line's own read flushes, this
 *     closes the loop around it.
 *  4. BOTH LEGS, ONE PHASE, and no synthetic widening — the chorus's reason, unchanged: width is
 *     the BUS's fact (2026-08-07-one-summing-bus.md).
 */
#ifndef OMX_MIX_FLANGER_H
#define OMX_MIX_FLANGER_H

#include <stdint.h>

#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/omx_fdelay.h>
#include <omxdsp/omx_lfo.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_wetdry.h>
#include <omxdsp/omx_contract.h>

/** The Lagrange order this stage constructs — the fdelay ruling's §3 row. */
#define OMX_FLANGER_ORDER OMX_FDELAY_MOD_READ_ORDER

/* The base delay (OMX_FLANGER_BASE_MS) is declared in omx-contract: the shortest delay the sweep reaches (spec §3b). */
/** The deepest sweep the row may ask for, ms (spec §3b's `depth` ceiling). */
#define OMX_FLANGER_MAX_DEPTH_MS ((float)OMX_FLANGER_DEPTH_RANGE_MAX)
/** The feedback clamp. Strictly below one, so the loop decays; 0.95 is +26.0 dB of resonance at
 *  mix 1, which is as far as a flanger is musically asked to go. */
#define OMX_FLANGER_FB_MAX OMX_FLANGER_FEEDBACK_RANGE_MAX
/** Highest graph rate the ring is sized for. */
#define OMX_FLANGER_MAX_RATE OMX_RT_HARD_TARGET_RATE
/* The longest delay, whole ms (OMX_FLANGER_MAX_MS), is declared in omx-contract, an integer constant expression for the ring size below. */
/** Per-leg ring capacity, samples: the longest delay at the highest rate plus the kernel's reach. */
#define OMX_FLANGER_CAP ((uint32_t)(((OMX_FLANGER_MAX_RATE / 1000) * OMX_FLANGER_MAX_MS) + 4))

/** The resolved control atom — times already in SAMPLES at the live rate. */
struct omx_flanger {
  int enabled;         /* 0 → passthrough, before a sample or a state word is touched */
  float base_samples;  /* the shortest delay the sweep reaches */
  float depth_samples; /* peak-to-peak excursion above the base */
  /* The oscillator's speed in TURNS PER SAMPLE (`omx_lfo_inc`), for the chorus's reason. */
  float lfo_inc;
  float feedback;      /* −OMX_FLANGER_FB_MAX .. +OMX_FLANGER_FB_MAX, signed */
  float mix;           /* 0 = dry (bit-identical), 1 = wet only */
};

/** Caller-owned rings + RT-owned state. `fb_l`/`fb_r` are the previous wet samples — the one
 *  state word per leg the loop is made of, and the one that is flushed. */
struct omx_flanger_state {
  struct omx_fdelay line_l;
  struct omx_fdelay line_r;
  struct omx_lfo lfo;
  float fb_l;
  float fb_r;
};

/** Clamp the feedback into the declared bound. The row's travel is what REFUSES an out-of-range
 *  control; this is the enforcement half, at the last line before a recursion. */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "flanger/clamp"
static inline float omx_flanger_clamp_fb(float fb) {
  const float f = omx_clamp_or(fb, -OMX_FLANGER_FB_MAX, OMX_FLANGER_FB_MAX, 0.0f); /* non-finite: none */
  OMX_POST(f > -1.0f && f < 1.0f, "a-clamped-feedback-is-strictly-inside-unity");
  return f;
}
#undef OMX_CONTRACT_STAGE

/**
 * The most this stage can exceed its input by, as a LINEAR factor: (1 − mix) + mix·Λ/(1 − Λ|fb|),
 * Λ the read kernel's ℓ1 norm read from the generated header (spec §6). The feedback passes
 * through the read every round, so each round adds at most Λ·|fb| of the loop's own peak — a
 * geometric series, proven for ANY sweep of the fraction while Λ·|fb| < 1 (|fb| < 0.8).
 * The `output-le-input-plus` law's bound, DERIVED from the controls the operator set rather than
 * quoted as a number a doc has to be kept equal to.
 *
 * For Λ·|fb| ≥ 1 (0.8 ≤ |fb| ≤ OMX_FLANGER_FB_MAX) NO finite bound is proven — there is no
 * closed form — and this returns +INFINITY: no bound is claimed rather than a false one. The ring
 * still decays there (`tail-decays`); only the sample-peak bound is unproven.
 */
static inline float omx_flanger_gain_bound(const struct omx_flanger *p) {
  const float fb = omx_flanger_clamp_fb(p->feedback);
  const float a = fb < 0.0f ? -fb : fb;
  const float mix = omx_unit(p->mix);
  const float loop = OMX_FDELAY_READ_L1_NORM * a;
  if (loop >= 1.0f) return mix > 0.0f ? INFINITY : 1.0f;
  return (1.0f - mix) + mix * OMX_FDELAY_READ_L1_NORM / (1.0f - loop);
}

/** The RESONANT delay, in samples: the line's own delay plus the feedback word's one sample. The
 *  number a row publishes, and the one the oracle's closed form stands on. */
static inline float omx_flanger_loop_delay(float delay_samples) { return delay_samples + 1.0f; }

/** The stage's own effect time, in samples: the ends of the sweep. A READBACK, never a latency
 *  term — the dry path is direct and nothing downstream waits (spec §5). */
static inline float omx_flanger_shortest_delay(const struct omx_flanger *p) {
  return p->base_samples;
}
static inline float omx_flanger_longest_delay(const struct omx_flanger *p) {
  return p->base_samples + p->depth_samples;
}

/** Arm both legs over caller-owned rings, silence the feedback words, start the turn at zero. */
#define OMX_CONTRACT_STAGE "flanger/init"
static inline enum omx_fdelay_code omx_flanger_state_init(struct omx_flanger_state *s,
                                                          float *ring_l, float *ring_r,
                                                          uint32_t cap) {
  omx_lfo_start(&s->lfo);
  s->fb_l = 0.0f;
  s->fb_r = 0.0f;
  enum omx_fdelay_code c = omx_fdelay_init(&s->line_l, ring_l, cap, OMX_FLANGER_ORDER);
  if (c != OMX_FDELAY_OK) return c;
  c = omx_fdelay_init(&s->line_r, ring_r, cap, OMX_FLANGER_ORDER);
  OMX_POST(c != OMX_FDELAY_OK || s->line_l.order == OMX_FLANGER_ORDER,
           "an-armed-flanger-reads-at-order-three");
  return c;
}
#undef OMX_CONTRACT_STAGE

/** The delay the sweep is at, at the oscillator's current phase — one place, as the chorus has. */
#define OMX_CONTRACT_STAGE "flanger/sweep"
static inline float omx_flanger_delay(const struct omx_flanger *p, const struct omx_lfo *lfo) {
  return omx_lfo_sweep(p->base_samples, p->depth_samples, omx_lfo_at(lfo, 0.0f));
}
#undef OMX_CONTRACT_STAGE

/**
 * Process one block IN PLACE. `l`/`r` are the strip's two legs; a disabled atom, a NULL ring or an
 * unarmed line is a passthrough. RT-safe: one kernel evaluation of four taps per leg per sample,
 * no allocation, no libm in the loop.
 */
#define OMX_CONTRACT_STAGE "flanger"
static inline void omx_flanger_process(float *l, float *r, uint32_t n, const struct omx_flanger *p,
                                       struct omx_flanger_state *s) {
  /* CONTRACT (omx_contract.h). A flanger is a feedback comb whose loop gain is strictly below one
   * — that bound is the whole reason the ring decays instead of running away, and it is a claim
   * about the CONTROL thread's travel, enforced by the clamp below and stated here because here
   * is where breaking it is audible. The output may exceed its input, by at most
   * `omx_flanger_gain_bound` (the `output-le-input-plus` law), and by nothing more. */
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
  OMX_PRE(p->mix >= 0.0f && p->mix <= 1.0f, "mix-in-unit-range");
  OMX_PRE(p->depth_samples >= 0.0f, "a-depth-is-not-negative");
  OMX_PRE(p->lfo_inc >= 0.0f && p->lfo_inc < 1.0f, "an-increment-is-inside-one-turn");
  OMX_PRE(!p->enabled || s->line_l.order == 0 ||
              p->base_samples >= omx_fdelay_min_delay(s->line_l.order),
          "the-shortest-delay-clears-the-kernels-reach");
  OMX_PRE(!p->enabled || s->line_l.order == 0 ||
              p->base_samples + p->depth_samples <=
                  omx_fdelay_max_delay(s->line_l.cap, s->line_l.order),
          "the-longest-delay-is-inside-the-ring");
  const float mix = omx_unit(p->mix);
  if (!p->enabled || n == 0u || mix <= 0.0f) return;
  if (s->line_l.order == 0 || s->line_r.order == 0) return; /* unarmed → announced passthrough */

  const float fb = omx_flanger_clamp_fb(p->feedback);
  const float dry = 1.0f - mix;
  float fl = s->fb_l, fr = s->fb_r;
  s->lfo.inc = p->lfo_inc; /* the speed is the atom's, the phase is the state's */

  for (uint32_t i = 0; i < n; i++) {
    const float xl = l[i], xr = r[i];
    const float d = omx_flanger_delay(p, &s->lfo);
    /* The per-sample door of the primitive, this time with the loop closed around it. */
    const float wl = omx_fdelay_tick(&s->line_l, xl + fb * fl, d);
    const float wr = omx_fdelay_tick(&s->line_r, xr + fb * fr, d);
    /* The loop's own word, flushed: a decaying tail in a feedback path is exactly where a
     * subnormal takes hold, and the line's read flush cannot see what is held out here. */
    omx_wetdry_loop(&l[i], &r[i], &fl, &fr, xl, xr, wl, wr, dry, mix);
    omx_lfo_advance(&s->lfo);
  }
  s->fb_l = fl;
  s->fb_r = fr;
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
  OMX_INVARIANT(s->fb_l - s->fb_l == 0.0f && s->fb_r - s->fb_r == 0.0f, "feedback-state-finite");
  OMX_INVARIANT(s->lfo.phase >= 0.0f && s->lfo.phase < 1.0f, "the-oscillator-stays-in-its-turn");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_FLANGER_H */
