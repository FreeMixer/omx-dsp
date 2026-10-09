/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_delay_instance.h — the native FX delay as a plugin instance: the shell's core, with no format
 * in it. The core is `omx_delay.h`'s `omx_fx_delay_process` — THE SAME INLINE the console's delay
 * stage runs (mix_lane.h step OMX_STAGE_DELAY) — and this file adds no DSP to it. It adds what a
 * host's port model needs and the kernel's atom does not carry:
 *
 *   1. RINGS. `struct omx_fx_delay_state` reads two rings the kernel leaves to its caller; the
 *      instance OWNS them, inline, {@link OMX_FXDELAY_CAP} floats each (the declared 2 s ceiling at
 *      192 kHz), so the full travel is reachable at every declared rate and nothing is allocated, at
 *      init or after. An instance is initialised where it lives and is never copied after init (the
 *      state points into its own rings).
 *   2. RESOLVE. {@link omx_delay_instance_resolve} takes the delay kernel's contract controls in
 *      their declared order and user units: the time (ms) through the kernel's own
 *      {@link omx_fxdelay_ms_to_samples} — shared with the console's resolve, never a second
 *      conversion — the feedback, the tone and the mix (0..1) and the ping-pong switch. Every travel
 *      is clamped into its declared range (FX_DELAY_TIME_RANGE, FX_DELAY_FEEDBACK_RANGE, which stops
 *      short of the unity the kernel refuses, DELAY_TONE_RANGE, DELAY_MIX_RANGE), a non-finite word
 *      reading as the declared default: a foreign host's port is not the console's codec.
 *   3. IN -> OUT. The kernel works IN PLACE on the two legs; run() copies in to out where they
 *      differ (they may alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * A time change moves the read tap and MAY CLICK, as mix_delay.h states for the console: the face
 * adds no cross-fade the console does not have. Re-engaging from bypass CLEARS the rings and the
 * per-leg damping, the console's own re-enable recipe (mixer_strip.c), so a re-enabled delay starts
 * silent instead of bursting a tail buffered before the bypass: a bounded memset over the inline
 * rings, no allocation.
 *
 * LATENCY IS ZERO, AND PUBLISHED: the delay IS the effect a host must not compensate away, and the
 * dry path is frame-aligned with the input.
 *
 * Moved from omx-plugins' plugins/omx-delay/omx_delay_instance.h (the caller-ring form), into the
 * instance face's shape (spec 2026-10-09-plugin-from-contract §2).
 */
#ifndef OMX_DELAY_INSTANCE_H
#define OMX_DELAY_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/fx/omx_delay.h>
#include <omxdsp/omx_param.h>

#define OMX_DELAY_INSTANCE_CHANNELS 2
#define OMX_DELAY_INSTANCE_LATENCY_FRAMES 0.0f

/** Control defaults: the contract's (FX_DELAY_*_RANGE.default, DELAY_*_RANGE.default, DELAY_PINGPONGS). */
#define OMX_DELAY_INSTANCE_TIME_DEFAULT ((float)OMX_FX_DELAY_TIME_RANGE_DEFAULT)
#define OMX_DELAY_INSTANCE_FEEDBACK_DEFAULT ((float)OMX_FX_DELAY_FEEDBACK_RANGE_DEFAULT)
#define OMX_DELAY_INSTANCE_TONE_DEFAULT ((float)OMX_DELAY_TONE_RANGE_DEFAULT)
#define OMX_DELAY_INSTANCE_MIX_DEFAULT ((float)OMX_DELAY_MIX_RANGE_DEFAULT)
#define OMX_DELAY_INSTANCE_PINGPONG_DEFAULT ((int)OMX_DELAY_PINGPONGS_DEFAULT)

/** One instance: the atom, the kernel's state over the instance's own rings. */
typedef struct {
  float sr;
  struct omx_fx_delay atom;
  struct omx_fx_delay_state state;
  float ring_l[OMX_FXDELAY_CAP];
  float ring_r[OMX_FXDELAY_CAP];
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the rings. */
  int was_engaged;
  int ready;
} OmxDelayInstance;

/**
 * Bind an instance to its rate, over its own rings. Returns 1 when usable, 0 when not: a rate the
 * console does not declare is refused, `ready` stays clear and run() is the identity. Allocates
 * nothing.
 */
static inline int omx_delay_instance_init(OmxDelayInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_declared(sr)) return 0;
  s->sr = sr;
  s->state.ring_l = s->ring_l;
  s->state.ring_r = s->ring_r;
  s->state.cap = (uint32_t)OMX_FXDELAY_CAP;
  s->atom.enabled = 0;
  s->ready = 1;
  return 1;
}

