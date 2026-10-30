/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_reverb_instance.h — the native reverb as a plugin instance: the shell's core, with no
 * format in it. The same shape as omx-plugins' omx_delay_instance.h (one C core, N shells): the core is
 * `omx_reverb.h`'s `omx_reverb_process`, THE SAME INLINE the console's reverb stage runs, and
 * this file adds no DSP to it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. POOL. `struct omx_reverb_state` binds a pool; the instance OWNS it, inline
 *      ({@link OMX_REVERB_POOL_FLOATS}, which fits every declared rate), and
 *      {@link omx_reverb_instance_init} lays the five configurations out over it at the instance
 *      rate, REFUSING a rate the console does not declare. Nothing is allocated, at init or after.
 *   2. RESOLVE. {@link omx_reverb_instance_resolve} takes the kernel's thirteen contract controls
 *      in their declared order and the row's own units (resolve_fx_reverb, mixer_rt.c, loads them
 *      unconverted) into the atom, every travel clamped through omx_param into its declared range
 *      (REVERB_*_RANGE, omxdsp_definitions.h) and a non-finite word reading as the declared
 *      default — a foreign host's port is not the console's codec. The algorithm is the
 *      REVERB_ALGORITHMS index; any other value reads as ROOM.
 *   3. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass LAYS THE POOL OUT AGAIN, which hands every line back silent
 * (omx_pool_take's own clear), so a re-enabled reverb does not replay a tail buffered before the
 * bypass: a bounded pass over the instance's own pool, no allocation. LATENCY IS ZERO: the dry
 * path is frame-aligned with the input; the pre-delay is the effect, not a latency.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxReverbInstance}. The state
 * points into the instance's own pool, so an instance is initialised where it lives and is never
 * copied after init.
 */
#ifndef OMX_REVERB_INSTANCE_H
#define OMX_REVERB_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_deesser_instance.h>
#include <omxdsp/fx/omx_reverb.h>
#include <omxdsp/omx_param.h>

#define OMX_REVERB_INSTANCE_CHANNELS 2
#define OMX_REVERB_INSTANCE_LATENCY_FRAMES 0.0f

/** Port defaults: the declaration's (REVERB_*_RANGE.default); the algorithm comes up as ROOM. */
#define OMX_REVERB_INSTANCE_PLATE_MOD_DEPTH_DEFAULT ((float)OMX_REVERB_PLATE_MOD_DEPTH_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_MIX_DEFAULT ((float)OMX_REVERB_MIX_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_SIZE_DEFAULT ((float)OMX_REVERB_SIZE_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_DAMPING_DEFAULT ((float)OMX_REVERB_DAMPING_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_WIDTH_DEFAULT ((float)OMX_REVERB_WIDTH_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_PREDELAY_DEFAULT ((float)OMX_REVERB_PREDELAY_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_LOWCUT_DEFAULT ((float)OMX_REVERB_LOWCUT_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_HIGHCUT_DEFAULT ((float)OMX_REVERB_HIGHCUT_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_REVERSE_DEFAULT ((float)OMX_REVERB_REVERSE_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_HOLD_DEFAULT ((float)OMX_REVERB_HOLD_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_RELEASE_DEFAULT ((float)OMX_REVERB_RELEASE_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_GATE_THRESHOLD_DEFAULT ((float)OMX_REVERB_GATE_THRESHOLD_RANGE_DEFAULT)
#define OMX_REVERB_INSTANCE_ALGORITHM_DEFAULT OMX_REVERB_ROOM

/** One instance. It owns its pool; the kernel's lines are laid out over it at init. */
/** The early reflections' table, REVERB_EARLY_TAP_MS and REVERB_EARLY_TAP_GAINS. */
static const float OMX_REVERB_INSTANCE_TAP_MS[OMX_REVERB_EARLY_TAPS] = OMX_REVERB_EARLY_TAP_MS_INIT;
static const float OMX_REVERB_INSTANCE_TAP_GAINS[OMX_REVERB_EARLY_TAPS] = OMX_REVERB_EARLY_TAP_GAINS_INIT;

/** The early ring, floats: the pre-delay's ceiling, which holds the longest tap at REVERB_EARLY_SIZE_MAX
 * at the highest rate with the line's kernel reach (test/fx/reverb_return.test.c holds the two to that). */
#define OMX_REVERB_INSTANCE_EARLY_CAP OMX_REVERB_PREDELAY_CAP

/** The early line's Lagrange order. */
#define OMX_REVERB_INSTANCE_EARLY_ORDER 3

