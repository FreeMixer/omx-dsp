// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_delay.h — native stereo FX delay line (send/return or channel insert), pure-C, RT-safe.
 *
 * Same contract as the primitives: NO PipeWire, NO allocation, NO libc beyond <math.h>. The ring buffers
 * live in `struct omx_fx_delay_state` and are allocated by the CALLER off the RT thread (on insert),
 * never here. Controls (`struct omx_fx_delay`) are word-atomic snapshots the RT thread reads once per
 * block. A delay-TIME change may click (the read pointer jumps) — standard for a delay, and documented
 * rather than cross-faded; the wet gain (mix) and feedback are smooth per-sample.
 *
 * Musical shape: input -> [wet = ringRead*mix, dry = in*(1-mix)] out; feedback writes (in + fbTap*fb)
 * back into the ring, with a one-pole low-pass (tone) in the feedback path so repeats darken. Ping-pong
 * cross-feeds the two legs' feedback so echoes bounce L<->R.
 *
 * Read taps (docs/design/specs/2026-09-26-tap-delay-variants.md in OpenMixer): up to
 * OMX_FXDELAY_MAX_TAPS integer reads of the SAME ring, each with its own per-leg gain; the wet is
 * their sum and the feedback regenerates from the longest engaged tap, so a rhythmic pattern
 * repeats as a whole. omx_fx_delay_process is the one-tap delay and plays the same bits it always
 * did; omx_fx_delay_process_taps is the same block with a `struct omx_fx_delay_taps` beside the atom.
 */
#ifndef OMX_MIX_DELAY_H
#define OMX_MIX_DELAY_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <omxdsp/omx_balance_law.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_lookahead.h>
#include <omxdsp/omx_param.h>

/** Max FX delay time (ms). ~2 s covers slow ambient repeats. Read from the generated header
 * (OMX_FX_DELAY_TIME_RANGE_MAX, FX_DELAY_TIME_RANGE.max — F7): this ring's ceiling and the TS travel
 * the row offers are the same declared fact, not two numbers a test has to hold equal. */
#define OMX_FXDELAY_MAX_MS ((int)OMX_FX_DELAY_TIME_RANGE_MAX)
/** Highest graph rate the ring is sized for: the declared rate roof (RT_HARD_TARGET_RATE), generated. */
#define OMX_FXDELAY_MAX_RATE OMX_RT_HARD_TARGET_RATE
/** Per-leg ring capacity, samples: MAX_MS at MAX_RATE, +1 so the full max delay is usable. */
#define OMX_FXDELAY_CAP (((OMX_FXDELAY_MAX_RATE / 1000) * OMX_FXDELAY_MAX_MS) + 1)
/** Read taps on the one ring per leg: tap 0 (the atom's own d_l/d_r) and up to three more. The
 * kernel's array size, the ceiling of a delay's `taps` travel. */
#define OMX_FXDELAY_MAX_TAPS 4

/** Control atom (word-atomic snapshot). `d_l`/`d_r` are per-leg delays in SAMPLES (caller converts
 * ms/tempo -> samples via omx_bpm_division_ms + rate). `feedback`/`mix`/`tone` in [0,1]. */
struct omx_fx_delay {
  int enabled;      /* 0 -> passthrough (a disabled/absent delay) */
  uint32_t d_l;     /* left-leg delay, samples (clamped to cap-1 here) */
  uint32_t d_r;     /* right-leg delay, samples */
  float feedback;   /* 0..<1 regeneration; hard-clamped below 1 to prevent runaway */
  float mix;        /* 0 = dry (bit-identical), 1 = wet-only (FX send/return default) */
  float tone;       /* feedback-path one-pole damping 0..1 (0 = bright, ->1 = dark repeats) */
  int pingpong;     /* non-zero -> cross-feed the legs (echoes bounce L<->R) */
};

