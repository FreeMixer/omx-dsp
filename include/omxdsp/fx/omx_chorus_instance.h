/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_chorus_instance.h — the native chorus as a plugin instance: the shell's core, with no
 * format in it. The same shape as omx_delay_instance.h (one C core, N shells): the core is
 * `omx_chorus.h`'s `omx_chorus_process`, THE SAME INLINE the console's chorus stage runs, and
 * this file adds no DSP to it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. RINGS. `struct omx_chorus_state` is caller-owned by design; the shell's caller hands the
 *      two rings in through {@link omx_chorus_instance_init}. A ring too short for the deepest
 *      sweep at the instance rate is refused there, not discovered by the kernel's PRE.
 *   2. RESOLVE. {@link omx_chorus_instance_resolve} turns the operator's units (ms, Hz, percent,
 *      a voice count, turns) into the atom, the same conversions resolve_fx_chorus (mixer_rt.c)
 *      makes, with every knob clamped through omx_param into the declared travel
 *      (CHORUS_*_RANGE, omx_contract_limits.h): a foreign host's port is not the console's codec,
 *      and a non-finite word reads as the declared default.
 *   3. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass CLEARS the rings and restarts the oscillator, so a re-enabled chorus
 * does not replay a tail buffered before the bypass. LATENCY IS ZERO: the dry path is
 * frame-aligned with the input.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxChorusInstance}.
 */
#ifndef OMX_CHORUS_INSTANCE_H
#define OMX_CHORUS_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_chorus.h>
#include <omxdsp/omx_param.h>

#define OMX_CHORUS_INSTANCE_CHANNELS 2
#define OMX_CHORUS_INSTANCE_LATENCY_FRAMES 0.0f

/** Port defaults: the declaration's (CHORUS_*_RANGE.default). */
#define OMX_CHORUS_INSTANCE_VOICES_DEFAULT ((float)OMX_CHORUS_VOICES_RANGE_DEFAULT)
#define OMX_CHORUS_INSTANCE_DEPTH_MS_DEFAULT ((float)OMX_CHORUS_DEPTH_RANGE_DEFAULT)
#define OMX_CHORUS_INSTANCE_RATE_HZ_DEFAULT ((float)OMX_CHORUS_RATE_RANGE_DEFAULT)
#define OMX_CHORUS_INSTANCE_MIX_PCT_DEFAULT ((float)OMX_CHORUS_MIX_RANGE_DEFAULT)
#define OMX_CHORUS_INSTANCE_SPREAD_DEFAULT ((float)OMX_CHORUS_SPREAD_RANGE_DEFAULT)

/** One instance. The rings are NOT owned here; {@link omx_chorus_instance_init} is given them. */
typedef struct {
  float sr;
  struct omx_chorus atom;
  struct omx_chorus_state state;
  float *ring_l;
  float *ring_r;
  uint32_t cap;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the rings. */
  int was_engaged;
  int ready;
} OmxChorusInstance;

/** The ring, per leg, the deepest sweep needs at `sr`: allocate this many floats per leg. */
static inline uint32_t omx_chorus_instance_cap_for(float sr) {
  return omx_fdelay_cap_for((OMX_CHORUS_BASE_MS + OMX_CHORUS_MAX_DEPTH_MS) * 0.001f * sr,
                            OMX_CHORUS_ORDER);
}

/**
 * Bind an instance to its rate and the caller's two rings of `cap` floats each. Returns 1 when
 * usable, 0 when not (a refused init leaves `ready` clear and run() is the identity).
 */
static inline int omx_chorus_instance_init(OmxChorusInstance *s, float sr, float *ring_l,
                                           float *ring_r, uint32_t cap) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 0.0f) || sr > (float)OMX_CHORUS_MAX_RATE || !ring_l || !ring_r ||
      cap < omx_chorus_instance_cap_for(sr))
    return 0;
  s->sr = sr;
  s->ring_l = ring_l;
  s->ring_r = ring_r;
  s->cap = cap;
  if (omx_chorus_state_init(&s->state, ring_l, ring_r, cap) != OMX_FDELAY_OK) return 0;
  s->atom.enabled = 0;
  s->ready = 1;
  return 1;
}

