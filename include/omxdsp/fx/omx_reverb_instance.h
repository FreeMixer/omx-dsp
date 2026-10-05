/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_reverb_instance.h — the native reverb as a plugin instance: the shell's core, with no
 * format in it. The same shape as omx_delay_instance.h (one C core, N shells): the core is
 * `omx_reverb.h`'s `omx_reverb_process`, THE SAME INLINE the console's reverb stage runs, and
 * this file adds no DSP to it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. POOL. `struct omx_reverb_state` binds a caller-owned pool; the shell's caller hands it in
 *      through {@link omx_reverb_instance_init}, which lays the five configurations out at the
 *      instance rate and REFUSES a rate the console does not declare or a pool the layout does
 *      not fit ({@link OMX_REVERB_POOL_FLOATS} fits every declared rate).
 *   2. RESOLVE. {@link omx_reverb_instance_resolve} carries the thirteen ports, in the row's own
 *      units (resolve_fx_reverb, mixer_rt.c, loads them unconverted), into the atom with every
 *      one clamped through omx_param into its declared travel (REVERB_*_RANGE,
 *      omx_contract_limits.h) and a non-finite word reading as the declared default — a foreign
 *      host's port is not the console's codec. The algorithm is an integer port (omx_port_int).
 *   3. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass LAYS THE POOL OUT AGAIN, which hands every line back silent
 * (omx_pool_take's own clear), so a re-enabled reverb does not replay a tail buffered before the
 * bypass: a bounded pass over the caller's fixed pool, no allocation. LATENCY IS ZERO: the dry
 * path is frame-aligned with the input; the pre-delay is the effect, not a latency.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxReverbInstance}.
 */
#ifndef OMX_REVERB_INSTANCE_H
#define OMX_REVERB_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_reverb.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_port_int.h>

#define OMX_REVERB_INSTANCE_CHANNELS 2
#define OMX_REVERB_INSTANCE_LATENCY_FRAMES 0.0f

/** The thirteen control ports, in the row's units, in port order. */
struct omx_reverb_instance_ports {
  float algorithm;         /* enum omx_reverb_algo, OMX_REVERB_ROOM..OMX_REVERB_GATED */
  float size;              /* 0..1 */
  float damping;           /* 0..1 */
  float predelay_ms;       /* 0..100 */
  float width;             /* 0..1 */
  float mix;               /* 0..1 */
  float lowcut;            /* Hz, 0 = none */
  float highcut;           /* Hz */
  float reverse_ms;        /* 50..500 */
  float hold_ms;           /* 10..2000 */
  float release_ms;        /* 1..500 */
  float gate_threshold_db; /* -80..0 */
  float plate_mod_depth;   /* % of Dattorro's excursion, 0..400 */
};

/** The declaration's defaults (REVERB_*_RANGE.default); the algorithm comes up as ROOM. */
#define OMX_REVERB_INSTANCE_PORT_DEFAULTS                                                         \
  { (float)OMX_REVERB_ROOM, (float)OMX_REVERB_SIZE_RANGE_DEFAULT,                                 \
    (float)OMX_REVERB_DAMPING_RANGE_DEFAULT, (float)OMX_REVERB_PREDELAY_RANGE_DEFAULT,            \
    (float)OMX_REVERB_WIDTH_RANGE_DEFAULT, (float)OMX_REVERB_MIX_RANGE_DEFAULT,                   \
    (float)OMX_REVERB_LOWCUT_RANGE_DEFAULT, (float)OMX_REVERB_HIGHCUT_RANGE_DEFAULT,              \
    (float)OMX_REVERB_REVERSE_RANGE_DEFAULT, (float)OMX_REVERB_HOLD_RANGE_DEFAULT,                \
    (float)OMX_REVERB_RELEASE_RANGE_DEFAULT, (float)OMX_REVERB_GATE_THRESHOLD_RANGE_DEFAULT,      \
    (float)OMX_REVERB_PLATE_MOD_DEPTH_RANGE_DEFAULT }

/** One instance. The pool is NOT owned here; {@link omx_reverb_instance_init} is given it. */
typedef struct {
  float sr;
  struct omx_reverb atom;
  struct omx_reverb_state state;
  float *pool;
  uint32_t pool_len;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the lines. */
  int was_engaged;
  int ready;
} OmxReverbInstance;

/**
 * Bind an instance to its rate and the caller's pool of `pool_len` floats, and lay it out.
 * Returns 1 when usable, 0 when not (a refused init leaves `ready` clear and run() is the
 * identity): the rate must be a declared one and the layout must fit the pool.
 */
static inline int omx_reverb_instance_init(OmxReverbInstance *s, float sr, float *pool,
                                           uint32_t pool_len) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_declared(sr) || !pool || pool_len == 0u) return 0;
  omx_reverb_state_layout(&s->state, pool, pool_len, sr);
  if (s->state._exhausted || s->state._pool == NULL) return 0;
  s->sr = sr;
  s->pool = pool;
  s->pool_len = pool_len;
  const struct omx_reverb_instance_ports d = OMX_REVERB_INSTANCE_PORT_DEFAULTS;
  s->atom.width = d.width; /* an atom inside the kernel's PREs before the first resolve */
  s->atom.size = d.size;
  s->atom.damping = d.damping;
  s->atom.mix = d.mix;
  s->ready = 1;
  return 1;
}

