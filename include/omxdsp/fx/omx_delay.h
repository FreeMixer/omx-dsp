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

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_lookahead.h>
#include <omxdsp/omx_param.h>

/** Max FX delay time (ms). ~2 s covers slow ambient repeats. Read from the generated header
 * (OMX_DELAY_TIME_MS_MAX, FX_DELAY_TIME_RANGE.max — F7): this ring's ceiling and the TS travel
 * the row offers are the same declared fact, not two numbers a test has to hold equal. */
#define OMX_FXDELAY_MAX_MS ((int)OMX_DELAY_TIME_MS_MAX)
/** Highest graph rate the ring is sized for: the declared rate roof (RT_HARD_TARGET_RATE), generated. */
#define OMX_FXDELAY_MAX_RATE OMX_RT_HARD_TARGET_RATE
/** Per-leg ring capacity, samples: MAX_MS at MAX_RATE, +1 so the full max delay is usable. */
#define OMX_FXDELAY_CAP (((OMX_FXDELAY_MAX_RATE / 1000) * OMX_FXDELAY_MAX_MS) + 1)

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
 * `resolve_fx_delay` (mixer_rt.c) and the LV2 plugin's port resolve (omx_delay_instance.h) — so a
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
 * Process one block IN PLACE. `l`/`r` are the strip's two legs (distinct buffers). A disabled atom /
 * NULL ring is a passthrough. RT-safe: fixed work per sample, no allocation.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fx-delay"
static inline void omx_fx_delay_process(float *l, float *r, uint32_t n,
                                        const struct omx_fx_delay *p, struct omx_fx_delay_state *s,
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
  OMX_PRE(sr <= 0.0f || OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  if (!p->enabled || n == 0 || s->ring_l == NULL || s->ring_r == NULL || s->cap == 0) return;
  uint32_t cap = s->cap;
  uint32_t dl = omx_fxdelay_clamp(p->d_l, cap);
  uint32_t dr = omx_fxdelay_clamp(p->d_r, cap);
  /* A non-finite feedback is no feedback at all — the flanger's rule (omx_flanger_clamp_fb): one
   * NaN written into the ring circulates until the insert is toggled
   * (2026-09-25-native-fx-rt-review.md F5). The ceiling 0.99 < 1: repeats never grow unbounded.
   * A non-finite mix is dry. */
  float fb = omx_clamp_or(p->feedback, 0.0f, 0.99f, 0.0f);
  float mix = omx_clamp_or(p->mix, 0.0f, 1.0f, 0.0f);
  float dry = 1.0f - mix;
  /* R-058: the tone knob is quoted at 96 kHz and raised to REF/sr HERE, once per block, so the
   * repeats darken by the same filter at every clock. */
  float tone = omx_fxdelay_tone_pole(p->tone, sr);
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
    /* one-pole low-pass in the feedback path (tone, Freeverb-style damping): tone=0 passes the tap
     * through (bright), tone->1 freezes it toward its running average (dark repeats). */
    dampL = tapL * (1.0f - tone) + dampL * tone;
    dampR = tapR * (1.0f - tone) + dampR * tone;
    /* ping-pong: each leg's feedback comes from the OTHER leg's damped tap */
    float fbL = p->pingpong ? dampR : dampL;
    float fbR = p->pingpong ? dampL : dampR;
    s->ring_l[w] = xl + fb * fbL;
    s->ring_r[w] = xr + fb * fbR;
    l[i] = dry * xl + mix * tapL;
    r[i] = dry * xr + mix * tapR;
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

#endif /* OMX_MIX_DELAY_H */
