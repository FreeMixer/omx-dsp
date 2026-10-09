/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_rotary_instance.h — the native rotary speaker as a plugin instance: the shell's core, with no
 * format in it. The same shape as omx_tremolo_instance.h (one C core, N shells): the core is
 * `omx_rotary.h`'s `omx_rotary_process`, THE SAME INLINE the console's rotary stage runs, and this
 * file adds no DSP to it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. RESOLVE. {@link omx_rotary_instance_resolve} takes the kernel's contract controls in their
 *      declared order and user units (four rotor rates in Hz, an accel scale, a balance and a mix
 *      in percent, the speed as its ROTARY_SPEEDS index), clamps each into its declared travel
 *      (ROTARY_*_RANGE, omx_contract_limits.h; a non-finite word reads as the declared default),
 *      and hands them to the kernel's own omx_rotary_resolve() with balance and mix over 100. That
 *      resolve reads the rotors' present rates (the accel/decel choice), so it runs every cycle,
 *      as the console's does.
 *   2. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass re-arms the state: the rings are silent and both rotors start from rest,
 * as a fresh instance does. LATENCY IS ZERO at every rate (the kernel's L1: the Doppler delay is
 * the effect). The rings are inline in the state; nothing is caller-owned.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxRotaryInstance}.
 */
#ifndef OMX_ROTARY_INSTANCE_H
#define OMX_ROTARY_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_rotary.h>
#include <omxdsp/omx_param.h>

#define OMX_ROTARY_INSTANCE_CHANNELS 2
#define OMX_ROTARY_INSTANCE_LATENCY_FRAMES 0.0f

/** Port defaults: the declaration's (ROTARY_*_RANGE.default). The speed set declares no default;
 * the stage comes up at `slow` (docs/design/specs/2026-09-26-rotary-speaker.md §3). */
#define OMX_ROTARY_INSTANCE_HORN_SLOW_HZ_DEFAULT ((float)OMX_ROTARY_HORN_SLOW_RANGE_DEFAULT)
#define OMX_ROTARY_INSTANCE_HORN_FAST_HZ_DEFAULT ((float)OMX_ROTARY_HORN_FAST_RANGE_DEFAULT)
#define OMX_ROTARY_INSTANCE_DRUM_SLOW_HZ_DEFAULT ((float)OMX_ROTARY_DRUM_SLOW_RANGE_DEFAULT)
#define OMX_ROTARY_INSTANCE_DRUM_FAST_HZ_DEFAULT ((float)OMX_ROTARY_DRUM_FAST_RANGE_DEFAULT)
#define OMX_ROTARY_INSTANCE_ACCEL_DEFAULT ((float)OMX_ROTARY_ACCEL_RANGE_DEFAULT)
#define OMX_ROTARY_INSTANCE_BALANCE_DEFAULT ((float)OMX_ROTARY_BALANCE_RANGE_DEFAULT)
#define OMX_ROTARY_INSTANCE_MIX_DEFAULT ((float)OMX_ROTARY_MIX_RANGE_DEFAULT)
#define OMX_ROTARY_INSTANCE_SPEED_DEFAULT OMX_ROTARY_SLOW

/** One instance: the atom the host's ports resolve to and the state (rotors and inline rings). */
typedef struct {
  float sr;
  struct omx_rotary atom;
  struct omx_rotary_state state;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can re-arm the state. */
  int was_engaged;
  int ready;
} OmxRotaryInstance;

/**
 * Bind an instance to its rate. Returns 1 when usable, 0 when not (a refused init leaves `ready`
 * clear and run() is the identity). The kernel's inline rings are sized for the declared rates, so
 * any other rate is refused.
 */