/** Caller-owned rings + RT-owned state. `ring_l`/`ring_r` are `cap` floats each (allocated on insert). */
struct omx_fx_delay_state {
  float *ring_l;
  float *ring_r;
  uint32_t cap;
  uint32_t wpos;     /* shared write cursor (mod cap) */
  float damp_l;      /* one-pole tone state per leg (feedback-path LP) */
  float damp_r;
};

/**
 * The read taps beside a delay atom (word-atomic snapshot, read once per block). Tap 0 reads the
 * atom's `d_l`/`d_r`; taps 1 .. ntaps-1 read `d[k]` on both legs. A gain pair is the tap's gain
 * times the balance law of its pan (omx_fxdelay_tap_legs). One tap at gains (1, 1) is the one-tap
 * delay, bit for bit.
 */
struct omx_fx_delay_taps {
  uint32_t ntaps;                       /* engaged taps 1..OMX_FXDELAY_MAX_TAPS; 0 reads as 1 */
  uint32_t d[OMX_FXDELAY_MAX_TAPS];     /* tap k >= 1 delay, samples, both legs; d[0] unused */
  float gl[OMX_FXDELAY_MAX_TAPS];       /* tap k's left-leg gain, 0..1 */
  float gr[OMX_FXDELAY_MAX_TAPS];       /* tap k's right-leg gain, 0..1 */
};

/** Tempo-synced delay time in ms: `division_beats * 60000 / bpm` (1/4=1.0, 1/8=0.5, dotted-1/8=0.75, 1/8T=1/3). */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay/bpm-division"
static inline float omx_bpm_division_ms(float bpm, float division_beats) {
  /* CONTRACT (omx_contract.h). A synced time is a DERIVATION, and the one way it goes wrong is
   * silently: a zero/negative bpm or a negative division returns a time that is not a time, and
   * it is then converted to samples and indexed into a ring. Both halves are stated here so the
   * fault is attributed to the tempo, not to the ring read three calls later. */
  OMX_PRE(division_beats >= 0.0f, "division-is-not-negative");
  float ms = bpm <= 0.0f ? 0.0f : division_beats * 60000.0f / bpm;
  OMX_POST(ms >= 0.0f && ms - ms == 0.0f, "a-synced-time-is-a-finite-non-negative-ms");
  return ms;
}
#undef OMX_CONTRACT_STAGE

/** The rate the `tone` knob is quoted at — the rate this desk runs (R-058), generated from core's
 * DSP_KNOB_REFERENCE_RATE. At that rate the exponent below is exactly 1, no `powf` is taken, and
 * the repeats are bit-for-bit what they always were; any other reference would retune the desk on
 * the day the law landed. */
#define OMX_FXDELAY_TONE_REFERENCE_RATE ((float)OMX_DSP_KNOB_REFERENCE_RATE)

/*
 * THE TONE KNOB'S POLE HOLDS ITS CORNER ACROSS RATES (R-058). `tone` is a UNIT-RANGE knob raised
 * to `p^(REF/sr)` before it reaches the feedback one-pole, so the same knob is the same corner
 * at every declared rate — the same fix R-058 minted for `mix_reverb.h`'s second kernel.
 * Bug: handing `tone` to the pole directly made the same knob a 3 585 Hz filter at 44.1 kHz and
 * a 15 610 Hz one at 192 kHz (docs/design/notes/2026-09-17-delay-math-review.md finding D-2).
 *
 * CALLED ONCE PER BLOCK, never per sample — a `powf` inside the per-sample feedback path would
 * be the most expensive line in this kernel by an order of magnitude, and the per-sample filter
 * stays the plain one-pole it always was.
 */
