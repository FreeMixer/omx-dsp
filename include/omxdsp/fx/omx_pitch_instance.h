/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_pitch_instance.h — the native pitch shifter as a plugin instance: the shell's core, with no
 * format in it. The same shape as omx_chorus_instance.h (one C core, N shells): the core is
 * `omx_pitch.h`'s `omx_pitch_process`, THE SAME INLINE the console's pitch stage runs, and this
 * file adds no DSP to it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. RINGS. `struct omx_pitch_state` reads two rings; the instance OWNS them, inline, sized for
 *      the window at the highest declared rate (OMX_PITCH_INSTANCE_RING_FLOATS per leg), and arms
 *      the kernel over the first omx_pitch_cap_for(sr) floats of each. Nothing is allocated, at
 *      init or after; a rate the inline rings cannot serve is refused at init.
 *   2. RESOLVE. {@link omx_pitch_instance_resolve} takes the kernel's contract controls in their
 *      declared order and user units (semitones, cents, percent), clamps each into its declared
 *      travel (OMX_PITCH_*, omx_contract_limits.h; a non-finite word reads as the declared
 *      default) and hands them to the kernel's own omx_pitch_resolve(). That resolve is `exp2f`
 *      and a filter design, so it runs only when a clamped control MOVED.
 *   3. IN -> OUT. The kernel works IN PLACE on two distinct legs; run() copies in to out where
 *      they differ (they may alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass CLEARS the rings and restarts the ramp, so a re-enabled shifter does not
 * replay audio buffered before the bypass. LATENCY IS PUBLISHED AS ZERO, as the console's stage
 * reports it: the dry path is frame-aligned and the window's delay is the effect.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxPitchInstance}. The state
 * points into the instance's own rings, so an instance is initialised where it lives and is never
 * copied after init.
 */
#ifndef OMX_PITCH_INSTANCE_H
#define OMX_PITCH_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_pitch.h>
#include <omxdsp/omx_param.h>

#define OMX_PITCH_INSTANCE_CHANNELS 2
#define OMX_PITCH_INSTANCE_LATENCY_FRAMES 0.0f

/** The highest rate, Hz, the inline rings are sized for: the top of the declared rate set. */
#define OMX_PITCH_INSTANCE_RATE_MAX 192000u
/** Floats per inline ring: the window at OMX_PITCH_INSTANCE_RATE_MAX plus the read's lookbehind
 * and guard, with room to spare; init refuses a rate whose omx_pitch_cap_for() exceeds it. */
#define OMX_PITCH_INSTANCE_RING_FLOATS ((uint32_t)OMX_PITCH_WINDOW_MS * (OMX_PITCH_INSTANCE_RATE_MAX / 1000u) + 64u)

/** One instance. It owns its rings; the kernel is armed over the first `cap` floats of each. */
typedef struct {
  float sr;
  struct omx_pitch atom;
  struct omx_pitch_state state;
  float ring_l[OMX_PITCH_INSTANCE_RING_FLOATS];
  float ring_r[OMX_PITCH_INSTANCE_RING_FLOATS];
  uint32_t cap;
  /** The clamped controls the atom was last resolved from; `resolved` 0 until the first. */
  float semitones, cents, mix;
  int resolved;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the rings. */
  int was_engaged;
  int ready;
} OmxPitchInstance;

/** The ring, per leg, the window needs at `sr`: the floats of each inline ring the kernel uses. */
static inline uint32_t omx_pitch_instance_cap_for(float sr) { return omx_pitch_cap_for(sr); }

/**
 * Bind an instance to its rate, over its own rings. Returns 1 when usable, 0 when not (a refused
 * init leaves `ready` clear and run() is the identity). A rate that is not declared, or whose
 * window the inline rings cannot hold, is refused. Allocates nothing.
 */
static inline int omx_pitch_instance_init(OmxPitchInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_declared(sr)) return 0;
  const uint32_t cap = omx_pitch_instance_cap_for(sr);
  if (cap > OMX_PITCH_INSTANCE_RING_FLOATS) return 0;
  s->sr = sr;
  s->cap = cap;
  if (omx_pitch_state_init(&s->state, s->ring_l, s->ring_r, cap) != OMX_FDELAY_OK) return 0;
  s->atom.enabled = 0;
  s->ready = 1;
  return 1;
}

/** Clear the rings and restart the ramp — the re-enable recipe. A bounded memset over the
 * instance's own rings, no allocation. */
#define OMX_CONTRACT_STAGE "pitch/instance-clear"
static inline void omx_pitch_instance_clear(OmxPitchInstance *s) {
  if (!s || !s->ready) return;
  memset(s->ring_l, 0, (size_t)s->cap * sizeof(float));
  memset(s->ring_r, 0, (size_t)s->cap * sizeof(float));
  (void)omx_pitch_state_init(&s->state, s->ring_l, s->ring_r, s->cap);
  OMX_POST(s->ring_l[0] == 0.0f && s->ring_r[s->cap - 1u] == 0.0f && s->state.ramp == 0u,
           "cleared-ring-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the kernel's contract controls, in their declared order and
 * user units: `semitones`, `cents` and `mix` (percent), each clamped into its declared travel, a
 * non-finite word reading as the declared default.
 */
#define OMX_CONTRACT_STAGE "pitch/instance-resolve"
static inline void omx_pitch_instance_resolve(OmxPitchInstance *s, int bypass, float semitones, float cents,
                                              float mix) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind was resolved from controls inside the
   * declared travel, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_pitch_instance_clear(s);
  s->was_engaged = engaged;
  semitones = omx_clamp_or(semitones, OMX_PITCH_SEMITONES_MIN, OMX_PITCH_SEMITONES_MAX,
                           OMX_PITCH_SEMITONES_DEFAULT);
  cents = omx_clamp_or(cents, OMX_PITCH_CENTS_MIN, OMX_PITCH_CENTS_MAX, OMX_PITCH_CENTS_DEFAULT);
  mix = omx_clamp_or(mix, OMX_PITCH_MIX_MIN, OMX_PITCH_MIX_MAX, OMX_PITCH_MIX_DEFAULT);
  const int moved = !s->resolved || s->semitones != semitones || s->cents != cents || s->mix != mix;
  if (moved) {
    omx_pitch_resolve(&s->atom, engaged, semitones, cents, mix, s->sr);
    s->semitones = semitones;
    s->cents = cents;
    s->mix = mix;
    s->resolved = 1;
  }
  s->atom.enabled = engaged;
  OMX_POST(s->atom.mix >= 0.0f && s->atom.mix <= 1.0f && s->atom.step < 0x80000000u &&
               omx_fdelay_fits_ring(&s->state.line_l, 1, omx_fdelay_min_delay(OMX_PITCH_ORDER) + s->atom.window),
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias; `out_l` and `out_r` are distinct. Not ready, or
 * bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "pitch/instance-run"
static inline void omx_pitch_instance_run(OmxPitchInstance *s, const float *in_l, const float *in_r,
                                          float *out_l, float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_pitch_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance publishes: none (see the header note). */
static inline uint32_t omx_pitch_instance_latency(const OmxPitchInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_PITCH_INSTANCE_H */