typedef struct {
  float sr;
  struct omx_reverb atom;
  struct omx_reverb_state state;
  struct omx_reverb_return ret;         /**< the early taps, the tail's level, the duck, the de-esser */
  struct omx_reverb_return_state rstate; /**< the early line, the duck's and the de-esser's state */
  OmxDeesserInstance deess;              /**< the wet de-esser's design (its atom; its state is rstate's) */
  float early_ring[OMX_REVERB_INSTANCE_EARLY_CAP];
  float pool[OMX_REVERB_POOL_FLOATS];
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the lines. */
  int was_engaged;
  int ready;
} OmxReverbInstance;

/**
 * Bind an instance to its rate and lay its own pool out. Returns 1 when usable, 0 when not (a
 * refused init leaves `ready` clear and run() is the identity): the rate must be a declared one.
 * Allocates nothing.
 */
static inline int omx_reverb_instance_init(OmxReverbInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr)) return 0;
  omx_reverb_state_layout(&s->state, s->pool, OMX_REVERB_POOL_FLOATS, sr);
  if (s->state._exhausted || s->state._pool == NULL) return 0;
  s->sr = sr;
  s->atom.width = OMX_REVERB_INSTANCE_WIDTH_DEFAULT; /* an atom inside the kernel's PREs before the first resolve */
  s->atom.size = OMX_REVERB_INSTANCE_SIZE_DEFAULT;
  s->atom.damping = OMX_REVERB_INSTANCE_DAMPING_DEFAULT;
  s->atom.mix = OMX_REVERB_INSTANCE_MIX_DEFAULT;
  s->ret.tail_lin = 1.0f;
  if (omx_fdelay_init(&s->rstate.line, s->early_ring, OMX_REVERB_INSTANCE_EARLY_CAP, OMX_REVERB_INSTANCE_EARLY_ORDER) !=
      OMX_FDELAY_OK)
    return 0;
  omx_deesser_instance_init(&s->deess, sr);
  s->ready = 1;
  return 1;
}

/** Lay the pool out again — every line handed back silent, every cursor and filter at rest:
 * the re-enable recipe. Bounded work over the instance's own pool, no allocation. */