static inline float omx_fxdelay_tone_pole(float tone, float sr) {
  /* A non-finite tone is no tone at all (pole 0, bright): a NaN carried into the damping state
   * stays there for good (2026-09-25-native-fx-rt-review.md F5). */
  const float p = omx_clamp_or(tone, 0.0f, 1.0f, 0.0f);
  /* pole 0 (bright) and pole 1 (frozen) are their own fixed points; so is the reference rate,
   * and an sr the caller could not supply leaves the knob exactly as it was. */
  if (p <= 0.0f || p >= 1.0f || sr <= 0.0f || sr == OMX_FXDELAY_TONE_REFERENCE_RATE) return p;
  return powf(p, OMX_FXDELAY_TONE_REFERENCE_RATE / sr);
}

/**
 * A per-leg delay TIME (ms) -> the ring tap (samples) at the live rate, saturated to the ring.
 *
 * ONE derivation, read by both shells that own a `struct omx_fx_delay`: the console's
 * `resolve_fx_delay` (mixer_rt.c) and the LV2 plugin's port resolve (omx-plugins' omx_delay_instance.h) — so a
 * millisecond means the same tap on the desk and in a foreign host, and the LV2 shell carries
 * no second copy of the conversion. The clamp happens in FLOAT before the uint32 cast: a
 * huge/NaN/negative ms (a bad tempo-sync division resolved at a low BPM) would make the
 * float->uint32 conversion UB. `> capf` saturates to the ring; `<= 0 / NaN` (both comparisons
 * false) floors to 0. omx_fxdelay_clamp then bounds to cap-1. Fixed work, no alloc — RT-safe.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay/ms-to-samples"
static inline uint32_t omx_fxdelay_ms_to_samples(float ms, float sr) {
  /* CONTRACT (omx_contract.h). Whatever the millisecond was — NaN, negative, a tempo-sync
   * division resolved absurdly — the tap that comes back is a count the ring can hold; it is
   * the statement omx_fxdelay_clamp then enforces against cap-1. */
  OMX_PRE(sr <= 0.0f || OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  float s = ms * 0.001f * sr;
  float capf = (float)OMX_FXDELAY_CAP;
  s = s > 0.0f ? (s > capf ? capf : s) : 0.0f;
  uint32_t d = (uint32_t)(s + 0.5f);
  OMX_POST(d <= (uint32_t)OMX_FXDELAY_CAP, "tap-is-within-the-ring-capacity");
  return d;
}
#undef OMX_CONTRACT_STAGE

/** Clamp a per-leg delay to [0, cap-1]. */
#define OMX_CONTRACT_STAGE "fx-delay/clamp"
static inline uint32_t omx_fxdelay_clamp(uint32_t d, uint32_t cap) {
  /* CONTRACT (omx_contract.h). The enforcement half of omx_fx_delay_process's
   * "taps-inside-the-ring" precondition: whatever the control thread asked for, what comes back
   * is an index the ring holds. Stated because this is the last line before a raw ring[] read. */
  uint32_t c = cap == 0 ? 0u : (d >= cap ? cap - 1 : d);
  OMX_POST(cap == 0u || c < cap, "clamped-tap-is-inside-the-ring");
  return c;
}
#undef OMX_CONTRACT_STAGE

