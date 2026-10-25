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
 */
#ifndef OMX_MIX_DELAY_H
#define OMX_MIX_DELAY_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <omxdsp/omx_balance_law.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_duck.h>
#include <omxdsp/omx_lfo.h>
#include <omxdsp/omx_lookahead.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_wetdry.h>

/** Max FX delay time (ms). ~2 s covers slow ambient repeats. Read from the generated header
 * (OMX_FX_DELAY_TIME_RANGE_MAX, FX_DELAY_TIME_RANGE.max — F7): this ring's ceiling and the TS travel
 * the row offers are the same declared fact, not two numbers a test has to hold equal. */
#define OMX_FXDELAY_MAX_MS ((int)OMX_FX_DELAY_TIME_RANGE_MAX)
/** Highest graph rate the ring is sized for: the declared rate roof (RT_HARD_TARGET_RATE), generated. */
#define OMX_FXDELAY_MAX_RATE OMX_RT_HARD_TARGET_RATE
/** Per-leg ring capacity, samples: MAX_MS at MAX_RATE, +1 so the full max delay is usable. */
#define OMX_FXDELAY_CAP (((OMX_FXDELAY_MAX_RATE / 1000) * OMX_FXDELAY_MAX_MS) + 1)
/** Read-tap ceiling of the one ring per leg: the tap-delay variants' `taps` travel 1..4, read from the
 * generated header (OMX_FX_DELAY_TAPS_RANGE_MAX, FX_DELAY_TAPS_RANGE.max, the integer reader that sizes
 * arrays), as OMX_FXDELAY_MAX_MS reads the time travel. */
#define OMX_FXDELAY_MAX_TAPS OMX_FX_DELAY_TAPS_RANGE_MAX

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

/** The read taps, a second snapshot beside the atom (word-atomic, read once per block by
 * omx_fx_delay_process_taps). The atom stays the one-tap delay's and every caller of
 * omx_fx_delay_process builds and plays as it did; a caller that plays taps resolves this once per
 * parameter change: each tap's time by omx_fxdelay_tap_ms then omx_fxdelay_ms_to_samples, each
 * tap's gain pair by omx_fxdelay_tap_legs. Tap 0 is the atom's own d_l/d_r. */