#define OMX_CONTRACT_STAGE "reverb/instance-clear"
static inline void omx_reverb_instance_clear(OmxReverbInstance *s) {
  if (!s || !s->ready) return;
  omx_reverb_state_layout(&s->state, s->pool, OMX_REVERB_POOL_FLOATS, s->sr);
  memset(s->early_ring, 0, sizeof s->early_ring);
  omx_fdelay_init(&s->rstate.line, s->early_ring, OMX_REVERB_INSTANCE_EARLY_CAP, OMX_REVERB_INSTANCE_EARLY_ORDER);
  s->rstate.duck.level = 0.0f;
  omx_deess_state_init(&s->rstate.deess);
  OMX_POST(!s->state._exhausted && s->state.sr == s->sr && s->state.pre_pos == 0u &&
               s->state.gate_env == 0.0f && s->pool[0] == 0.0f,
           "cleared-pool-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the kernel's contract controls, in their declared order and
 * the row's units: `plate_mod_depth` (% of Dattorro's excursion), `mix`, `size`, `damping` and
 * `width` (0..1), `predelay` (ms), `lowcut` and `highcut` (Hz, a zero lowcut is none), `reverse`,
 * `hold` and `release` (ms), `gate_threshold` (dBFS), each clamped into its declared travel, a
 * non-finite word reading as the declared default; `algorithm` is the REVERB_ALGORITHMS index, an
 * `enum omx_reverb_algo` member (any other value reads as ROOM). Then the return: `early_db` and
 * `tail_db` (dB, each travel's floor off), `early_size_pct` (percent), the duck's `duck_db`,
 * `duck_threshold_db` and `duck_release_ms`, `wet_deess` (REVERB_WET_DEESSES: off, on) and the wet
 * de-esser's `deess_freq_hz` and `deess_threshold_db`, clamped likewise.
 */
#define OMX_CONTRACT_STAGE "reverb/instance-resolve"
static inline void omx_reverb_instance_resolve(OmxReverbInstance *s, int bypass, float plate_mod_depth,
                                               float mix, float size, float damping, float width,
                                               float predelay, float lowcut, float highcut, float reverse,
                                               float hold, float release, float gate_threshold,
                                               int algorithm, float early_db, float tail_db, float early_size_pct,
                                               float duck_db, float duck_threshold_db, float duck_release_ms,
                                               int wet_deess, float deess_freq_hz, float deess_threshold_db) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_reverb_process states, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_reverb_instance_clear(s);
  s->was_engaged = engaged;
  struct omx_reverb *o = &s->atom;
  o->enabled = engaged;
  o->algorithm = (algorithm < OMX_REVERB_ROOM || algorithm > OMX_REVERB_GATED)
                     ? OMX_REVERB_INSTANCE_ALGORITHM_DEFAULT
                     : algorithm;
  o->size = omx_clamp_or(size, (float)OMX_REVERB_SIZE_RANGE_MIN, (float)OMX_REVERB_SIZE_RANGE_MAX,
                         OMX_REVERB_INSTANCE_SIZE_DEFAULT);
  o->damping = omx_clamp_or(damping, (float)OMX_REVERB_DAMPING_RANGE_MIN,
                            (float)OMX_REVERB_DAMPING_RANGE_MAX, OMX_REVERB_INSTANCE_DAMPING_DEFAULT);
  o->predelay_ms = omx_clamp_or(predelay, (float)OMX_REVERB_PREDELAY_RANGE_MIN,
                                (float)OMX_REVERB_PREDELAY_RANGE_MAX, OMX_REVERB_INSTANCE_PREDELAY_DEFAULT);
  o->width = omx_clamp_or(width, (float)OMX_REVERB_WIDTH_RANGE_MIN, (float)OMX_REVERB_WIDTH_RANGE_MAX,
                          OMX_REVERB_INSTANCE_WIDTH_DEFAULT);
  o->mix = omx_clamp_or(mix, (float)OMX_REVERB_MIX_RANGE_MIN, (float)OMX_REVERB_MIX_RANGE_MAX,
                        OMX_REVERB_INSTANCE_MIX_DEFAULT);
  o->lowcut = omx_clamp_or(lowcut, (float)OMX_REVERB_LOWCUT_RANGE_MIN, (float)OMX_REVERB_LOWCUT_RANGE_MAX,
                           OMX_REVERB_INSTANCE_LOWCUT_DEFAULT);
  o->highcut = omx_clamp_or(highcut, (float)OMX_REVERB_HIGHCUT_RANGE_MIN,
                            (float)OMX_REVERB_HIGHCUT_RANGE_MAX, OMX_REVERB_INSTANCE_HIGHCUT_DEFAULT);
  o->reverse_ms = omx_clamp_or(reverse, (float)OMX_REVERB_REVERSE_RANGE_MIN,
                               (float)OMX_REVERB_REVERSE_RANGE_MAX, OMX_REVERB_INSTANCE_REVERSE_DEFAULT);
  o->hold_ms = omx_clamp_or(hold, (float)OMX_REVERB_HOLD_RANGE_MIN, (float)OMX_REVERB_HOLD_RANGE_MAX,
                            OMX_REVERB_INSTANCE_HOLD_DEFAULT);
  o->release_ms = omx_clamp_or(release, (float)OMX_REVERB_RELEASE_RANGE_MIN,
                               (float)OMX_REVERB_RELEASE_RANGE_MAX, OMX_REVERB_INSTANCE_RELEASE_DEFAULT);
  o->gate_threshold_db = omx_clamp_or(gate_threshold, (float)OMX_REVERB_GATE_THRESHOLD_RANGE_MIN,
                                      (float)OMX_REVERB_GATE_THRESHOLD_RANGE_MAX,
                                      OMX_REVERB_INSTANCE_GATE_THRESHOLD_DEFAULT);
  o->plate_mod_depth = omx_clamp_or(plate_mod_depth, (float)OMX_REVERB_PLATE_MOD_DEPTH_RANGE_MIN,
                                    (float)OMX_REVERB_PLATE_MOD_DEPTH_RANGE_MAX,
                                    OMX_REVERB_INSTANCE_PLATE_MOD_DEPTH_DEFAULT);
  /* The return. A level at its travel's floor is off: exactly 0; 0 dB is exactly 1. */
  const float early_level = omx_clamp_or(early_db, (float)OMX_REVERB_EARLY_DB_RANGE_MIN, (float)OMX_REVERB_EARLY_DB_RANGE_MAX,
                                      (float)OMX_REVERB_EARLY_DB_RANGE_DEFAULT);
  const float tail_level = omx_clamp_or(tail_db, (float)OMX_REVERB_TAIL_DB_RANGE_MIN, (float)OMX_REVERB_TAIL_DB_RANGE_MAX,
                                     (float)OMX_REVERB_TAIL_DB_RANGE_DEFAULT);
  const float early_size = omx_clamp_or(early_size_pct, (float)OMX_REVERB_EARLY_SIZE_PCT_RANGE_MIN,
                                        (float)OMX_REVERB_EARLY_SIZE_PCT_RANGE_MAX,
                                        (float)OMX_REVERB_EARLY_SIZE_PCT_RANGE_DEFAULT);
  const float early_scale = (float)OMX_REVERB_EARLY_SIZE_MIN +
                            ((float)OMX_REVERB_EARLY_SIZE_MAX - (float)OMX_REVERB_EARLY_SIZE_MIN) * early_size * 0.01f;
  const float duck_depth = omx_clamp_or(duck_db, (float)OMX_REVERB_DUCK_DB_RANGE_MIN, (float)OMX_REVERB_DUCK_DB_RANGE_MAX,
                                        (float)OMX_REVERB_DUCK_DB_RANGE_DEFAULT);
  const float duck_thr = omx_clamp_or(duck_threshold_db, (float)OMX_REVERB_DUCK_THRESHOLD_DB_RANGE_MIN,
                                      (float)OMX_REVERB_DUCK_THRESHOLD_DB_RANGE_MAX,
                                      (float)OMX_REVERB_DUCK_THRESHOLD_DB_RANGE_DEFAULT);
  const float duck_rel = omx_clamp_or(duck_release_ms, (float)OMX_REVERB_DUCK_RELEASE_MS_RANGE_MIN,
                                      (float)OMX_REVERB_DUCK_RELEASE_MS_RANGE_MAX,
                                      (float)OMX_REVERB_DUCK_RELEASE_MS_RANGE_DEFAULT);
  const int deess_on = wet_deess == OMX_REVERB_WET_DEESSES_ON ? 1 : 0;
  const float deess_freq = omx_clamp_or(deess_freq_hz, (float)OMX_REVERB_DEESS_FREQ_HZ_RANGE_MIN,
                                        (float)OMX_REVERB_DEESS_FREQ_HZ_RANGE_MAX,
                                        (float)OMX_REVERB_DEESS_FREQ_HZ_RANGE_DEFAULT);
  const float deess_thr = omx_clamp_or(deess_threshold_db, (float)OMX_REVERB_DEESS_THRESHOLD_DB_RANGE_MIN,
                                       (float)OMX_REVERB_DEESS_THRESHOLD_DB_RANGE_MAX,
                                       (float)OMX_REVERB_DEESS_THRESHOLD_DB_RANGE_DEFAULT);
  struct omx_reverb_return *x = &s->ret;
  x->early_lin = early_level <= (float)OMX_REVERB_EARLY_DB_RANGE_MIN ? 0.0f
                 : early_level == 0.0f                           ? 1.0f
                                                                 : omx_db_to_lin(early_level);
  x->tail_lin = tail_level <= (float)OMX_REVERB_TAIL_DB_RANGE_MIN ? 0.0f
                : tail_level == 0.0f                          ? 1.0f
                                                              : omx_db_to_lin(tail_level);
  for (uint32_t t = 0; t < OMX_REVERB_EARLY_TAPS; t++) {
    const float d = OMX_REVERB_INSTANCE_TAP_MS[t] * early_scale * 0.001f * s->sr;
    const float lo = omx_fdelay_min_delay(OMX_REVERB_INSTANCE_EARLY_ORDER);
    const float hi = omx_fdelay_max_delay(OMX_REVERB_INSTANCE_EARLY_CAP, OMX_REVERB_INSTANCE_EARLY_ORDER);
    x->tap_d[t] = d < lo ? lo : d > hi ? hi : d;
    x->tap_g[t] = OMX_REVERB_INSTANCE_TAP_GAINS[t];
  }
  x->duck.thresh_db = duck_thr;
  x->duck.depth_db = duck_depth;
  x->duck.attack = omx_pole_from_time_ms((float)OMX_FX_DUCK_ATTACK_MS, s->sr);
  x->duck.release = omx_pole_from_time_ms(duck_rel, s->sr);
  omx_deesser_instance_resolve(&s->deess, !deess_on, deess_freq, OMX_DEESSER_INSTANCE_WIDTH_DEFAULT, deess_thr,
                               OMX_DEESSER_INSTANCE_RATIO_DEFAULT, OMX_DEESSER_INSTANCE_RANGE_DEFAULT,
                               OMX_DEESSER_INSTANCE_ATTACK_DEFAULT, OMX_DEESSER_INSTANCE_RELEASE_DEFAULT,
                               OMX_DEESSER_INSTANCE_MODE_DEFAULT);
  x->deess = deess_on ? &s->deess.atom : NULL;
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
  omx_reverb_process_return(out_l, out_r, n, &s->atom, &s->ret, &s->rstate, &s->state, s->sr);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_REVERB_INSTANCE_H */