/**
 * A read tap's time (ms): the base time times a factor `num/den`, formed in double and clamped to
 * the delay time travel's top (OMX_FXDELAY_MAX_MS) in MILLISECONDS, so the longest tap does not
 * depend on the clock. omx_fxdelay_ms_to_samples then rounds it ONCE: a tap is never `num/den`
 * times an already-rounded base, so no tap inherits another's error (tap-delay §2).
 *
 * @param base_ms the resolved base time (free or synced), ms
 * @param num the factor's numerator, > 0
 * @param den the factor's denominator, > 0
 * @param clamped out, may be NULL: 1 when the product passed the travel's top and the top is returned
 * @return the tap's time in [0, OMX_FXDELAY_MAX_MS]; a non-finite or negative product is 0
 * @pre num and den are positive
 * @post the time is finite and inside the travel
 * @note Control thread or RT: fixed work, no allocation. Thread-safe: pure.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay/tap-ms"
static inline float omx_fxdelay_tap_ms(float base_ms, uint32_t num, uint32_t den, int *clamped) {
  OMX_PRE(num > 0u && den > 0u, "a-tap-factor-is-a-positive-fraction");
  double raw = den == 0u ? 0.0 : (double)base_ms * (double)num / (double)den;
  double safe = raw - raw == 0.0 && raw > 0.0 ? raw : 0.0;
  int over = safe > (double)OMX_FXDELAY_MAX_MS;
  if (clamped) *clamped = over;
  float ms = over ? (float)OMX_FXDELAY_MAX_MS : (float)safe;
  OMX_POST(ms >= 0.0f && ms <= (float)OMX_FXDELAY_MAX_MS, "a-tap-time-is-inside-the-travel");
  return ms;
}
#undef OMX_CONTRACT_STAGE

/**
 * A read tap's gain pair: `gain` clamped to [0, 1] (non-finite is 0) times the stereo balance law
 * of `pan` (omx_balance_law: the far leg attenuates, nothing boosts, centre is (1, 1)).
 *
 * @param gain the tap's gain, 0..1
 * @param pan the tap's pan, -1..+1 (clamped by the law)
 * @param gl out: the left-leg gain, 0..1
 * @param gr out: the right-leg gain, 0..1
 * @note Control thread or RT: compares and two products. Thread-safe: pure.
 */
static inline void omx_fxdelay_tap_legs(float gain, float pan, float *gl, float *gr) {
  float g = omx_clamp_or(gain, 0.0f, 1.0f, 0.0f);
  float bl, br;
  omx_balance_law(pan - pan == 0.0f ? pan : 0.0f, &bl, &br);
  *gl = g * bl;
  *gr = g * br;
}

/**
 * A tap's per-leg gain as the kernel reads it: finite and inside [0, 1].
 *
 * @param g the control thread's gain for one leg of one tap
 * @return `g` clamped to [0, 1]; a non-finite gain is 0 (the F5 rule)
 * @post the gain is inside unity
 * @note RT-safe: compares only. Thread-safe: pure.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay/tap-gain"
static inline float omx_fxdelay_tap_gain(float g) {
  float c = omx_clamp_or(g, 0.0f, 1.0f, 0.0f);
  OMX_POST(c >= 0.0f && c <= 1.0f, "a-tap-gain-is-inside-unity");
  return c;
}
#undef OMX_CONTRACT_STAGE

/**
 * The index of the LONGEST engaged tap on one leg: the tap the feedback regenerates from.
 *
 * @param d0 tap 0's delay on this leg (samples, clamped)
 * @param d the clamped taps, `d[k]` for `k = 1 .. nt-1`
 * @param nt engaged taps, 1..OMX_FXDELAY_MAX_TAPS
 * @return k of the largest delay; the first on a tie, so one tap is tap 0
 * @pre nt is inside the tap array
 * @post the returned tap is engaged
 * @note RT-safe: at most three compares. Thread-safe: pure.
 */
#define OMX_CONTRACT_STAGE "fx-delay/longest-tap"
static inline uint32_t omx_fxdelay_longest_tap(uint32_t d0, const uint32_t *d, uint32_t nt) {
  OMX_PRE(nt >= 1u && nt <= (uint32_t)OMX_FXDELAY_MAX_TAPS, "tap-count-inside-the-array");
  uint32_t kf = 0, best = d0;
  for (uint32_t k = 1; k < nt; k++) {
    if (d[k] > best) { best = d[k]; kf = k; }
  }
  OMX_POST(kf < nt, "the-feedback-tap-is-engaged");
  return kf;
}
#undef OMX_CONTRACT_STAGE