/** Clear the rings and restart the oscillator — the re-enable recipe. A bounded memset over the
 * caller's fixed rings, no allocation. */
#define OMX_CONTRACT_STAGE "chorus/instance-clear"
static inline void omx_chorus_instance_clear(OmxChorusInstance *s) {
  if (!s || !s->ready) return;
  memset(s->ring_l, 0, (size_t)s->cap * sizeof(float));
  memset(s->ring_r, 0, (size_t)s->cap * sizeof(float));
  (void)omx_chorus_state_init(&s->state, s->ring_l, s->ring_r, s->cap);
  OMX_POST(s->ring_l[0] == 0.0f && s->ring_r[s->cap - 1u] == 0.0f && s->state.lfo.phase == 0.0f,
           "cleared-ring-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. `voices` rounds to 1..OMX_CHORUS_MAX_VOICES; `depth_ms`, `rate_hz`,
 * `mix_pct` and `spread` are clamped into their declared travels, a non-finite word reading as
 * the declared default.
 */
#define OMX_CONTRACT_STAGE "chorus/instance-resolve"
static inline void omx_chorus_instance_resolve(OmxChorusInstance *s, int bypass, float voices,
                                               float depth_ms, float rate_hz, float mix_pct,
                                               float spread) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_chorus_process states, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_chorus_instance_clear(s);
  s->was_engaged = engaged;
  struct omx_chorus *o = &s->atom;
  o->enabled = engaged;
  const float v = omx_clamp_or(voices, (float)OMX_CHORUS_VOICES_RANGE_MIN,
                               (float)OMX_CHORUS_VOICES_RANGE_MAX, OMX_CHORUS_INSTANCE_VOICES_DEFAULT);
  o->voices = (int)(v + 0.5f);
  o->base_samples = OMX_CHORUS_BASE_MS * 0.001f * s->sr;
  o->depth_samples = omx_clamp_or(depth_ms, (float)OMX_CHORUS_DEPTH_RANGE_MIN,
                                  (float)OMX_CHORUS_DEPTH_RANGE_MAX,
                                  OMX_CHORUS_INSTANCE_DEPTH_MS_DEFAULT) *
                     0.001f * s->sr;
  o->lfo_inc = omx_lfo_inc(omx_clamp_or(rate_hz, (float)OMX_CHORUS_RATE_RANGE_MIN,
                                        (float)OMX_CHORUS_RATE_RANGE_MAX,
                                        OMX_CHORUS_INSTANCE_RATE_HZ_DEFAULT),
                           s->sr);
  o->mix = 0.01f * omx_clamp_or(mix_pct, (float)OMX_CHORUS_MIX_RANGE_MIN,
                                (float)OMX_CHORUS_MIX_RANGE_MAX, OMX_CHORUS_INSTANCE_MIX_PCT_DEFAULT);
  o->spread = omx_clamp_or(spread, (float)OMX_CHORUS_SPREAD_RANGE_MIN, OMX_CHORUS_SPREAD_MAX,
                           OMX_CHORUS_INSTANCE_SPREAD_DEFAULT);
  OMX_POST(o->voices >= 1 && o->voices <= OMX_CHORUS_MAX_VOICES && o->mix >= 0.0f &&
               o->mix <= 1.0f && o->spread >= 0.0f && o->spread <= OMX_CHORUS_SPREAD_MAX &&
               o->depth_samples >= 0.0f && o->lfo_inc >= 0.0f && o->lfo_inc < 1.0f &&
               o->base_samples + o->depth_samples <=
                   omx_fdelay_max_delay(s->state.line_l.cap, s->state.line_l.order),
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "chorus/instance-run"
static inline void omx_chorus_instance_run(OmxChorusInstance *s, const float *in_l,
                                           const float *in_r, float *out_l, float *out_r,
                                           uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memcpy(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memcpy(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_chorus_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_CHORUS_INSTANCE_H */