struct omx_fx_delay_taps {
  uint32_t ntaps;                   /* engaged read taps 1..OMX_FXDELAY_MAX_TAPS; 0 is the one-tap delay at unity, centre */
  uint32_t d[OMX_FXDELAY_MAX_TAPS]; /* tap k >= 1: delay, samples, both legs; d[0] unused (tap 0 is d_l/d_r) */
  float gl[OMX_FXDELAY_MAX_TAPS];   /* tap k's left-leg gain, gain_k x balance-law bL(pan_k), 0..1 */
  float gr[OMX_FXDELAY_MAX_TAPS];   /* tap k's right-leg gain, gain_k x balance-law bR(pan_k), 0..1 */
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

/* The repeats' colour and the wet duck, a third snapshot beside the atom and the taps so every caller
 * of the atom builds and plays as it did. A NULL colour, or digital with a 0 dB duck, is the delay
 * that shipped, bit for bit. */
enum omx_fx_delay_mode {
  OMX_FX_DELAY_DIGITAL = 0, /* the delay as it shipped */
  OMX_FX_DELAY_TAPE = 1,    /* a loop low pass and a wow on the read head */
  OMX_FX_DELAY_ANALOG = 2,  /* a lower loop low pass and a soft saturation in the feedback */
};

struct omx_fx_delay_color {
  int mode;          /* enum omx_fx_delay_mode */
  float loop_pole;   /* the loop low pass's one-pole coefficient (tape, analog); 0 passes */
  float wow_inc;     /* the wow's LFO turns per sample (tape) */
  float wow_depth;   /* the wow's depth, samples: the head reads between the time and the time plus this */
  struct omx_duck duck; /* the wet duck; a 0 dB depth ducks nothing */
};

/* The colour's own state: the loop low pass per leg, the wow and the duck's follower. */
struct omx_fx_delay_color_state {
  float lp_l;
  float lp_r;
  struct omx_lfo wow;
  struct omx_duck_state duck;
};

/** The soft saturation of the analog mode's feedback, `x / (1 + |x|)`: unity slope at 0, never past 1. */
static inline float omx_fxdelay_soft(float x) { return x / (1.0f + fabsf(x)); }

/** A read `d + a` samples behind the write cursor, `a` in [0, 1): linear between the two whole reads,
 * the further one held inside the ring. */
static inline float omx_fxdelay_read_frac(const float *ring, uint32_t w, uint32_t d, float a, uint32_t cap) {
  const uint32_t d1 = d + 1u < cap ? d + 1u : cap - 1u;
  const float y0 = ring[omx_lookahead_back(w, d, cap)], y1 = ring[omx_lookahead_back(w, d1, cap)];
  return y0 + a * (y1 - y0);
}

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
 * a 15 610 Hz one at 192 kHz (the delay math review notes finding D-2).
 *
 * CALLED ONCE PER BLOCK, never per sample — a `powf` inside the per-sample feedback path would
 * be the most expensive line in this kernel by an order of magnitude, and the per-sample filter
 * stays the plain one-pole it always was.
 */
static inline float omx_fxdelay_tone_pole(float tone, float sr) {
  /* A non-finite tone is no tone at all (pole 0, bright): a NaN carried into the damping state
   * stays there for good. */
  const float p = omx_clamp_or(tone, 0.0f, 1.0f, 0.0f);
  /* the tier's one reference-rate law: pole 0 (bright), pole 1 (frozen), the reference rate and an
   * sr the caller could not supply leave the knob exactly as it was. */
  return omx_pole_rescale(p, OMX_FXDELAY_TONE_REFERENCE_RATE, sr);
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
 * A read tap's time (ms) from the base time and its factor `num/den`, bounded IN MILLISECONDS to the
 * declared time travel's top (OMX_FXDELAY_MAX_MS, FX_DELAY_TIME_RANGE.max). The control thread's
 * resolve, one call per tap, before omx_fxdelay_ms_to_samples rounds the tap to a whole sample.
 *
 * The product `base_ms x num / den` is formed in double from the exact fraction, so each tap is
 * rounded ONCE from its own time and never inherits the rounding of another tap or of the base. The
 * ceiling is a millisecond one, not a sample one: the ring (OMX_FXDELAY_CAP) is sized for the top of
 * the travel at the highest rate, so it holds more than that at a lower rate, and a sample ceiling
 * would make the longest tap depend on the clock. omx_fxdelay_clamp stays the kernel's backstop for a
 * tap its caller did not bound.
 *
 * @param base_ms the base time T, ms (the free time, or omx_bpm_division_ms when synced); a
 *        non-finite or negative base is 0
 * @param num the factor's numerator
 * @param den the factor's denominator; 0 is refused (PRE factor-denominator-is-not-zero) and the
 *        tap is 0 ms
 * @param clamped out, may be NULL: 1 when the tap was asked past the travel's top and was clamped
 *        to it, else 0
 * @return the tap's time, ms, in [0, OMX_FXDELAY_MAX_MS]
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay/tap-ms"
static inline float omx_fxdelay_tap_ms(float base_ms, uint32_t num, uint32_t den, int *clamped) {
  /* num/den is a member of the declared factor set, DELAY_TAP_FACTOR_NUMS[i] / DELAY_TAP_FACTOR_DENS[i],
   * or any fraction a caller forms; the kernel takes the fraction, never a float factor. */
  OMX_PRE(den != 0u, "factor-denominator-is-not-zero");
  const double top = (double)OMX_FXDELAY_MAX_MS;
  /* A NaN, an infinity, a negative or a zero base is no time at all (x - x is 0 only when x is
   * finite, and `> 0` is false for a NaN). */
  const int timed = den != 0u && base_ms - base_ms == 0.0f && base_ms > 0.0f;
  const double ms = timed ? (double)base_ms * (double)num / (double)den : 0.0;
  const int over = ms > top;
  if (clamped != NULL) *clamped = over;
  const float out = over ? (float)top : (float)ms;
  OMX_POST(out >= 0.0f && out <= (float)top, "a-tap-is-inside-the-time-travel");
  return out;
}
#undef OMX_CONTRACT_STAGE

/**
 * A tap's per-leg gain from its gain and pan: `gain x omx_balance_law(pan)`, the stereo-strip law that
 * attenuates the far leg and never boosts. The control thread's resolve, one call per tap.
 *
 * @param gain the tap's gain, 0..1; a non-finite gain is 0
 * @param pan the tap's pan, -1..+1 (omx_clamp_pan's travel); a non-finite pan is centre
 * @param gl out: the left leg's gain, in [0, 1]
 * @param gr out: the right leg's gain, in [0, 1]
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay/tap-legs"
static inline void omx_fxdelay_tap_legs(float gain, float pan, float *gl, float *gr) {
  /* The gain and pan travels are DELAY_TAP_LIMITS' rows: gain 0..1, pan -1..+1. */
  const float g = omx_clamp_or(gain, 0.0f, 1.0f, 0.0f);
  /* A non-finite pan is centre: the balance law would carry a NaN into both legs' gains. */
  float bl, br;
  omx_balance_law(omx_clamp_or(pan, (float)OMX_PAN_RANGE_MIN, (float)OMX_PAN_RANGE_MAX, 0.0f), &bl, &br);
  *gl = g * bl;
  *gr = g * br;
  OMX_POST(*gl >= 0.0f && *gl <= 1.0f && *gr >= 0.0f && *gr <= 1.0f, "a-tap-gain-is-inside-unity");
}
#undef OMX_CONTRACT_STAGE

/**
 * A tap's per-leg gain as the kernel reads it: finite and inside [0, 1].
 *
 * @param g the control thread's `gain_k x b_k,leg`
 * @return `g` clamped to [0, 1]; a non-finite gain is 0
 */
#define OMX_CONTRACT_STAGE "fx-delay/tap-gain"
static inline float omx_fxdelay_tap_gain(float g) {
  const float c = omx_clamp_or(g, 0.0f, 1.0f, 0.0f);
  OMX_POST(c >= 0.0f && c <= 1.0f, "a-tap-gain-is-inside-unity");
  return c;
}
#undef OMX_CONTRACT_STAGE

/**
 * The index of the LONGEST engaged tap on one leg, the one the feedback regenerates from, so a
 * pattern repeats as a whole.
 *
 * @param d0 tap 0's delay on this leg (samples, clamped)
 * @param d the clamped taps, `d[k]` for `k = 1 .. nt-1`
 * @param nt engaged taps, 1..OMX_FXDELAY_MAX_TAPS
 * @return k of the largest delay; the first on a tie, so one tap is tap 0
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
 * Process one block IN PLACE with read taps and a colour. `l`/`r` are the strip's two legs (distinct buffers). A
 * disabled atom / NULL ring is a passthrough. RT-safe: fixed work per sample, no allocation.
 *
 * `t->ntaps` 1..OMX_FXDELAY_MAX_TAPS integer reads of the SAME ring, tap 0 at the atom's d_l/d_r and
 * tap k >= 1 at `t->d[k]` on both legs, each with its own per-leg gain; the wet sum is
 * `sum_k g_k,leg x tap_k` and the feedback regenerates from the longest engaged tap. A NULL `t`, or
 * `ntaps` 0, is the one-tap delay at unity and centre, bit for bit omx_fx_delay_process. A tap asked
 * past the ring is refused (PRE taps-inside-the-ring) and read at cap-1.
 *
 * The colour `c` (with its state `cs`): tape reads every tap `0..wow_depth` samples further back on a
 * slow LFO, linearly between whole reads, and runs a one-pole low pass in the loop after the tone;
 * analog runs its low pass and then `x / (1 + |x|)` in the feedback; a duck depth below 0 dB scales the
 * wet sum by omx_duck_step() on the louder dry leg. A NULL `c`, or digital with a 0 dB duck, is the
 * delay with taps as it shipped, bit for bit.
 *
 * @param l the left leg, `n` frames, processed in place
 * @param r the right leg, `n` frames, processed in place
 * @param n frames in the block
 * @param p the delay's control atom
 * @param t the read taps, or NULL for the one-tap delay
 * @param c the colour and the wet duck, or NULL for none
 * @param cs the colour's state, or NULL with `c`
 * @param s the caller-owned rings and the RT state
 * @param sr the live rate, Hz
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay"
static inline void omx_fx_delay_process_color(float *l, float *r, uint32_t n, const struct omx_fx_delay *p,
                                              const struct omx_fx_delay_taps *t, const struct omx_fx_delay_color *c,
                                              struct omx_fx_delay_color_state *cs, struct omx_fx_delay_state *s,
                                              float sr) {
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
  /* No taps, or ntaps 0, is the one-tap delay: tap 0 alone, at unity and centre. */
  const uint32_t ntaps = t == NULL ? 0u : t->ntaps;
  OMX_PRE(ntaps <= (uint32_t)OMX_FXDELAY_MAX_TAPS, "tap-count-inside-the-array");
#ifdef OMX_CONTRACTS
  for (uint32_t k = 1; s->cap != 0u && k < ntaps && k < (uint32_t)OMX_FXDELAY_MAX_TAPS; k++)
    OMX_PRE(t->d[k] < s->cap, "taps-inside-the-ring");
#endif
  OMX_PRE(sr <= 0.0f || OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  if (!p->enabled || n == 0 || s->ring_l == NULL || s->ring_r == NULL || s->cap == 0) return;
  uint32_t cap = s->cap;
  uint32_t dl = omx_fxdelay_clamp(p->d_l, cap);
  uint32_t dr = omx_fxdelay_clamp(p->d_r, cap);
  /* A non-finite feedback is no feedback at all — the flanger's rule (omx_flanger_clamp_fb): one
   * NaN written into the ring circulates until the insert is toggled.
   * The travel is the declared FX_DELAY_FEEDBACK_RANGE,
   * whose ceiling stays below 1: repeats never grow unbounded. A non-finite mix is dry. */
  float fb = omx_clamp_or(p->feedback, OMX_FX_DELAY_FEEDBACK_RANGE_MIN, OMX_FX_DELAY_FEEDBACK_RANGE_MAX, 0.0f);
  float mix = omx_clamp_or(p->mix, 0.0f, 1.0f, 0.0f);
  float dry = 1.0f - mix;
  /* R-058: the tone knob is quoted at 96 kHz and raised to REF/sr HERE, once per block, so the
   * repeats darken by the same filter at every clock. */
  float tone = omx_fxdelay_tone_pole(p->tone, sr);
  /* The taps, resolved once per block. ntaps 0 is the one-tap delay at unity and centre, so a
   * caller that plays no taps hears the delay as it always was; tap 0 is d_l/d_r. */
  const uint32_t nt = ntaps == 0u ? 1u
                    : (ntaps > (uint32_t)OMX_FXDELAY_MAX_TAPS ? (uint32_t)OMX_FXDELAY_MAX_TAPS : ntaps);
  uint32_t td[OMX_FXDELAY_MAX_TAPS];
  float gl[OMX_FXDELAY_MAX_TAPS], gr[OMX_FXDELAY_MAX_TAPS];
  gl[0] = ntaps == 0u ? 1.0f : omx_fxdelay_tap_gain(t->gl[0]);
  gr[0] = ntaps == 0u ? 1.0f : omx_fxdelay_tap_gain(t->gr[0]);
  td[0] = 0u;
  for (uint32_t k = 1; k < nt; k++) {
    td[k] = omx_fxdelay_clamp(t->d[k], cap);
    gl[k] = omx_fxdelay_tap_gain(t->gl[k]);
    gr[k] = omx_fxdelay_tap_gain(t->gr[k]);
  }
  /* The colour, hoisted: each flag 0 leaves its arithmetic out, so digital is the delay as it shipped. */
  const int col = c != NULL && cs != NULL;
  const int tape = col && c->mode == OMX_FX_DELAY_TAPE;
  const int analog = col && c->mode == OMX_FX_DELAY_ANALOG;
  const int wow = tape && c->wow_depth > 0.0f;
  const int ducked = col && c->duck.depth_db < 0.0f;
  const float lp = (tape || analog) ? c->loop_pole : 0.0f;
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
    /* Tape's wow: the head reads `woff` samples further back, 0 to the depth, linearly between reads. */
    uint32_t wi = 0u;
    float wa = 0.0f;
    if (wow) {
      const float woff = c->wow_depth * 0.5f * (1.0f + omx_lfo_at(&cs->wow, 0.0f));
      wi = (uint32_t)woff;
      wa = woff - (float)wi;
      omx_lfo_advance(&cs->wow);
      if (dl != 0) tapL = omx_fxdelay_read_frac(s->ring_l, w, omx_fxdelay_clamp(dl + wi, cap), wa, cap);
      if (dr != 0) tapR = omx_fxdelay_read_frac(s->ring_r, w, omx_fxdelay_clamp(dr + wi, cap), wa, cap);
    }
    /* The wet sum starts AT tap 0's term (never 0 + term, which turns a -0 into +0): one tap at
     * unity is the tap, bit for bit. */
    float wetL = gl[0] * tapL, wetR = gr[0] * tapR;
    float srcL = tapL, srcR = tapR;
    for (uint32_t k = 1; k < nt; k++) {
      const uint32_t rk = omx_lookahead_back(w, td[k], cap);
      float tl = td[k] == 0 ? xl : s->ring_l[rk];
      float tr = td[k] == 0 ? xr : s->ring_r[rk];
      if (wow && td[k] != 0) {
        tl = omx_fxdelay_read_frac(s->ring_l, w, omx_fxdelay_clamp(td[k] + wi, cap), wa, cap);
        tr = omx_fxdelay_read_frac(s->ring_r, w, omx_fxdelay_clamp(td[k] + wi, cap), wa, cap);
      }
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
    /* Tape and analog: the loop low pass after the tone, and analog's saturation after it. */
    float loopL = dampL, loopR = dampR;
    if (tape || analog) {
      cs->lp_l = loopL = dampL * (1.0f - lp) + cs->lp_l * lp;
      cs->lp_r = loopR = dampR * (1.0f - lp) + cs->lp_r * lp;
      if (analog) {
        loopL = omx_fxdelay_soft(loopL);
        loopR = omx_fxdelay_soft(loopR);
      }
    }
    /* ping-pong: each leg's feedback comes from the OTHER leg's damped tap */
    float fbL = p->pingpong ? loopR : loopL;
    float fbR = p->pingpong ? loopL : loopR;
    /* The wet duck, keyed by the louder dry leg. */
    if (ducked) {
      const float g = omx_duck_step(&c->duck, &cs->duck, fabsf(xl) > fabsf(xr) ? xl : xr);
      wetL *= g;
      wetR *= g;
    }
    s->ring_l[w] = xl + fb * fbL;
    s->ring_r[w] = xr + fb * fbR;
    omx_wetdry_mix(&l[i], &r[i], xl, xr, wetL, wetR, dry, mix);
    w = omx_lookahead_fwd(w, 1u, cap);
  }
  s->wpos = w;
  s->damp_l = dampL;
  s->damp_r = dampR;
  if (tape || analog) {
    cs->lp_l = omx_flush(cs->lp_l);
    cs->lp_r = omx_flush(cs->lp_r);
  }
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
  OMX_POST(s->wpos < s->cap, "write-cursor-inside-the-ring");
  OMX_INVARIANT(s->damp_l - s->damp_l == 0.0f && s->damp_r - s->damp_r == 0.0f,
                "damping-state-finite");
}
#undef OMX_CONTRACT_STAGE

/**
 * Process one block IN PLACE with read taps and no colour: omx_fx_delay_process_color with a NULL
 * colour, the delay with taps as it shipped.
 *
 * @param l the left leg, `n` frames, processed in place
 * @param r the right leg, `n` frames, processed in place
 * @param n frames in the block
 * @param p the delay's control atom
 * @param t the read taps, or NULL for the one-tap delay
 * @param s the caller-owned rings and the RT state
 * @param sr the live rate, Hz
 */
static inline void omx_fx_delay_process_taps(float *l, float *r, uint32_t n, const struct omx_fx_delay *p,
                                             const struct omx_fx_delay_taps *t, struct omx_fx_delay_state *s,
                                             float sr) {
  omx_fx_delay_process_color(l, r, n, p, t, NULL, NULL, s, sr);
}

/**
 * Process one block IN PLACE: the one-tap delay, omx_fx_delay_process_taps with no taps. `l`/`r` are
 * the strip's two legs (distinct buffers). A disabled atom / NULL ring is a passthrough. RT-safe:
 * fixed work per sample, no allocation.
 *
 * @param l the left leg, `n` frames, processed in place
 * @param r the right leg, `n` frames, processed in place
 * @param n frames in the block
 * @param p the delay's control atom
 * @param s the caller-owned rings and the RT state
 * @param sr the live rate, Hz
 */
static inline void omx_fx_delay_process(float *l, float *r, uint32_t n,
                                        const struct omx_fx_delay *p, struct omx_fx_delay_state *s,
                                        float sr) {
  omx_fx_delay_process_taps(l, r, n, p, NULL, s, sr);
}

#endif /* OMX_MIX_DELAY_H */
