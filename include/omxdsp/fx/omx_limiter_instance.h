/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_limiter_instance.h — the precision limiter as a plugin instance: the shell's core, with no
 * format in it. The same shape as omx_chorus_instance.h (one C core, N shells): the core is
 * `omx_limiter.h`'s `omx_limiter_process`, THE SAME INLINE the console's limiter stage runs, and
 * this file adds no DSP to it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. MEMORY. `struct omx_limiter_state` reads caller-owned rings; the shell's caller hands them
 *      in through {@link omx_limiter_instance_init}, sized for the longest declared look-ahead at
 *      the instance rate (omx_limiter_instance_cap_for). A shorter one is refused there.
 *   2. RESOLVE. {@link omx_limiter_instance_resolve} takes the kernel's contract controls in their
 *      declared order and user units (dBFS, ms, ms) and clamps each into its declared travel
 *      (OMX_LIMITER_*, omx_contract_limits.h; a non-finite word reads as the declared default).
 *      The look-ahead is the state's geometry, not an atom word: when it moves by a frame the
 *      state is re-armed (omx_limiter_init: a bounded pass over the caller's rings, no
 *      allocation), the gain restarts at unity and the latency changes.
 *   3. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * LATENCY IS PUBLISHED: {@link omx_limiter_instance_latency} answers the armed state's `T = U + D`
 * frames while engaged and 0 while bypassed (the kernel's disabled path delays nothing), so a host
 * reads it after every resolve. Re-engaging from bypass re-arms the state, so a re-enabled limiter
 * does not release a gain or replay audio held before the bypass.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxLimiterInstance}.
 */
#ifndef OMX_LIMITER_INSTANCE_H
#define OMX_LIMITER_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_limiter.h>
#include <omxdsp/omx_param.h>

#define OMX_LIMITER_INSTANCE_CHANNELS 2

/** One instance. The rings are NOT owned here; {@link omx_limiter_instance_init} is given them. */
typedef struct {
  float sr;
  struct omx_limiter atom;
  struct omx_limiter_state state;
  float *mem;
  uint32_t *idx;
  uint32_t cap;
  /** The look-ahead, ms, the state is armed at. */
  float lookahead_ms;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can re-arm the state. */
  int was_engaged;
  int ready;
} OmxLimiterInstance;

/** The ring length every declared look-ahead fits at `sr`: allocate omx_limiter_mem_floats() of
 * it in floats and as many uint32_t indices. */
static inline uint32_t omx_limiter_instance_cap_for(float sr) { return omx_limiter_cap(sr); }

/**
 * Bind an instance to its rate and the caller's memory: `mem` holds omx_limiter_mem_floats(cap)
 * floats, `idx` holds `cap` indices. Returns 1 when usable, 0 when not (a refused init leaves
 * `ready` clear and run() is the identity). The kernel is defined at the declared rates only, so
 * any other rate is refused. The state is armed at the declared default look-ahead.
 */
static inline int omx_limiter_instance_init(OmxLimiterInstance *s, float sr, float *mem, uint32_t *idx,
                                            uint32_t cap) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_declared(sr) || !mem || !idx || cap < omx_limiter_instance_cap_for(sr)) return 0;
  s->sr = sr;
  s->mem = mem;
  s->idx = idx;
  s->cap = cap;
  s->lookahead_ms = OMX_LIMITER_LOOKAHEAD_MS_DEFAULT;
  omx_limiter_init(&s->state, s->lookahead_ms, sr, mem, idx, cap);
  s->atom.enabled = 0;
  s->atom.ceiling_db = OMX_LIMITER_CEILING_DB_DEFAULT;
  s->atom.release_ms = OMX_LIMITER_RELEASE_MS_DEFAULT;
  s->ready = 1;
  return 1;
}

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the kernel's contract controls, in their declared order and
 * user units: `ceiling_db` (dBFS, true peak), `lookahead_ms` and `release_ms` (ms), each clamped
 * into its declared travel, a non-finite word reading as the declared default. A look-ahead that
 * lands on another frame count, or a bypass->engaged edge, re-arms the state.
 */
#define OMX_CONTRACT_STAGE "limiter/instance-resolve"
static inline void omx_limiter_instance_resolve(OmxLimiterInstance *s, int bypass, float ceiling_db,
                                                float lookahead_ms, float release_ms) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_limiter_process states, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  lookahead_ms = omx_clamp_or(lookahead_ms, OMX_LIMITER_LOOKAHEAD_MS_MIN, OMX_LIMITER_LOOKAHEAD_MS_MAX,
                              OMX_LIMITER_LOOKAHEAD_MS_DEFAULT);
  const int moved = omx_limiter_lookahead_frames(lookahead_ms, s->sr) != s->state.d;
  if ((engaged && !s->was_engaged) || moved)
    omx_limiter_init(&s->state, lookahead_ms, s->sr, s->mem, s->idx, s->cap);
  s->lookahead_ms = lookahead_ms;
  s->was_engaged = engaged;
  struct omx_limiter *o = &s->atom;
  o->enabled = engaged;
  o->ceiling_db = omx_clamp_or(ceiling_db, OMX_LIMITER_CEILING_DB_MIN, OMX_LIMITER_CEILING_DB_MAX,
                               OMX_LIMITER_CEILING_DB_DEFAULT);
  o->release_ms = omx_clamp_or(release_ms, OMX_LIMITER_RELEASE_MS_MIN, OMX_LIMITER_RELEASE_MS_MAX,
                               OMX_LIMITER_RELEASE_MS_DEFAULT);
  OMX_POST(o->ceiling_db >= OMX_LIMITER_CEILING_DB_MIN && o->ceiling_db <= OMX_LIMITER_CEILING_DB_MAX &&
               o->release_ms >= OMX_LIMITER_RELEASE_MS_MIN && o->release_ms <= OMX_LIMITER_RELEASE_MS_MAX &&
               s->cap >= s->state.t + 2u,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "limiter/instance-run"
static inline void omx_limiter_instance_run(OmxLimiterInstance *s, const float *in_l, const float *in_r,
                                            float *out_l, float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_limiter_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces now: the armed state's `T` while engaged, 0
 * while bypassed or not ready. */
static inline uint32_t omx_limiter_instance_latency(const OmxLimiterInstance *s) {
  if (!s || !s->ready || !s->atom.enabled) return 0u;
  return omx_limiter_latency(&s->state);
}

#endif /* OMX_LIMITER_INSTANCE_H */