/** Lay the pool out again — every line handed back silent, every cursor and filter at rest:
 * the re-enable recipe. Bounded work over the caller's fixed pool, no allocation. */
#define OMX_CONTRACT_STAGE "reverb/instance-clear"
static inline void omx_reverb_instance_clear(OmxReverbInstance *s) {
  if (!s || !s->ready) return;
  omx_reverb_state_layout(&s->state, s->pool, s->pool_len, s->sr);
  OMX_POST(!s->state._exhausted && s->state.sr == s->sr && s->state.pre_pos == 0u &&
               s->state.gate_env == 0.0f && s->pool[0] == 0.0f,
           "cleared-pool-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. Every port is clamped into its declared travel; a non-finite word reads as
 * the declared default.
 */
#define OMX_CONTRACT_STAGE "reverb/instance-resolve"
static inline void omx_reverb_instance_resolve(OmxReverbInstance *s, int bypass,
                                               const struct omx_reverb_instance_ports *p) {
  if (!s || !s->ready || !p) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_reverb_process states, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_reverb_instance_clear(s);
  s->was_engaged = engaged;
  const struct omx_reverb_instance_ports d = OMX_REVERB_INSTANCE_PORT_DEFAULTS;
  struct omx_reverb *o = &s->atom;
  o->enabled = engaged;
  o->algorithm = omx_port_int(p->algorithm, OMX_REVERB_ROOM, OMX_REVERB_GATED, OMX_REVERB_ROOM);
  o->size = omx_clamp_or(p->size, (float)OMX_REVERB_SIZE_RANGE_MIN, (float)OMX_REVERB_SIZE_RANGE_MAX,
                         d.size);
  o->damping = omx_clamp_or(p->damping, (float)OMX_REVERB_DAMPING_RANGE_MIN,
                            (float)OMX_REVERB_DAMPING_RANGE_MAX, d.damping);
  o->predelay_ms = omx_clamp_or(p->predelay_ms, (float)OMX_REVERB_PREDELAY_RANGE_MIN,
                                (float)OMX_REVERB_PREDELAY_RANGE_MAX, d.predelay_ms);
  o->width = omx_clamp_or(p->width, (float)OMX_REVERB_WIDTH_RANGE_MIN,
                          (float)OMX_REVERB_WIDTH_RANGE_MAX, d.width);
  o->mix = omx_clamp_or(p->mix, (float)OMX_REVERB_MIX_RANGE_MIN, (float)OMX_REVERB_MIX_RANGE_MAX,
                        d.mix);
  o->lowcut = omx_clamp_or(p->lowcut, (float)OMX_REVERB_LOWCUT_RANGE_MIN,
                           (float)OMX_REVERB_LOWCUT_RANGE_MAX, d.lowcut);
  o->highcut = omx_clamp_or(p->highcut, (float)OMX_REVERB_HIGHCUT_RANGE_MIN,
                            (float)OMX_REVERB_HIGHCUT_RANGE_MAX, d.highcut);
  o->reverse_ms = omx_clamp_or(p->reverse_ms, (float)OMX_REVERB_REVERSE_RANGE_MIN,
                               (float)OMX_REVERB_REVERSE_RANGE_MAX, d.reverse_ms);
  o->hold_ms = omx_clamp_or(p->hold_ms, (float)OMX_REVERB_HOLD_RANGE_MIN,
                            (float)OMX_REVERB_HOLD_RANGE_MAX, d.hold_ms);
  o->release_ms = omx_clamp_or(p->release_ms, (float)OMX_REVERB_RELEASE_RANGE_MIN,
                               (float)OMX_REVERB_RELEASE_RANGE_MAX, d.release_ms);
  o->gate_threshold_db = omx_clamp_or(p->gate_threshold_db, (float)OMX_REVERB_GATE_THRESHOLD_RANGE_MIN,
                                      (float)OMX_REVERB_GATE_THRESHOLD_RANGE_MAX, d.gate_threshold_db);
  o->plate_mod_depth = omx_clamp_or(p->plate_mod_depth, (float)OMX_REVERB_PLATE_MOD_DEPTH_RANGE_MIN,
                                    (float)OMX_REVERB_PLATE_MOD_DEPTH_RANGE_MAX, d.plate_mod_depth);
  OMX_POST(o->algorithm >= OMX_REVERB_ROOM && o->algorithm <= OMX_REVERB_GATED &&
               o->size >= 0.0f && o->size <= 1.0f && o->damping >= 0.0f && o->damping <= 1.0f &&
               o->width >= 0.0f && o->width <= 1.0f && o->mix >= 0.0f && o->mix <= 1.0f &&
               o->predelay_ms >= 0.0f && o->predelay_ms <= (float)OMX_REVERB_PREDELAY_MAX_MS &&
               o->reverse_ms >= (float)OMX_REVERB_REVERSE_MIN_MS &&
               o->reverse_ms <= (float)OMX_REVERB_REVERSE_MAX_MS,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out at the instance rate. `in_*` and `out_*` may alias. Not ready, or bypassed, is
 * the identity.
 */
#define OMX_CONTRACT_STAGE "reverb/instance-run"
static inline void omx_reverb_instance_run(OmxReverbInstance *s, const float *in_l,
                                           const float *in_r, float *out_l, float *out_r,
                                           uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memcpy(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memcpy(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_reverb_process(out_l, out_r, n, &s->atom, &s->state, s->sr);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_REVERB_INSTANCE_H */
