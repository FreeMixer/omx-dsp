/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_phaser_instance.h — the native phaser as a plugin instance: the shell's core, with no format
 * in it. The same shape as omx_tremolo_instance.h (one C core, N shells): the core is
 * `omx_phaser.h`'s `omx_phaser_process`, THE SAME INLINE the console's phaser stage runs, and this
 * file adds no DSP to it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. RESOLVE. {@link omx_phaser_instance_resolve} takes the kernel's contract controls in their
 *      declared order and user units (Hz, Hz, octaves, a section count, a signed resonance,
 *      percent) and turns them into the atom: `lfo_inc = omx_lfo_inc(rate_hz, sr)`, the section
 *      count rounded to the nearest even member, mix over 100. Every knob is clamped through
 *      omx_param into its declared travel (PHASER_*_RANGE, omx_contract_limits.h): a foreign
 *      host's port is not the console's codec, and a non-finite word reads as the declared default.
 *   2. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass CLEARS the state (sections, feedback words, oscillator, ramp), so a
 * re-enabled phaser does not ring a resonance built before the bypass. LATENCY IS ZERO: the dry
 * path is frame-aligned with the input. The state is inline; nothing is caller-owned.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxPhaserInstance}.
 */
#ifndef OMX_PHASER_INSTANCE_H
#define OMX_PHASER_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_phaser.h>
#include <omxdsp/omx_param.h>

#define OMX_PHASER_INSTANCE_CHANNELS 2
#define OMX_PHASER_INSTANCE_LATENCY_FRAMES 0.0f

/** Port defaults: the declaration's (PHASER_*_RANGE.default). */
#define OMX_PHASER_INSTANCE_RATE_HZ_DEFAULT ((float)OMX_PHASER_RATE_RANGE_DEFAULT)
#define OMX_PHASER_INSTANCE_BASE_HZ_DEFAULT ((float)OMX_PHASER_BASE_RANGE_DEFAULT)
#define OMX_PHASER_INSTANCE_DEPTH_OCT_DEFAULT ((float)OMX_PHASER_DEPTH_RANGE_DEFAULT)
#define OMX_PHASER_INSTANCE_STAGES_DEFAULT ((float)OMX_PHASER_STAGES_RANGE_DEFAULT)
#define OMX_PHASER_INSTANCE_FEEDBACK_DEFAULT ((float)OMX_PHASER_FEEDBACK_RANGE_DEFAULT)
#define OMX_PHASER_INSTANCE_MIX_DEFAULT ((float)OMX_PHASER_MIX_RANGE_DEFAULT)

/** One instance: the atom the host's ports resolve to and the state it drives. */
typedef struct {
  float sr;
  struct omx_phaser atom;
  struct omx_phaser_state state;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the state. */
  int was_engaged;
  int ready;
} OmxPhaserInstance;

/**
 * Bind an instance to its rate. Returns 1 when usable, 0 when not (a refused init leaves `ready`
 * clear and run() is the identity). The rate must be finite and high enough that the sweep's
 * declared top (OMX_PHASER_F_TOP_HZ) stays below Nyquist, the kernel's own precondition.
 */
static inline int omx_phaser_instance_init(OmxPhaserInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 0.0f) || sr - sr != 0.0f || !((float)OMX_PHASER_F_TOP_HZ < 0.5f * sr)) return 0;
  s->sr = sr;
  omx_phaser_state_init(&s->state);
  s->atom.enabled = 0;
  s->atom.stages = (int)OMX_PHASER_STAGES_RANGE_DEFAULT;
  s->ready = 1;
  return 1;
}

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the kernel's contract controls, in their declared order and
 * user units: `rate_hz` (Hz), `base_hz` (Hz), `depth_oct` (octaves), `stages` (a section count,
 * rounded to the nearest even member), `feedback` (signed) and `mix` (percent), each clamped into
 * its declared travel, a non-finite word reading as the declared default.
 */
#define OMX_CONTRACT_STAGE "phaser/instance-resolve"
static inline void omx_phaser_instance_resolve(OmxPhaserInstance *s, int bypass, float rate_hz,
                                               float base_hz, float depth_oct, float stages,
                                               float feedback, float mix) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_phaser_process states, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_phaser_state_init(&s->state);
  s->was_engaged = engaged;
  struct omx_phaser *o = &s->atom;
  o->enabled = engaged;
  o->lfo_inc = omx_lfo_inc(omx_clamp_or(rate_hz, (float)OMX_PHASER_RATE_RANGE_MIN,
                                        (float)OMX_PHASER_RATE_RANGE_MAX,
                                        OMX_PHASER_INSTANCE_RATE_HZ_DEFAULT),
                           s->sr);
  o->base_hz = omx_clamp_or(base_hz, (float)OMX_PHASER_BASE_RANGE_MIN, (float)OMX_PHASER_BASE_RANGE_MAX,
                            OMX_PHASER_INSTANCE_BASE_HZ_DEFAULT);
  o->depth_oct = omx_clamp_or(depth_oct, (float)OMX_PHASER_DEPTH_RANGE_MIN,
                              (float)OMX_PHASER_DEPTH_RANGE_MAX, OMX_PHASER_INSTANCE_DEPTH_OCT_DEFAULT);
  const float n = omx_clamp_or(stages, (float)OMX_PHASER_STAGES_RANGE_MIN, (float)OMX_PHASER_STAGES_RANGE_MAX,
                               OMX_PHASER_INSTANCE_STAGES_DEFAULT);
  o->stages = omx_phaser_stages_run(2 * (int)lrintf(0.5f * n));
  o->feedback = omx_clamp_or(feedback, (float)OMX_PHASER_FEEDBACK_RANGE_MIN,
                             (float)OMX_PHASER_FEEDBACK_RANGE_MAX, OMX_PHASER_INSTANCE_FEEDBACK_DEFAULT);
  o->mix = 0.01f * omx_clamp_or(mix, (float)OMX_PHASER_MIX_RANGE_MIN, (float)OMX_PHASER_MIX_RANGE_MAX,
                                OMX_PHASER_INSTANCE_MIX_DEFAULT);
  OMX_POST(omx_phaser_stages_legal(o->stages) && fabsf(o->feedback) <= OMX_PHASER_FB_MAX &&
               o->mix >= 0.0f && o->mix <= 1.0f && o->base_hz > 0.0f && o->depth_oct >= 0.0f &&
               o->lfo_inc >= 0.0f && o->lfo_inc < 1.0f,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "phaser/instance-run"
static inline void omx_phaser_instance_run(OmxPhaserInstance *s, const float *in_l,
                                           const float *in_r, float *out_l, float *out_r,
                                           uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memcpy(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memcpy(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_phaser_process(out_l, out_r, n, &s->atom, &s->state, s->sr);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces: none, at every rate. */
static inline uint32_t omx_phaser_instance_latency(const OmxPhaserInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_PHASER_INSTANCE_H */
