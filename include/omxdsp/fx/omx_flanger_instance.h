/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_flanger_instance.h — the native flanger as a plugin instance: the shell's core, with no
 * format in it. The same shape as omx-plugins' omx_delay_instance.h (one C core, N shells): the core is
 * `omx_flanger.h`'s `omx_flanger_process`, THE SAME INLINE the console's flanger stage runs, and
 * this file adds no DSP to it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. RINGS. `struct omx_flanger_state` reads two rings; the instance OWNS them, inline, sized for
 *      the deepest sweep at the highest rate the kernel admits (OMX_FLANGER_CAP per leg), and arms
 *      the kernel over the first omx_flanger_instance_cap_for(sr) floats of each. Nothing is
 *      allocated, at init or after; a rate the console does not declare is refused at init.
 *   2. RESOLVE. {@link omx_flanger_instance_resolve} takes the kernel's contract controls in their
 *      declared order and user units (Hz, ms, a signed feedback, percent) and turns them into the
 *      atom, the same conversions resolve_fx_flanger (mixer_rt.c)
 *      makes, with every knob clamped through omx_param into the declared travel
 *      (FLANGER_*_RANGE, omx_contract_limits.h): a foreign host's port is not the console's codec,
 *      and a non-finite word reads as the declared default.
 *   3. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass CLEARS the rings and the feedback words and restarts the oscillator, so
 * a re-enabled flanger does not replay a tail buffered before the bypass. LATENCY IS ZERO: the
 * dry path is frame-aligned with the input.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxFlangerInstance}. The state
 * points into the instance's own rings, so an instance is initialised where it lives and is never
 * copied after init.
 */
#ifndef OMX_FLANGER_INSTANCE_H
#define OMX_FLANGER_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_flanger.h>
#include <omxdsp/omx_param.h>

#define OMX_FLANGER_INSTANCE_CHANNELS 2
#define OMX_FLANGER_INSTANCE_LATENCY_FRAMES 0.0f

/** Port defaults: the declaration's (FLANGER_*_RANGE.default). */
#define OMX_FLANGER_INSTANCE_DEPTH_MS_DEFAULT ((float)OMX_FLANGER_DEPTH_RANGE_DEFAULT)
#define OMX_FLANGER_INSTANCE_RATE_HZ_DEFAULT ((float)OMX_FLANGER_RATE_RANGE_DEFAULT)
#define OMX_FLANGER_INSTANCE_FEEDBACK_DEFAULT ((float)OMX_FLANGER_FEEDBACK_RANGE_DEFAULT)
#define OMX_FLANGER_INSTANCE_MIX_PCT_DEFAULT ((float)OMX_FLANGER_MIX_RANGE_DEFAULT)

/** One instance. It owns its rings; the kernel is armed over the first `cap` floats of each. */
typedef struct {
  float sr;
  struct omx_flanger atom;
  struct omx_flanger_state state;
  float ring_l[OMX_FLANGER_CAP];
  float ring_r[OMX_FLANGER_CAP];
  uint32_t cap;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the rings. */
  int was_engaged;
  int ready;
} OmxFlangerInstance;

/** The ring, per leg, the deepest sweep needs at `sr`: the floats of each inline ring the kernel uses. */
static inline uint32_t omx_flanger_instance_cap_for(float sr) {
  return omx_fdelay_cap_for((OMX_FLANGER_BASE_MS + OMX_FLANGER_MAX_DEPTH_MS) * 0.001f * sr,
                            OMX_FLANGER_ORDER);
}

/**
 * Bind an instance to its rate, over its own rings. Returns 1 when usable, 0 when not (a refused
 * init leaves `ready` clear and run() is the identity). A rate that is not declared, or whose
 * sweep the inline rings cannot hold, is refused. Allocates nothing.
 */
