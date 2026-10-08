// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_chorus.h — the native CHORUS stage: N voices reading ONE modulated fractional delay line.
 *
 * Spec: docs/design/specs/2026-09-22-chorus-and-flanger.md (openmixer#859). The primitives are
 * `mix_fdelay.h` at order 3 (the fdelay ruling's own row for this consumer) and `mix_lfo.h`;
 * nothing here is a kernel of its own.
 *
 * Same contract as omx_delay.h: NO PipeWire, NO napi, NO allocation. The rings live in
 * `struct omx_chorus_state` and are allocated by the CALLER off the RT thread (on insert), never
 * here. Controls are a word-atomic snapshot the RT thread resolves once per block.
 *
 * ## What it IS
 *
 *     d_k(t) = base + depth·(1 + s(φ + k/N))/2            (omx_lfo_sweep, unipolar, upward)
 *     y      = (1 − mix)·x + (mix/N)·Σ_k line(d_k(t))
 *
 * At N = 1 and mix = 0.5 that is a comb whose notches sit at f = (2m+1)/(2d) and whose peaks sit
 * at f = m/d, both MOVING with d(t) — the swept comb the spec's §2 measures against its closed
 * form at four rates.
 *
 * ## The five things this file is careful about
 *
 *  1. ONE WRITE, N READS. `omx_fdelay_tick` is write-then-read; a chorus's voices differ only in
 *     WHERE they read, so the write happens once and the reads N times. That is the tick
 *     decomposed, not a second path through the ring — `mix_fdelay.h`'s own two doors.
 *  2. ONE OSCILLATOR, N OFFSETS. Voice k reads the LFO at k/N turns. N accumulators meant to stay
 *     a fixed distance apart are N facts where there is one, and they drift (mix_lfo.h's header).
 *  3. NO GAIN ELEMENT. The voices sum at 1/N and the wet/dry mix is convex; the one thing that
 *     is NOT convex is the Lagrange-3 read itself, whose taps overshoot by its ℓ1 norm Λ
 *     (`OMX_FDELAY_READ_L1_NORM`, 1.25 at the half sample). So |y| ≤ ((1−mix) + mix·Λ)·max|x|
 *     ≤ Λ·max|x| — the `no-gain-added` law as spec §6 states it, held by the shape of the
 *     arithmetic and not by a clamp, and published by `omx_chorus_gain_bound`.
 *  4. BOTH LEGS, ONE PHASE — unless the row asks. At `spread` 0 both legs read the same instant
 *     of the oscillator, so a mono source stays mono: width is the BUS's fact
 *     (2026-08-07-one-summing-bus.md). The one declared exception is the Dimension (spec §2):
 *     the right leg's voice k reads the SAME oscillator at k/N + spread through `omx_lfo_at`'s
 *     offset — no second oscillator, no pan, and at spread 0 the right leg reuses the left leg's
 *     delay word, so the stage is the one-phase stage bit for bit.
 *  5. DISABLED IS FREE. `enabled == 0` returns before a sample or a state word is touched, and
 *     mix == 0 returns the block bit-identically — including words below the flush floor, because
 *     the dry path is a multiply by exactly 1 and a sum with exactly 0, which this file takes as
 *     an early return rather than as arithmetic.
 */
#ifndef OMX_MIX_CHORUS_H
#define OMX_MIX_CHORUS_H

#include <stdint.h>

#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/omx_fdelay.h>
#include <omxdsp/omx_lfo.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_contract.h>

/** The Lagrange order this stage constructs — the fdelay ruling's §3 row for chorus/flanger. */
#define OMX_CHORUS_ORDER OMX_FDELAY_MOD_READ_ORDER

/** The most voices one strip may run. Four is the M32 chorus's widest ensemble and the point
 *  where more taps stop being heard as more voices; the array is fixed because the RT path
 *  allocates nothing. */
#define OMX_CHORUS_MAX_VOICES OMX_CHORUS_VOICES_RANGE_MAX

/* The base delay (OMX_CHORUS_BASE_MS) is declared in omx-contract: the shortest delay any voice reads (spec §3a). */
/** The deepest sweep the row may ask for, ms (spec §3a's `depth` ceiling). */
#define OMX_CHORUS_MAX_DEPTH_MS ((float)OMX_CHORUS_DEPTH_RANGE_MAX)
/** The widest right-leg offset, turns: half a turn is the Dimension's opposed sweep (spec §3a),
 *  and past it the offset only repeats the other side of the circle. The row's travel ceiling,
 *  generated from CHORUS_SPREAD_RANGE (dsp-primitives §7, the scalar door). */
#define OMX_CHORUS_SPREAD_MAX OMX_CHORUS_SPREAD_RANGE_MAX
/** Highest graph rate the ring is sized for, so base+depth is reachable at every declared rate. */
#define OMX_CHORUS_MAX_RATE OMX_RT_HARD_TARGET_RATE
/* The longest delay, whole ms (OMX_CHORUS_MAX_MS: base + depth ceiling rounded up), is declared in omx-contract, an integer constant expression for the ring size below. */
/** Per-leg ring capacity, samples: the longest delay at the highest rate, plus the kernel's reach
 *  (`omx_fdelay_cap_for`'s +2 and lookbehind, taken at the maximum order). */
#define OMX_CHORUS_CAP ((uint32_t)(((OMX_CHORUS_MAX_RATE / 1000) * OMX_CHORUS_MAX_MS) + 4))

/**
 * The resolved control atom — built once per block by the caller from the word-atomic controls,
 * with every time already converted to SAMPLES at the live rate (the same division of labour
 * `omx_delay.h` uses: the kernel never sees a millisecond).
 */
struct omx_chorus {
  int enabled;         /* 0 → passthrough, before a sample or a state word is touched */
  int voices;          /* 1..OMX_CHORUS_MAX_VOICES */
  float base_samples;  /* the shortest delay a voice reads */
  float depth_samples; /* peak-to-peak excursion above the base */
  /* The oscillator's speed in TURNS PER SAMPLE, derived by the caller from a rate in Hz at the
   * live graph rate (`omx_lfo_inc`). The kernel never sees a hertz, for the same reason it never
   * sees a millisecond: the conversion belongs where the rate is known, once per block. */
  float lfo_inc;
  float mix;           /* 0 = dry (bit-identical), 1 = wet only */
  /* The right leg's read offset, TURNS of the one oscillator, 0..OMX_CHORUS_SPREAD_MAX: voice k
   * of the right leg reads the LFO at k/N + spread. 0 = both legs one phase, bit-identical to the
   * stage without the field (spec §2). Last, so a positional initialiser leaves it 0. */
  float spread;
};

/** Caller-owned rings + RT-owned state. `ring_l`/`ring_r` are OMX_CHORUS_CAP floats each,
 *  allocated on insert; the two lines are armed over them once, by `omx_chorus_state_init`. */
struct omx_chorus_state {
  struct omx_fdelay line_l;
  struct omx_fdelay line_r;
  struct omx_lfo lfo;
};

/**
 * Arm both legs over caller-owned rings and start the oscillator at the turn's zero. Returns the
 * line's own code, so a ring the caller sized wrongly is a FACT it is told rather than a stage
 * that quietly does nothing.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "chorus/init"
static inline enum omx_fdelay_code omx_chorus_state_init(struct omx_chorus_state *s, float *ring_l,
                                                         float *ring_r, uint32_t cap) {
  omx_lfo_start(&s->lfo);
  enum omx_fdelay_code c = omx_fdelay_init(&s->line_l, ring_l, cap, OMX_CHORUS_ORDER);
  if (c != OMX_FDELAY_OK) return c;
  c = omx_fdelay_init(&s->line_r, ring_r, cap, OMX_CHORUS_ORDER);
  OMX_POST(c != OMX_FDELAY_OK || s->line_l.order == OMX_CHORUS_ORDER,
           "an-armed-chorus-reads-at-order-three");
  return c;
}
#undef OMX_CONTRACT_STAGE

/**
 * The delay voice `k` of `n` reads, at the oscillator's current phase — the one place the sweep
 * is derived, so the oracle and the kernel cannot be reading two different curves.
 */
#define OMX_CONTRACT_STAGE "chorus/voice-delay"
/** The sweep at a voice's place in the turn, `off` = k/N — the kernel hoists the N offsets out of
 *  its sample loop and reads through here, the oracle reads through `omx_chorus_voice_delay`. */
static inline float omx_chorus_delay_at(const struct omx_chorus *p, const struct omx_lfo *lfo,
                                        float off) {
  /* CONTRACT: a voice's place is a point in ONE turn, and the delay it yields is a point of the
   * stage's own sweep, [shortest, longest] — the reach the process PREs checked against the ring. */
  OMX_PRE(off >= 0.0f && off < 1.0f, "an-offset-is-inside-one-turn");
  OMX_PRE(p->depth_samples >= 0.0f, "a-depth-is-not-negative");
  const float d = omx_lfo_sweep(p->base_samples, p->depth_samples, omx_lfo_at(lfo, off));
  OMX_POST(d >= p->base_samples - 1e-6f && d <= p->base_samples + p->depth_samples + 1e-6f,
           "a-voice-delay-is-inside-the-sweep");
  return d;
}
static inline float omx_chorus_voice_delay(const struct omx_chorus *p, const struct omx_lfo *lfo,
                                           int k, int n) {
  OMX_PRE(n >= 1 && n <= OMX_CHORUS_MAX_VOICES, "a-voice-count-is-inside-the-ensemble");
  OMX_PRE(k >= 0 && k < n, "a-voice-is-one-of-the-ensemble");
  return omx_chorus_delay_at(p, lfo, (float)k / (float)n);
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "chorus/voice-delay-right"
/**
 * @brief The delay the RIGHT leg's voice `k` of `n` reads: the left leg's sweep read `spread`
 *        turns further round the one oscillator (spec §2, `d_k^R`).
 * @param p The resolved atom; `p->spread` in [0, OMX_CHORUS_SPREAD_MAX].
 * @param lfo The stage's one oscillator, read and never moved.
 * @param k The voice, in [0, n).
 * @param n The voice count, in [1, OMX_CHORUS_MAX_VOICES].
 * @return A delay in samples inside [base, base + depth].
 * @pre `a-voice-count-is-inside-the-ensemble`, `a-voice-is-one-of-the-ensemble`,
 *      `a-spread-is-inside-half-a-turn`.
 * @note RT-safe and thread-safe: one wrap, one shape, one sweep.
 */
static inline float omx_chorus_voice_delay_right(const struct omx_chorus *p,
                                                 const struct omx_lfo *lfo, int k, int n) {
  OMX_PRE(n >= 1 && n <= OMX_CHORUS_MAX_VOICES, "a-voice-count-is-inside-the-ensemble");
  OMX_PRE(k >= 0 && k < n, "a-voice-is-one-of-the-ensemble");
  OMX_PRE(p->spread >= 0.0f && p->spread <= OMX_CHORUS_SPREAD_MAX, "a-spread-is-inside-half-a-turn");
  return omx_chorus_delay_at(p, lfo, omx_lfo_wrap((float)k / (float)n + p->spread));
}
#undef OMX_CONTRACT_STAGE

/** The stage's own effect time, in samples: the ends of the sweep. A READBACK for the row, never
 *  a latency term — the dry path is direct and nothing downstream waits (spec §5). */
/**
 * The most this stage can exceed its input by, as a LINEAR factor: (1 − mix) + mix·Λ, Λ the read
 * kernel's ℓ1 norm read from the generated header — never above Λ. The `no-gain-added` law's
 * bound (spec §6): the 1/N voice sum is convex, each voice read is at most Λ·max|x|.
 */
static inline float omx_chorus_gain_bound(const struct omx_chorus *p) {
  const float mix = omx_unit(p->mix);
  return (1.0f - mix) + mix * OMX_FDELAY_READ_L1_NORM;
}

static inline float omx_chorus_shortest_delay(const struct omx_chorus *p) { return p->base_samples; }
static inline float omx_chorus_longest_delay(const struct omx_chorus *p) {
  return p->base_samples + p->depth_samples;
}

/**
 * Process one block IN PLACE. `l`/`r` are the strip's two legs (distinct buffers); a disabled
 * atom, a NULL ring or an unarmed line is a passthrough. The LFO advances ONCE per sample for the
 * whole stage — both legs and every voice read the same instant of the same oscillator.
 *
 * RT-safe: fixed work per sample (N kernels, each read on both legs: 2N four-tap reads), no
 * allocation, no libm call in the loop. The voice offsets are computed once per block.
 */
#define OMX_CONTRACT_STAGE "chorus"
static inline void omx_chorus_process(float *l, float *r, uint32_t n, const struct omx_chorus *p,
                                      struct omx_chorus_state *s) {
  /* CONTRACT (omx_contract.h). A chorus is a CONVEX combination of the dry signal and the mean of
   * N fractional reads of its own past: there is no gain element in it, and the only overshoot is
   * the read kernel's own, so |y| ≤ omx_chorus_gain_bound·max|x| ≤ Λ·max|x| whatever the controls
   * say — the `no-gain-added` law as spec §6 states it. The delays are
   * inside the line's own reach (the resolve's clamp is the enforcement half, stated here because
   * here is where a bad one would be read), and mix == 0 leaves the block bit-identical. */
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
  OMX_PRE(p->mix >= 0.0f && p->mix <= 1.0f, "mix-in-unit-range");
  OMX_PRE(p->voices >= 1 && p->voices <= OMX_CHORUS_MAX_VOICES,
          "a-voice-count-is-inside-the-ensemble");
  /* The chorus-only controls together, so the reach/mix PREs around them read as the flanger's. */
  OMX_PRE(p->spread >= 0.0f && p->spread <= OMX_CHORUS_SPREAD_MAX, "a-spread-is-inside-half-a-turn");
  OMX_PRE(p->depth_samples >= 0.0f, "a-depth-is-not-negative");
  OMX_PRE(p->lfo_inc >= 0.0f && p->lfo_inc < 1.0f, "an-increment-is-inside-one-turn");
  OMX_PRE(!p->enabled || s->line_l.order == 0 ||
              p->base_samples >= omx_fdelay_min_delay(s->line_l.order),
          "the-shortest-voice-clears-the-kernels-reach");
  OMX_PRE(!p->enabled || s->line_l.order == 0 ||
              p->base_samples + p->depth_samples <=
                  omx_fdelay_max_delay(s->line_l.cap, s->line_l.order),
          "the-longest-voice-is-inside-the-ring");
  const float mix = omx_unit(p->mix);
  if (!p->enabled || n == 0u || mix <= 0.0f) return;
  if (s->line_l.order == 0 || s->line_r.order == 0) return; /* unarmed → announced passthrough */

  const int voices = p->voices < 1 ? 1
                     : p->voices > OMX_CHORUS_MAX_VOICES ? OMX_CHORUS_MAX_VOICES
                                                         : p->voices;
  const float dry = 1.0f - mix;
  const float per_voice = mix / (float)voices;
  /* A spread of 0 reuses the left leg's delay word, so the default is the one-phase stage. */
  const int split = p->spread > 0.0f;
  /* The speed is the ATOM's and the phase is the STATE's — so a rate change lands on the next
   * block without moving the sweep, and a block boundary is not a discontinuity. */
  s->lfo.inc = p->lfo_inc;
  /* PER BLOCK, not per sample: each voice's place in the turn, k/N — the same float division the
   * per-sample form made, so the delays it yields are the same bits. */
  float off[OMX_CHORUS_MAX_VOICES];
  for (int k = 0; k < voices; k++) off[k] = (float)k / (float)voices;
  /* The right leg's places, `spread` turns further round the SAME oscillator (spec §2, d_k^R). */
  float off_r[OMX_CHORUS_MAX_VOICES];
  for (int k = 0; k < voices; k++) off_r[k] = split ? omx_lfo_wrap(off[k] + p->spread) : off[k];
  /* Both legs are armed over rings of one size at one order and written in lockstep, so a delay
   * splits identically on either: ONE split and ONE kernel per voice serve both reads. */
  OMX_PRE(s->line_l.cap == s->line_r.cap && s->line_l.order == s->line_r.order &&
              s->line_l.wpos == s->line_r.wpos,
          "both-legs-share-one-geometry");

  for (uint32_t i = 0; i < n; i++) {
    const float xl = l[i], xr = r[i];
    /* ONE WRITE per leg per sample — the tick decomposed, because the voices share it. */
    omx_fdelay_write(&s->line_l, xl);
    omx_fdelay_write(&s->line_r, xr);
    float wl = 0.0f, wr = 0.0f;
    for (int k = 0; k < voices; k++) {
      /* `omx_fdelay_read` decomposed into its own words — split, kernel, read_at — so the kernel
       * is computed once per voice and read on both legs, bit-identical to two reads. A spread
       * moves the right leg's delay off the left's, and then each leg splits its own. */
      const float d = omx_chorus_delay_at(p, &s->lfo, off[k]);
      if (split) {
        wl += omx_fdelay_read(&s->line_l, d);
        wr += omx_fdelay_read(&s->line_r, omx_chorus_delay_at(p, &s->lfo, off_r[k]));
        continue;
      }
      uint32_t id = 0u;
      float kf = 0.0f;
      if (omx_fdelay_split(&s->line_l, d, &id, &kf)) {
        wl += omx_fdelay_read_at(&s->line_l, id, 0);
        wr += omx_fdelay_read_at(&s->line_r, id, 0);
      } else {
        float c[OMX_FDELAY_MAX_TAPS];
        omx_fdelay_lagrange(s->line_l.order, kf, c);
        wl += omx_fdelay_read_at(&s->line_l, id, c);
        wr += omx_fdelay_read_at(&s->line_r, id, c);
      }
    }
    l[i] = dry * xl + per_voice * wl;
    r[i] = dry * xr + per_voice * wr;
    omx_lfo_advance(&s->lfo);
  }
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
  OMX_INVARIANT(s->lfo.phase >= 0.0f && s->lfo.phase < 1.0f, "the-oscillator-stays-in-its-turn");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_CHORUS_H */
