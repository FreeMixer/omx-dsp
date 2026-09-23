/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * mix_lv2_delay.h — the native FX delay AS AN LV2 PLUGIN: the shell's core, with no LV2 in it.
 *
 * docs/design/notes/2026-09-23-omx-lv2-pack-scope.md §7 lane B, under the HRP shells rule
 * (omx-hrp.lv2/omx-hrp.ttl, citing 2026-07-16-recording-vsc.md §0.5): one C core, N shells.
 * The core here is `mix_delay.h`'s `omx_fx_delay_process` — THE SAME INLINE the console's
 * delay stage runs (mix_lane.h step OMX_STAGE_DELAY) — and this file adds no DSP to it. What it
 * adds is the three things a host's port model needs and the kernel's atom does not carry:
 *
 *   1. RINGS. `struct omx_fx_delay_state` is caller-owned by design ("allocated by the CALLER
 *      off the RT thread"); the LV2 shell's caller is `instantiate`, which hands the two rings
 *      in through {@link omx_delay_lv2_init} exactly as the console's `mixer_strip.c` does on a
 *      disabled->enabled transition. The rings are sized {@link OMX_FXDELAY_CAP} (the declared
 *      ceiling at 192 kHz), so the full 2 s travel is reachable at every declared rate.
 *   2. RESOLVE. A host publishes control ports as floats in the operator's units (ms, unit
 *      knobs, toggles). {@link omx_delay_lv2_resolve} turns them into the atom the kernel reads,
 *      with the ms -> samples step being the kernel's own {@link omx_fxdelay_ms_to_samples} —
 *      shared with `mixer_rt.c`'s `resolve_fx_delay`, never a second conversion — and every
 *      unit knob clamped into the kernel's stated contract (feedback strictly below 1, mix and
 *      tone in unit range, NaN floored) because a foreign host's port is not the console's
 *      codec and nothing upstream has clamped it.
 *   3. IN -> OUT. The kernel works IN PLACE on the strip's two legs; an LV2 host connects
 *      separate input and output buffers (and may connect them to the same memory). run() copies
 *      in to out when they differ and runs the kernel on out. Identity when bypassed.
 *
 * DELAY-TIME STATE, as mix_delay.h states it: a time change moves the read tap and MAY CLICK —
 * "standard for a delay, and documented rather than cross-faded". The shell does not add a
 * cross-fade the console does not have; it re-resolves the tap every cycle from the port, so
 * a host automating `time_l` gets exactly the desk's behaviour. Re-engaging from bypass CLEARS
 * the rings (wpos/damping reset), as the console does, so a re-enabled delay starts silent
 * instead of bursting a tail buffered before the bypass.
 *
 * LATENCY IS ZERO, AND PUBLISHED. A musical delay is the EFFECT the operator asked for, not a
 * processing latency a host should compensate away — compensating it would cancel the delay.
 * The dry path (mix < 1) is sample-aligned with the input (the kernel reads `l[i]` and writes
 * `dry * xl` in the same frame), so the plugin introduces no latency of its own. The
 * `lv2:latency` port exists because a host cannot tell "zero" from "never said".
 *
 * Everything below is plain C; `mix_lv2_delay.test.c` drives it with a plain `cc`, and
 * `mix_lv2_delay.c` carries it across the host boundary and nothing more.
 */
#ifndef OMX_MIX_LV2_DELAY_H
#define OMX_MIX_LV2_DELAY_H

#include <stdint.h>
#include <string.h>

#include "mix_delay.h"

/** Audio legs in and out: TWO, because the kernel is the console's stereo FX delay (ping-pong
 * cross-feeds the legs; a mono shell could not carry that control). */
#define OMX_DELAY_LV2_CHANNELS 2

/** Frames of latency this plugin introduces: NONE — see the header note. The delay IS the
 * effect; the dry path is frame-aligned with the input. */
#define OMX_DELAY_LV2_LATENCY_FRAMES 0.0f

/** Port defaults, quoted where the desk quotes them (packages/core/src/strip-fx-limits.ts:
 * FX_DELAY_TIME_RANGE.default, FX_DELAY_FEEDBACK_RANGE.default, DELAY_TONE_RANGE.default) and
 * carried into the TTL by mix_lv2_delay.test.c's index/default parity check. MIX is the one
 * number that is NOT the desk's: the console's delay is a send/return effect whose return bus
 * does the blending, so its mix ships at 1 (wet-only); a foreign host racks this plugin INLINE
 * on a channel, where wet-only would swallow the dry signal, so the insert ships half-and-half. */