/** The engage-edge clear: both rings and the per-leg damping back to silence. RT-safe: a bounded
 * memset over the inline rings. */
#define OMX_CONTRACT_STAGE "fx-delay/instance-clear"
static inline void omx_delay_instance_clear(OmxDelayInstance *s) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). A cleared delay holds nothing: the next tap read, at any delay, is
   * silence until the ring has been written that far. */
  memset(s->ring_l, 0, sizeof s->ring_l);
  memset(s->ring_r, 0, sizeof s->ring_r);
  s->state.wpos = 0u;
  s->state.damp_l = s->state.damp_r = 0.0f;
  OMX_POST(s->ring_l[0] == 0.0f && s->ring_r[OMX_FXDELAY_CAP - 1] == 0.0f && s->state.wpos == 0u,
           "cleared-ring-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * Resolve the host's control values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the delay kernel's contract controls, in their declared
 * order and user units: `fx_delay_time` (ms, both legs), `fx_delay_feedback`, `tone` and `mix`
 * (0..1) are clamped into their declared travels, a non-finite word reading as the declared
 * default; `pingpong` is the DELAY_PINGPONGS index (0 off, 1 on), any other value reading as the
 * declared default.
 */
#define OMX_CONTRACT_STAGE "fx-delay/instance-resolve"
static inline void omx_delay_instance_resolve(OmxDelayInstance *s, int bypass, float fx_delay_time,
                                              float fx_delay_feedback, float tone, float mix, int pingpong) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE omx_fx_delay_process
   * states — feedback strictly below unity, mix and tone in unit range, both taps inside the ring —
   * whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_delay_instance_clear(s);
  s->was_engaged = engaged;
  struct omx_fx_delay *o = &s->atom;
  o->enabled = engaged;
  const float ms = omx_clamp_or(fx_delay_time, (float)OMX_FX_DELAY_TIME_RANGE_MIN, (float)OMX_FX_DELAY_TIME_RANGE_MAX,
                                OMX_DELAY_INSTANCE_TIME_DEFAULT);
  o->d_l = o->d_r = omx_fxdelay_ms_to_samples(ms, s->sr);
  o->feedback = omx_clamp_or(fx_delay_feedback, (float)OMX_FX_DELAY_FEEDBACK_RANGE_MIN,
                             (float)OMX_FX_DELAY_FEEDBACK_RANGE_MAX, OMX_DELAY_INSTANCE_FEEDBACK_DEFAULT);
  o->tone = omx_clamp_or(tone, (float)OMX_DELAY_TONE_RANGE_MIN, (float)OMX_DELAY_TONE_RANGE_MAX,
                         OMX_DELAY_INSTANCE_TONE_DEFAULT);
  o->mix = omx_clamp_or(mix, (float)OMX_DELAY_MIX_RANGE_MIN, (float)OMX_DELAY_MIX_RANGE_MAX,
                        OMX_DELAY_INSTANCE_MIX_DEFAULT);
  o->pingpong = pingpong == 0 || pingpong == 1 ? pingpong : OMX_DELAY_INSTANCE_PINGPONG_DEFAULT;
  OMX_POST(o->feedback >= 0.0f && o->feedback < 1.0f && o->mix >= 0.0f && o->mix <= 1.0f && o->tone >= 0.0f &&
               o->tone <= 1.0f && o->d_l <= s->state.cap && o->d_r <= s->state.cap,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel in
 * place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity byte for byte.
 */
#define OMX_CONTRACT_STAGE "fx-delay/instance-run"
static inline void omx_delay_instance_run(OmxDelayInstance *s, const float *in_l, const float *in_r, float *out_l,
                                          float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_fx_delay_process(out_l, out_r, n, &s->atom, &s->state, s->sr);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces: none, at every rate (the delay is the effect). */
static inline uint32_t omx_delay_instance_latency(const OmxDelayInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_DELAY_INSTANCE_H */
