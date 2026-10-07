/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_transient_instance.h — the native transient designer as a plugin instance: the shell's core,
 * with no format in it.
 *
 * The shape is omx-plugins' omx_delay_instance.h's, under the same HRP shells rule (one C core, N shells). The
 * core here is `omx_transient.h`'s `omx_transient_process` — THE SAME INLINE the console's
 * transient stage runs — and this file adds no DSP to it. What it adds is the three things a
 * host's port model needs and the kernel's atom does not carry:
 *
 *   1. STATE. `struct omx_transient_state` is three envelope cascades and a gain word, plain
 *      inline floats, so the instance holds it whole; there is nothing to hand in.
 *   2. RESOLVE. A host publishes control ports as floats in the operator's units (dB, ms).
 *      {@link omx_transient_instance_resolve} clamps every word into the declared travel
 *      (omx_contract_limits.h's OMX_TRANSIENT_*) BEFORE the kernel's own resolve sees it, because
 *      a foreign host's port is not the console's codec and nothing upstream has clamped it: the
 *      three gains read any non-finite word as their declared default, 0 dB (omx_clamp_or — a
 *      NaN gain carries no position), the two times read NaN as the floor and ±Inf as the ends
 *      (omx_clampf — a time is ordered). The kernel's resolve is `expf`/`powf` work, so it runs
 *      only when a control MOVED; a static instance re-resolves nothing.
 *   3. IN -> OUT. The kernel works IN PLACE on the two legs; a host connects separate input and
 *      output buffers (and may connect them to the same memory). run() copies in to out when they
 *      differ and runs the kernel on out. Identity when bypassed.
 *
 * Re-engaging from bypass CLEARS the state (the followers restart from silence), so a re-enabled
 * designer does not apply a contrast measured before the bypass.
 *
 * LATENCY IS ZERO, AND PUBLISHED: the gain is computed from the sample it multiplies
 * (omx_transient_latency()), and a host cannot tell "zero" from "never said".
 */
#ifndef OMX_TRANSIENT_INSTANCE_H
#define OMX_TRANSIENT_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_transient.h>
#include <omxdsp/omx_param.h>

/** Audio legs in and out: TWO, because the kernel folds both legs into one detector and applies
 * one gain to both. */
#define OMX_TRANSIENT_INSTANCE_CHANNELS 2

/** Frames of latency this plugin introduces: NONE — see the header note. */
#define OMX_TRANSIENT_INSTANCE_LATENCY_FRAMES 0.0f

/** One instance: the atom, the state and the last resolved controls. */
typedef struct {
  float sr;
  struct omx_transient atom;
  struct omx_transient_state state;
  /** The clamped controls the atom was last resolved from; `resolved` 0 until the first. */
  float attack_db, sustain_db, attack_ms, sustain_ms, output_db;
  int resolved;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the state. */
  int was_engaged;
  int ready;
} OmxTransientInstance;

/**
 * Bind an instance to its rate. Returns 1 when usable, 0 when not: the kernel's resolve is
 * defined at the declared rates only, so any other rate is refused, `ready` stays clear and
 * {@link omx_transient_instance_run} is then the identity: absence is a wire, not a crash.
 */
static inline int omx_transient_instance_init(OmxTransientInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!OMX_RATE_IS_DECLARED(sr)) return 0;
  s->sr = sr;
  s->atom.enabled = 0;
  omx_transient_state_init(&s->state);
  s->ready = 1;
  return 1;
}

/** The engage-edge clear: the three followers back to silence, the gain meter to 0 dB. One
 * memset over a few dozen bytes; RT-safe. */
#define OMX_CONTRACT_STAGE "transient/instance-clear"
static inline void omx_transient_instance_clear(OmxTransientInstance *s) {
  if (!s || !s->ready) return;
  omx_transient_state_init(&s->state);
  OMX_POST(s->state.gain_db == 0.0f && s->state.fast.stage[0] == 0.0f, "cleared-state-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle.
 *
 * `bypass` non-zero -> the atom is disabled and the kernel passes through. Every word is clamped
 * into its declared travel first (see the header note), so the kernel's `control-inside-travel`
 * PRE never sees a foreign value; the kernel's resolve runs only when a clamped word moved.
 */
#define OMX_CONTRACT_STAGE "transient/instance-resolve"
static inline void omx_transient_instance_resolve(OmxTransientInstance *s, int bypass, float attack_db,
                                                  float sustain_db, float attack_ms, float sustain_ms,
                                                  float output_db) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind was resolved from controls inside the
   * declared travel, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_transient_instance_clear(s);
  s->was_engaged = engaged;
  attack_db = omx_clamp_or(attack_db, OMX_TRANSIENT_ATTACK_DB_MIN, OMX_TRANSIENT_ATTACK_DB_MAX,
                           OMX_TRANSIENT_ATTACK_DB_DEFAULT);
  sustain_db = omx_clamp_or(sustain_db, OMX_TRANSIENT_SUSTAIN_DB_MIN, OMX_TRANSIENT_SUSTAIN_DB_MAX,
                            OMX_TRANSIENT_SUSTAIN_DB_DEFAULT);
  output_db = omx_clamp_or(output_db, OMX_TRANSIENT_OUTPUT_DB_MIN, OMX_TRANSIENT_OUTPUT_DB_MAX,
                           OMX_TRANSIENT_OUTPUT_DB_DEFAULT);
  attack_ms = omx_clampf(attack_ms, OMX_TRANSIENT_ATTACK_TIME_MS_MIN, OMX_TRANSIENT_ATTACK_TIME_MS_MAX);
  sustain_ms = omx_clampf(sustain_ms, OMX_TRANSIENT_SUSTAIN_TIME_MS_MIN, OMX_TRANSIENT_SUSTAIN_TIME_MS_MAX);
  const int moved = !s->resolved || s->attack_db != attack_db || s->sustain_db != sustain_db ||
                    s->attack_ms != attack_ms || s->sustain_ms != sustain_ms || s->output_db != output_db;
  if (moved) {
    omx_transient_resolve(&s->atom, 0, attack_db, sustain_db, attack_ms, sustain_ms, output_db, s->sr);
    s->attack_db = attack_db;
    s->sustain_db = sustain_db;
    s->attack_ms = attack_ms;
    s->sustain_ms = sustain_ms;
    s->output_db = output_db;
    s->resolved = 1;
  }
  s->atom.enabled = engaged;
  OMX_POST(s->atom.attack_db >= OMX_TRANSIENT_ATTACK_DB_MIN && s->atom.attack_db <= OMX_TRANSIENT_ATTACK_DB_MAX &&
               s->atom.sustain_db >= OMX_TRANSIENT_SUSTAIN_DB_MIN &&
               s->atom.sustain_db <= OMX_TRANSIENT_SUSTAIN_DB_MAX &&
               s->atom.output_db >= OMX_TRANSIENT_OUTPUT_DB_MIN && s->atom.output_db <= OMX_TRANSIENT_OUTPUT_DB_MAX,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel in
 * place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity byte for
 * byte.
 */
#define OMX_CONTRACT_STAGE "transient/instance-run"
static inline void omx_transient_instance_run(OmxTransientInstance *s, const float *in_l, const float *in_r,
                                              float *out_l, float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_transient_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_TRANSIENT_INSTANCE_H */