static inline int omx_rotary_instance_init(OmxRotaryInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_declared(sr)) return 0;
  if (omx_rotary_init(&s->state) != OMX_FDELAY_OK) return 0;
  s->sr = sr;
  s->atom.enabled = 0;
  s->ready = 1;
  return 1;
}

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the kernel's contract controls, in their declared order and
 * user units: `horn_slow_hz`, `horn_fast_hz`, `drum_slow_hz`, `drum_fast_hz` (Hz), `accel` (a
 * scale), `balance` and `mix` (percent), each clamped into its declared travel, a non-finite word
 * reading as the declared default; `speed` is the ROTARY_SPEEDS index, an `enum omx_rotary_speed`
 * member (any other value reads as `slow`).
 */
#define OMX_CONTRACT_STAGE "rotary/instance-resolve"
static inline void omx_rotary_instance_resolve(OmxRotaryInstance *s, int bypass, float horn_slow_hz,
                                               float horn_fast_hz, float drum_slow_hz, float drum_fast_hz,
                                               float accel, float balance, float mix, int speed) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_rotary_resolve and omx_rotary_process state, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) (void)omx_rotary_init(&s->state);
  s->was_engaged = engaged;
  horn_slow_hz = omx_clamp_or(horn_slow_hz, (float)OMX_ROTARY_HORN_SLOW_RANGE_MIN,
                              (float)OMX_ROTARY_HORN_SLOW_RANGE_MAX, OMX_ROTARY_INSTANCE_HORN_SLOW_HZ_DEFAULT);
  horn_fast_hz = omx_clamp_or(horn_fast_hz, (float)OMX_ROTARY_HORN_FAST_RANGE_MIN,
                              (float)OMX_ROTARY_HORN_FAST_RANGE_MAX, OMX_ROTARY_INSTANCE_HORN_FAST_HZ_DEFAULT);
  drum_slow_hz = omx_clamp_or(drum_slow_hz, (float)OMX_ROTARY_DRUM_SLOW_RANGE_MIN,
                              (float)OMX_ROTARY_DRUM_SLOW_RANGE_MAX, OMX_ROTARY_INSTANCE_DRUM_SLOW_HZ_DEFAULT);
  drum_fast_hz = omx_clamp_or(drum_fast_hz, (float)OMX_ROTARY_DRUM_FAST_RANGE_MIN,
                              (float)OMX_ROTARY_DRUM_FAST_RANGE_MAX, OMX_ROTARY_INSTANCE_DRUM_FAST_HZ_DEFAULT);
  accel = omx_clamp_or(accel, (float)OMX_ROTARY_ACCEL_RANGE_MIN, (float)OMX_ROTARY_ACCEL_RANGE_MAX,
                       OMX_ROTARY_INSTANCE_ACCEL_DEFAULT);
  balance = omx_clamp_or(balance, (float)OMX_ROTARY_BALANCE_RANGE_MIN, (float)OMX_ROTARY_BALANCE_RANGE_MAX,
                         OMX_ROTARY_INSTANCE_BALANCE_DEFAULT);
  mix = omx_clamp_or(mix, (float)OMX_ROTARY_MIX_RANGE_MIN, (float)OMX_ROTARY_MIX_RANGE_MAX,
                     OMX_ROTARY_INSTANCE_MIX_DEFAULT);
  if (speed < OMX_ROTARY_STOP || speed > OMX_ROTARY_FAST) speed = OMX_ROTARY_INSTANCE_SPEED_DEFAULT;
  omx_rotary_resolve(&s->atom, &s->state, engaged, speed, horn_slow_hz, horn_fast_hz, drum_slow_hz,
                     drum_fast_hz, accel, 0.01f * balance, 0.01f * mix, s->sr);
  OMX_POST(s->atom.mix >= 0.0f && s->atom.mix <= 1.0f && s->atom.horn.target_hz >= 0.0f &&
               s->atom.drum.target_hz >= 0.0f,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "rotary/instance-run"
static inline void omx_rotary_instance_run(OmxRotaryInstance *s, const float *in_l, const float *in_r,
                                           float *out_l, float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_rotary_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces: the kernel's, zero at every rate. */
static inline uint32_t omx_rotary_instance_latency(const OmxRotaryInstance *s) {
  (void)s;
  return omx_rotary_latency();
}

#endif /* OMX_ROTARY_INSTANCE_H */