#define OMX_DELAY_LV2_TIME_MS_DEFAULT OMX_DELAY_TIME_MS_DEFAULT
#define OMX_DELAY_LV2_FEEDBACK_DEFAULT 0.3f
#define OMX_DELAY_LV2_FEEDBACK_MAX 0.99f
#define OMX_DELAY_LV2_MIX_DEFAULT 0.5f
#define OMX_DELAY_LV2_TONE_DEFAULT 0.3f

/** One instance. The rings are NOT owned here; {@link omx_delay_lv2_init} is given them. */
typedef struct {
  float sr;
  struct omx_fx_delay atom;
  struct omx_fx_delay_state state;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the rings. */
  int was_engaged;
  int ready;
} OmxDelayLv2;

/**
 * Bind an instance to its rate and the caller's two rings of `cap` floats each. Returns 1 when
 * usable, 0 when not. A refused init leaves `ready` clear and {@link omx_delay_lv2_run} is then
 * the identity: absence is a wire, not a crash.
 */
static inline int omx_delay_lv2_init(OmxDelayLv2 *s, float sr, float *ring_l, float *ring_r,
                                     uint32_t cap) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 0.0f) || !ring_l || !ring_r || cap == 0u) return 0;
  s->sr = sr;
  s->state.ring_l = ring_l;
  s->state.ring_r = ring_r;
  s->state.cap = cap;
  s->atom.enabled = 0;
  s->ready = 1;
  return 1;
}

/** Clear the rings and the per-leg damping — the console's own re-enable recipe
 * (mixer_strip.c, "a re-enable of an already allocated (bypassed) delay CLEARS the stale
 * tail"). Off the audio thread on the desk; here it runs inside run() on the engage edge, which
 * is a bounded memset over a fixed ring and no allocation — the same cost class as the block. */
static inline void omx_delay_lv2_clear(OmxDelayLv2 *s) {
  if (!s || !s->ready) return;
  memset(s->state.ring_l, 0, (size_t)s->state.cap * sizeof(float));
  memset(s->state.ring_r, 0, (size_t)s->state.cap * sizeof(float));
  s->state.wpos = 0u;
  s->state.damp_l = s->state.damp_r = 0.0f;
}

/** A unit knob from a port: NaN and out-of-range floor/ceil into [0, hi]. */
static inline float omx_delay_lv2_unit(float v, float hi) {
  return v > 0.0f ? (v > hi ? hi : v) : 0.0f;
}

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle.
 *
 * `bypass` non-zero -> the atom is disabled and the kernel passes through. Times are ms and go
 * through the kernel's own ms -> samples derivation at the instance rate. `feedback` is clamped
 * to [0, {@link OMX_DELAY_LV2_FEEDBACK_MAX}] — the desk's declared travel, which already stops
 * short of the unity the kernel refuses — `mix` and `tone` to unit range.
 */
static inline void omx_delay_lv2_resolve(OmxDelayLv2 *s, int bypass, float time_l_ms,
                                         float time_r_ms, float feedback, float mix, float tone,
                                         int pingpong) {
  if (!s || !s->ready) return;
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_delay_lv2_clear(s);
  s->was_engaged = engaged;
  s->atom.enabled = engaged;
  s->atom.d_l = omx_fxdelay_ms_to_samples(time_l_ms, s->sr);
  s->atom.d_r = omx_fxdelay_ms_to_samples(time_r_ms, s->sr);
  s->atom.feedback = omx_delay_lv2_unit(feedback, OMX_DELAY_LV2_FEEDBACK_MAX);
  s->atom.mix = omx_delay_lv2_unit(mix, 1.0f);
  s->atom.tone = omx_delay_lv2_unit(tone, 1.0f);
  s->atom.pingpong = pingpong ? 1 : 0;
}

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias (hosts do connect a port in place). Not ready,
 * or bypassed, is the identity byte for byte.
 */
#define OMX_CONTRACT_STAGE "fx-delay/lv2-run"
static inline void omx_delay_lv2_run(OmxDelayLv2 *s, const float *in_l, const float *in_r,
                                     float *out_l, float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memcpy(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memcpy(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_fx_delay_process(out_l, out_r, n, &s->atom, &s->state, s->sr);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_LV2_DELAY_H */