static inline int omx_flanger_instance_init(OmxFlangerInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_declared(sr)) return 0;
  const uint32_t cap = omx_flanger_instance_cap_for(sr);
  if (cap > OMX_FLANGER_CAP) return 0;
  s->sr = sr;
  s->cap = cap;
  if (omx_flanger_state_init(&s->state, s->ring_l, s->ring_r, cap) != OMX_FDELAY_OK) return 0;
  s->atom.enabled = 0;
  s->ready = 1;
  return 1;
}

/** Clear the rings and restart the oscillator — the re-enable recipe. A bounded memset over the
 * instance's own rings, no allocation. */
#define OMX_CONTRACT_STAGE "flanger/instance-clear"
static inline void omx_flanger_instance_clear(OmxFlangerInstance *s) {
  if (!s || !s->ready) return;
  memset(s->ring_l, 0, (size_t)s->cap * sizeof(float));
  memset(s->ring_r, 0, (size_t)s->cap * sizeof(float));
  (void)omx_flanger_state_init(&s->state, s->ring_l, s->ring_r, s->cap);
  OMX_POST(s->ring_l[0] == 0.0f && s->ring_r[s->cap - 1u] == 0.0f && s->state.lfo.phase == 0.0f &&
               s->state.fb_l == 0.0f && s->state.fb_r == 0.0f,
           "cleared-ring-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the kernel's contract controls, in their declared order and
 * user units: `rate` (Hz), `depth` (ms), `feedback` (signed) and `mix` (percent), each clamped
 * into its declared travel, a non-finite word reading as the declared default; the kernel's own
 * feedback clamp stays the last line before the recursion.
 */
#define OMX_CONTRACT_STAGE "flanger/instance-resolve"
static inline void omx_flanger_instance_resolve(OmxFlangerInstance *s, int bypass, float rate,
                                                float depth, float feedback, float mix) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_flanger_process states, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_flanger_instance_clear(s);
  s->was_engaged = engaged;
  struct omx_flanger *o = &s->atom;
  o->enabled = engaged;
  o->base_samples = OMX_FLANGER_BASE_MS * 0.001f * s->sr;
  o->depth_samples = omx_clamp_or(depth, (float)OMX_FLANGER_DEPTH_RANGE_MIN,
                                  (float)OMX_FLANGER_DEPTH_RANGE_MAX,
                                  OMX_FLANGER_INSTANCE_DEPTH_MS_DEFAULT) *
                     0.001f * s->sr;
  o->lfo_inc = omx_lfo_inc(omx_clamp_or(rate, (float)OMX_FLANGER_RATE_RANGE_MIN,
                                        (float)OMX_FLANGER_RATE_RANGE_MAX,
                                        OMX_FLANGER_INSTANCE_RATE_HZ_DEFAULT),
                           s->sr);
  o->feedback = omx_flanger_clamp_fb(omx_clamp_or(feedback, OMX_FLANGER_FEEDBACK_RANGE_MIN,
                                                  OMX_FLANGER_FEEDBACK_RANGE_MAX,
                                                  OMX_FLANGER_INSTANCE_FEEDBACK_DEFAULT));
  o->mix = 0.01f * omx_clamp_or(mix, (float)OMX_FLANGER_MIX_RANGE_MIN,
                                (float)OMX_FLANGER_MIX_RANGE_MAX, OMX_FLANGER_INSTANCE_MIX_PCT_DEFAULT);
  OMX_POST(o->mix >= 0.0f && o->mix <= 1.0f && o->feedback >= -OMX_FLANGER_FB_MAX &&
               o->feedback <= OMX_FLANGER_FB_MAX && o->depth_samples >= 0.0f &&
               o->lfo_inc >= 0.0f && o->lfo_inc < 1.0f &&
               o->base_samples + o->depth_samples <=
                   omx_fdelay_max_delay(s->state.line_l.cap, s->state.line_l.order),
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "flanger/instance-run"
static inline void omx_flanger_instance_run(OmxFlangerInstance *s, const float *in_l,
                                           const float *in_r, float *out_l, float *out_r,
                                           uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memcpy(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memcpy(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_flanger_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_FLANGER_INSTANCE_H */