/**
 * Process one block IN PLACE with read taps. `l`/`r` are the strip's two legs (distinct buffers).
 * A disabled atom / NULL ring is a passthrough. `t` NULL is the one-tap delay (tap 0 at unity on
 * both legs). RT-safe: fixed work per sample (at most OMX_FXDELAY_MAX_TAPS reads), no allocation.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay"
static inline void omx_fx_delay_process_taps(float *l, float *r, uint32_t n,
                                             const struct omx_fx_delay *p,
                                             const struct omx_fx_delay_taps *t,
                                             struct omx_fx_delay_state *s, float sr) {
  /* CONTRACT (omx_contract.h). A delay is a LINEAR, time-invariant transformation with memory:
   * out = dry*in + mix*in[k-d], and the recursion in the ring is bounded by a feedback strictly
   * below 1 — that bound is the whole reason this stage cannot run away, and it is a claim about
   * the CONTROL thread's clamp, checked here because here is where breaking it is audible. The
   * taps are inside the ring (the clamp below is the enforcement; this is its statement), and a
   * mix of 0 must leave the block bit-identical, which the battery asserts around the call. */
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
  OMX_PRE(p->feedback >= 0.0f && p->feedback < 1.0f, "feedback-strictly-below-unity");
  OMX_PRE(p->mix >= 0.0f && p->mix <= 1.0f, "mix-in-unit-range");
  OMX_PRE(p->tone >= 0.0f && p->tone <= 1.0f, "tone-in-unit-range");
  OMX_PRE(s->cap == 0u || (p->d_l < s->cap && p->d_r < s->cap), "taps-inside-the-ring");
  OMX_PRE(t == NULL || t->ntaps <= (uint32_t)OMX_FXDELAY_MAX_TAPS, "tap-count-inside-the-array");
  OMX_PRE(sr <= 0.0f || OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  if (!p->enabled || n == 0 || s->ring_l == NULL || s->ring_r == NULL || s->cap == 0) return;
  uint32_t cap = s->cap;
  uint32_t dl = omx_fxdelay_clamp(p->d_l, cap);
  uint32_t dr = omx_fxdelay_clamp(p->d_r, cap);
  /* A non-finite feedback is no feedback at all — the flanger's rule (omx_flanger_clamp_fb): one
   * NaN written into the ring circulates until the insert is toggled
   * (2026-09-25-native-fx-rt-review.md F5). The travel is the declared FX_DELAY_FEEDBACK_RANGE,
   * whose ceiling stays below 1: repeats never grow unbounded. A non-finite mix is dry. */
  float fb = omx_clamp_or(p->feedback, OMX_FX_DELAY_FEEDBACK_RANGE_MIN, OMX_FX_DELAY_FEEDBACK_RANGE_MAX, 0.0f);
  float mix = omx_clamp_or(p->mix, 0.0f, 1.0f, 0.0f);
  float dry = 1.0f - mix;
  /* R-058: the tone knob is quoted at 96 kHz and raised to REF/sr HERE, once per block, so the
   * repeats darken by the same filter at every clock. */
  float tone = omx_fxdelay_tone_pole(p->tone, sr);
  /* The taps, resolved once per block. No taps, or ntaps 0, is the one-tap delay at unity; tap 0
   * reads d_l/d_r. A tap past the ring reads cap-1 (omx_fxdelay_clamp, the backstop for a tap the
   * caller did not bound to the travel). */
  uint32_t nt = t == NULL || t->ntaps == 0u ? 1u
              : (t->ntaps > (uint32_t)OMX_FXDELAY_MAX_TAPS ? (uint32_t)OMX_FXDELAY_MAX_TAPS : t->ntaps);
  uint32_t td[OMX_FXDELAY_MAX_TAPS];
  float gl[OMX_FXDELAY_MAX_TAPS], gr[OMX_FXDELAY_MAX_TAPS];
  td[0] = 0u;
  gl[0] = t == NULL || t->ntaps == 0u ? 1.0f : omx_fxdelay_tap_gain(t->gl[0]);
  gr[0] = t == NULL || t->ntaps == 0u ? 1.0f : omx_fxdelay_tap_gain(t->gr[0]);
  for (uint32_t k = 1; k < nt; k++) {
    td[k] = omx_fxdelay_clamp(t->d[k], cap);
    gl[k] = omx_fxdelay_tap_gain(t->gl[k]);
    gr[k] = omx_fxdelay_tap_gain(t->gr[k]);
  }
  const uint32_t kfl = omx_fxdelay_longest_tap(dl, td, nt);
  const uint32_t kfr = omx_fxdelay_longest_tap(dr, td, nt);
  uint32_t w = s->wpos;
  float dampL = s->damp_l, dampR = s->damp_r;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t rl = omx_lookahead_back(w, dl, cap);
    uint32_t rr = omx_lookahead_back(w, dr, cap);
    float xl = l[i], xr = r[i];
    /* D == 0 reads the sample ARRIVING this frame, not the w-slot that still holds the value from a
     * full ring ago (which would echo `cap` samples ≈ 2 s of stale audio). Reading the input makes a
     * 0 ms delay an exact passthrough, matching omx_delay_apply's write-then-read at tgt=0. */
    float tapL = dl == 0 ? xl : s->ring_l[rl];
    float tapR = dr == 0 ? xr : s->ring_r[rr];
    /* The wet sum starts AT tap 0's term (never 0 + term, which turns a -0 into +0): one tap at
     * unity is the tap, bit for bit. */
    float wetL = gl[0] * tapL, wetR = gr[0] * tapR;
    float srcL = tapL, srcR = tapR;
    for (uint32_t k = 1; k < nt; k++) {
      uint32_t d = td[k];
      uint32_t rk = omx_lookahead_back(w, d, cap);
      float tl = d == 0 ? xl : s->ring_l[rk];
      float tr = d == 0 ? xr : s->ring_r[rk];
      wetL += gl[k] * tl;
      wetR += gr[k] * tr;
      if (k == kfl) srcL = tl;
      if (k == kfr) srcR = tr;
    }
    /* one-pole low-pass in the feedback path (tone, Freeverb-style damping): tone=0 passes the tap
     * through (bright), tone->1 freezes it toward its running average (dark repeats). The feedback
     * regenerates from the LONGEST engaged tap, so a pattern repeats as a whole. */
    dampL = srcL * (1.0f - tone) + dampL * tone;
    dampR = srcR * (1.0f - tone) + dampR * tone;
    /* ping-pong: each leg's feedback comes from the OTHER leg's damped tap */
    float fbL = p->pingpong ? dampR : dampL;
    float fbR = p->pingpong ? dampL : dampR;
    s->ring_l[w] = xl + fb * fbL;
    s->ring_r[w] = xr + fb * fbR;
    l[i] = dry * xl + mix * wetL;
    r[i] = dry * xr + mix * wetR;
    w = omx_lookahead_fwd(w, 1u, cap);
  }
  s->wpos = w;
  s->damp_l = dampL;
  s->damp_r = dampR;
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
  OMX_POST(s->wpos < s->cap, "write-cursor-inside-the-ring");
  OMX_INVARIANT(s->damp_l - s->damp_l == 0.0f && s->damp_r - s->damp_r == 0.0f,
                "damping-state-finite");
}
#undef OMX_CONTRACT_STAGE

/**
 * Process one block IN PLACE: the one-tap delay. `l`/`r` are the strip's two legs (distinct
 * buffers). A disabled atom / NULL ring is a passthrough. RT-safe: fixed work per sample, no
 * allocation.
 */
static inline void omx_fx_delay_process(float *l, float *r, uint32_t n,
                                        const struct omx_fx_delay *p, struct omx_fx_delay_state *s,
                                        float sr) {
  omx_fx_delay_process_taps(l, r, n, p, NULL, s, sr);
}

#endif /* OMX_MIX_DELAY_H */
