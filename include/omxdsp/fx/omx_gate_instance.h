/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_gate_instance.h — the strip gate as a plugin instance: the shell's core, with no format in it.
 *
 * The shape is omx-plugins' omx_delay_instance.h's (one C core, N shells). The core here is `omx_gate.h` — its
 * `omx_gate_resolve` (the console's gateStateToNativeDyn as control reads) and its `omx_gate_run`
 * (the host's block through fixed scratch by `omx_dynamics_keyed`, THE SAME kernel the console's
 * gate slot runs) — and this file adds no DSP to it. What it adds is what a host's port model
 * needs and `omx_gate.h`'s pointer-per-port face does not carry:
 *
 *   1. READY. {@link omx_gate_instance_init} binds the instance to its rate and refuses a rate
 *      that is not positive. A refused instance is the identity in {@link omx_gate_instance_run}
 *      and reports no latency: absence is a wire, not a crash.
 *   2. RESOLVE. A host publishes control ports as floats in the operator's units (dB, ratio, ms,
 *      toggles). {@link omx_gate_instance_resolve} takes them BY VALUE and hands them to
 *      `omx_gate_resolve` — the one resolution, never a second one — so every value lands inside
 *      the declared OMX_GATE_* travel (NaN floored) whatever the host's port held, and `bypass`
 *      is the slot's `enabled` control inverted.
 *   3. IN -> OUT. `omx_gate_run` already copies every block (inputs AND key) through its scratch,
 *      so any aliasing a host does (an output on its input, the key on an output) is safe.
 *
 * STATE ACROSS BYPASS: the kernel's disabled path leaves the slot's state as it was, and the
 * shell adds no re-arm the console's slot does not have. {@link omx_gate_instance_init} (a host's
 * instantiate/activate) is the one place the state is cleared.
 *
 * LATENCY IS PUBLISHED: OMX_OVS_LATENCY_4X while the attack engages the 4x control path (the
 * kernel's compensation delay), else 0 — `omx_gate_latency` over the resolved atom, never restated.
 *
 * Everything below is plain C; test/fx/gate_instance.test.c drives it with a plain `cc`.
 */
#ifndef OMX_GATE_INSTANCE_H
#define OMX_GATE_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_gate.h>

/** @brief Audio legs in and out: TWO, the strip gate's stereo pair (one detector, linked gain). */
#define OMX_GATE_INSTANCE_CHANNELS 2

/** @brief One instance: the rate, the resolved atom, the key-source control and the gate's state. */
typedef struct {
  float sr;
  struct omx_dyn atom;
  /** The key-source control as resolved: 1 listens to the key, 0 is SELF. */
  float key_external;
  struct omx_gate gate;
  int ready;
} OmxGateInstance;

/**
 * @brief Bind an instance to its rate (instantiate/activate): the slot's state cleared, base path,
 *        the atom resolved at the declared defaults.
 * @param s The instance.
 * @param sr The host's sample rate, Hz.
 * @return 1 when usable, 0 when not (NULL instance, rate not positive or NaN); a refused instance
 *         is the identity.
 * @note No allocation; clears the whole instance. Thread-safe on distinct instances.
 */
static inline int omx_gate_instance_init(OmxGateInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 0.0f)) return 0;
  s->sr = sr;
  omx_gate_init(&s->gate, sr);
  const struct omx_gate_controls dflt = {0};
  omx_gate_resolve(&s->gate, &dflt, &s->atom);
  s->key_external = 1.0f;
  s->ready = 1;
  return 1;
}

/* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE the kernel's gain
 * computer states — ratio at least the gate's floor, range an attenuation — and sits inside the
 * declared travel with poles in [0, 1), whatever the host's ports held. This is the shell's whole
 * promise to the kernel, and the POST below is that promise verbatim. */
#define OMX_CONTRACT_STAGE "gate/instance-resolve"
/**
 * @brief Resolve the host's control-port values into the gate's atom for one cycle.
 * @param s The instance; a refused one is left untouched.
 * @param bypass Non-zero disables the slot (the identity).
 * @param key_source The contract's keySource, as its index: 1 (sidechain) listens to the key handed to
 *        {@link omx_gate_instance_run}, 0 (self) to the gate's own input; any non-zero is sidechain.
 * @param threshold_db The open point, dB.
 * @param ratio The expansion ratio.
 * @param range_db The attenuation floor, dB.
 * @param attack_ms The attack, ms.
 * @param release_ms The release, ms.
 * @post `atom-meets-the-kernel-preconditions`: every field inside the declared OMX_GATE_* travel.
 * @note RT-safe: two expf per call. Thread-safe on distinct instances.
 */
static inline void omx_gate_instance_resolve(OmxGateInstance *s, int bypass, int key_source,
                                             float threshold_db, float ratio, float range_db,
                                             float attack_ms, float release_ms) {
  if (!s || !s->ready) return;
  const float enabled = bypass ? 0.0f : 1.0f;
  s->key_external = key_source ? 1.0f : 0.0f;
  const struct omx_gate_controls c = {&enabled,    &s->key_external, &threshold_db, &ratio,
                                      &range_db,   &attack_ms,       &release_ms};
  omx_gate_resolve(&s->gate, &c, &s->atom);
  const struct omx_dyn *p = &s->atom;
  (void)p; /* read only by the POST, compiled out of a release build */
  OMX_POST(p->gc.mode == OMX_DYN_BELOW && p->gc.ratio >= OMX_GATE_RATIO_MIN &&
               p->gc.ratio <= OMX_GATE_RATIO_MAX && p->gc.range_db >= OMX_GATE_RANGE_DB_MIN &&
               p->gc.range_db <= OMX_GATE_RANGE_DB_MAX &&
               p->gc.thresh_db >= OMX_GATE_THRESHOLD_DB_MIN &&
               p->gc.thresh_db <= OMX_GATE_THRESHOLD_DB_MAX && p->attack_ms >= OMX_GATE_ATTACK_MS_MIN &&
               p->attack_ms <= OMX_GATE_ATTACK_MS_MAX && p->attack_coeff >= 0.0f &&
               p->attack_coeff < 1.0f && p->release_coeff >= 0.0f && p->release_coeff < 1.0f,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The frames of latency the instance reports for its current atom.
 * @param s The instance.
 * @return OMX_OVS_LATENCY_4X while 4x is engaged, else 0; 0 for a refused instance.
 * @note RT-safe. Thread-safe: pure in `s`.
 */
static inline float omx_gate_instance_latency(const OmxGateInstance *s) {
  return s && s->ready ? omx_gate_latency(&s->atom) : 0.0f;
}

/**
 * @brief THE AUDIO CALLBACK'S WHOLE SHARE: the block through `omx_gate_run` with the resolved atom.
 * @param s The instance.
 * @param key The sidechain block, or NULL (unconnected: the self-detecting gate).
 * @param in_l The left input block.
 * @param in_r The right input block.
 * @param out_l The left output block; may alias any input or the key.
 * @param out_r The right output block; may alias any input or the key.
 * @param n Frames.
 * @note RT-safe: no allocation, no lock. Not ready is the identity byte for byte. Thread-safe on
 *       distinct instances.
 */
static inline void omx_gate_instance_run(OmxGateInstance *s, const float *key, const float *in_l,
                                         const float *in_r, float *out_l, float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (!s || !s->ready) {
    if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
    if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
    return;
  }
  omx_gate_run(&s->gate, &s->atom, key, &s->key_external, in_l, in_r, out_l, out_r, n);
}

#endif /* OMX_GATE_INSTANCE_H */
