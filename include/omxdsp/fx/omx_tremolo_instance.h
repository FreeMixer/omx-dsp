/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_tremolo_instance.h — the native tremolo / auto-pan as a plugin instance: the shell's core,
 * with no format in it. The same shape as omx_flanger_instance.h (one C core, N shells): the core
 * is `omx_tremolo.h`'s `omx_tremolo_process`, THE SAME INLINE the console's tremolo stage runs,
 * and this file adds no DSP to it. It adds what a host's port model needs and the atom does not
 * carry:
 *
 *   1. RESOLVE. {@link omx_tremolo_instance_resolve} turns the operator's units (a mode index, Hz,
 *      percent) into the atom: `lfo_inc = omx_lfo_inc(rate_hz, sr)`, depth and mix over 100, every
 *      knob clamped through omx_param into the declared travel (TREMOLO_*_RANGE,
 *      omx_contract_limits.h): a foreign host's port is not the console's codec, and a non-finite
 *      word reads as the declared default. A mode that is not a member of `enum omx_tremolo_mode`
 *      reads as the tremolo, the stage's come-up mode.
 *   2. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass restarts the oscillator at phase zero, so a re-enabled tremolo begins
 * its cycle at the top of the gain, the same place a fresh one does. LATENCY IS ZERO: the gain is
 * applied to the frame it is computed for. The state is the oscillator alone; nothing is
 * caller-owned.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxTremoloInstance}.
 */
#ifndef OMX_TREMOLO_INSTANCE_H
#define OMX_TREMOLO_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_tremolo.h>
#include <omxdsp/omx_param.h>

#define OMX_TREMOLO_INSTANCE_CHANNELS 2
#define OMX_TREMOLO_INSTANCE_LATENCY_FRAMES 0.0f

/** Port defaults: the declaration's (TREMOLO_*_RANGE.default), the mode the stage comes up at. */
#define OMX_TREMOLO_INSTANCE_MODE_DEFAULT OMX_TREMOLO_MODE_TREMOLO
#define OMX_TREMOLO_INSTANCE_RATE_HZ_DEFAULT ((float)OMX_TREMOLO_RATE_RANGE_DEFAULT)
#define OMX_TREMOLO_INSTANCE_DEPTH_DEFAULT ((float)OMX_TREMOLO_DEPTH_RANGE_DEFAULT)
#define OMX_TREMOLO_INSTANCE_MIX_DEFAULT ((float)OMX_TREMOLO_MIX_RANGE_DEFAULT)

/** One instance: the atom the host's ports resolve to and the oscillator it drives. */
typedef struct {
  float sr;
  struct omx_tremolo atom;
  struct omx_tremolo_state state;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can restart the oscillator. */
  int was_engaged;
  int ready;
} OmxTremoloInstance;

/**
 * Bind an instance to its rate. Returns 1 when usable, 0 when not (a refused init leaves `ready`
 * clear and run() is the identity). The rate must be positive, finite and high enough that the
 * declared top rate stays under the oscillator's half-turn bound.
 */
static inline int omx_tremolo_instance_init(OmxTremoloInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 0.0f) || sr - sr != 0.0f || !((float)OMX_TREMOLO_RATE_RANGE_MAX * 2.0f < sr)) return 0;
  s->sr = sr;
  omx_tremolo_state_init(&s->state);
  s->atom.enabled = 0;
  s->atom.mode = OMX_TREMOLO_INSTANCE_MODE_DEFAULT;
  s->ready = 1;
  return 1;
}

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the kernel's contract controls, in their declared order and
 * user units: `rate_hz` (Hz), `depth` and `mix` (percent) are clamped into their declared travels,
 * a non-finite word reading as the declared default; `mode` is the TREMOLO_MODES index, an
 * `enum omx_tremolo_mode` member (any other value reads as the tremolo).
 */
#define OMX_CONTRACT_STAGE "tremolo/instance-resolve"
static inline void omx_tremolo_instance_resolve(OmxTremoloInstance *s, int bypass, float rate_hz,
                                                float depth, float mix, int mode) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_tremolo_process states, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_tremolo_state_init(&s->state);
  s->was_engaged = engaged;
  struct omx_tremolo *o = &s->atom;
  o->enabled = engaged;
  o->mode = mode == OMX_TREMOLO_MODE_PAN ? OMX_TREMOLO_MODE_PAN : OMX_TREMOLO_MODE_TREMOLO;
  o->lfo_inc = omx_lfo_inc(omx_clamp_or(rate_hz, (float)OMX_TREMOLO_RATE_RANGE_MIN,
                                        (float)OMX_TREMOLO_RATE_RANGE_MAX,
                                        OMX_TREMOLO_INSTANCE_RATE_HZ_DEFAULT),
                           s->sr);
  o->depth = 0.01f * omx_clamp_or(depth, (float)OMX_TREMOLO_DEPTH_RANGE_MIN,
                                  (float)OMX_TREMOLO_DEPTH_RANGE_MAX,
                                  OMX_TREMOLO_INSTANCE_DEPTH_DEFAULT);
  o->mix = 0.01f * omx_clamp_or(mix, (float)OMX_TREMOLO_MIX_RANGE_MIN,
                                (float)OMX_TREMOLO_MIX_RANGE_MAX, OMX_TREMOLO_INSTANCE_MIX_DEFAULT);
  OMX_POST(o->depth >= 0.0f && o->depth <= 1.0f && o->mix >= 0.0f && o->mix <= 1.0f &&
               o->lfo_inc >= 0.0f && o->lfo_inc < 1.0f &&
               (o->mode == OMX_TREMOLO_MODE_TREMOLO || o->mode == OMX_TREMOLO_MODE_PAN),
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "tremolo/instance-run"
static inline void omx_tremolo_instance_run(OmxTremoloInstance *s, const float *in_l,
                                            const float *in_r, float *out_l, float *out_r,
                                            uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memcpy(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memcpy(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_tremolo_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces: none, at every rate (the gain is applied to the
 * frame it is computed for). */
static inline uint32_t omx_tremolo_instance_latency(const OmxTremoloInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_TREMOLO_INSTANCE_H */
