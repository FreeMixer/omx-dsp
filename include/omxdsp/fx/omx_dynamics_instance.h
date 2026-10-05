/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_dynamics_instance.h — the strip compressor as a plugin instance: the shell's core, with no
 * format in it.
 *
 * The shape is omx_delay_instance.h's (one C core, N shells). The core here is `omx_dyn.h`'s
 * `omx_dynamics` — the self-detecting dynamics slot, THE SAME kernel the console's comp slot runs
 * — and this file adds no DSP to it. What it adds is what a host's port model needs and the
 * kernel's atom does not carry:
 *
 *   1. READY. {@link omx_dynamics_instance_init} binds the instance to its rate and refuses a rate
 *      that is not positive. A refused instance is the identity in
 *      {@link omx_dynamics_instance_run} and reports no latency: absence is a wire, not a crash.
 *   2. RESOLVE. A host publishes control ports as floats in the operator's units (dB, ratio, ms,
 *      toggles). {@link omx_dynamics_instance_resolve} is the console's `resolve_dyn` (mixer_rt.c)
 *      for a comp slot — ACT-ABOVE, make-up dB -> linear by omx_db_to_lin, the attack and release
 *      poles by omx_pole_from_time_ms at the instance rate, the attack's ms carried beside its pole
 *      for `auto` — with every value clamped into the declared OMX_COMP_* travel (NaN floored)
 *      because a foreign host's port is not the console's codec and nothing upstream has clamped
 *      it. An oversampling mode that is not one of OMX_DYN_OVS_* reads as `auto`, the default.
 *   3. IN -> OUT. The kernel works IN PLACE on the strip's two legs; a host connects separate
 *      input and output buffers (and may connect them to the same memory). run() copies in to out
 *      when they differ and runs the kernel on out. Without a key nothing else can alias.
 *
 * WHAT THE DESK'S COMP HAS THAT THIS ONE DOES NOT: the `mix` control (OMX_COMP_MIX_PCT_*). The
 * kernel's atom carries no dry/wet, and a blend here would be DSP the console's slot does not run.
 *
 * STATE ACROSS BYPASS: the kernel's disabled path leaves the slot's state as it was, and the
 * shell adds no re-arm the console's slot does not have. {@link omx_dynamics_instance_init} (a
 * host's instantiate/activate) is the one place the state is cleared.
 *
 * LATENCY IS PUBLISHED: OMX_OVS_LATENCY_4X while the 4x control path is engaged (the kernel's
 * compensation delay), else 0 — read off omx_dyn_oversample_factor, the kernel's own derivation.
 *
 * Everything below is plain C; test/fx/dynamics_instance.test.c drives it with a plain `cc`.
 */
#ifndef OMX_DYNAMICS_INSTANCE_H
#define OMX_DYNAMICS_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_contract_limits.h>
#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_units.h>

/** @brief Audio legs in and out: TWO, the strip's stereo pair (one detector, linked gain). */
#define OMX_DYNAMICS_INSTANCE_CHANNELS 2

/** @brief One instance: the rate, the resolved atom and the slot's state. */
typedef struct {
  float sr;
  struct omx_dyn atom;
  struct omx_dyn_state st;
  int ready;
} OmxDynamicsInstance;

/* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE the kernel's gain
 * computer states — ratio at least one, knee not negative, make-up positive — and sits inside the
 * declared travel with poles in [0, 1), whatever the host's ports held. This is the shell's whole
 * promise to the kernel, and the POST below is that promise verbatim. */
#define OMX_CONTRACT_STAGE "dynamics/instance-resolve"
/**
 * @brief Resolve the host's control-port values into the comp slot's atom for one cycle.
 * @param s The instance; a refused one is left untouched.
 * @param bypass Non-zero disables the slot (the identity).
 * @param threshold_db The threshold, dB.
 * @param ratio The compression ratio.
 * @param knee_db The knee width, dB; 0 is a hard knee.
 * @param attack_ms The attack, ms.
 * @param release_ms The release, ms.
 * @param makeup_db The make-up gain, dB.
 * @param rms Non-zero detects RMS, zero peak.
 * @param ovs_mode OMX_DYN_OVS_AUTO, _OFF or _X4; anything else reads as `auto`.
 * @post `atom-meets-the-kernel-preconditions`: every field inside the declared OMX_COMP_* travel.
 * @note RT-safe: two expf and one powf per call. Thread-safe on distinct instances.
 */
static inline void omx_dynamics_instance_resolve(OmxDynamicsInstance *s, int bypass,
                                                 float threshold_db, float ratio, float knee_db,
                                                 float attack_ms, float release_ms,
                                                 float makeup_db, int rms, int ovs_mode) {
  if (!s || !s->ready) return;
  struct omx_dyn *p = &s->atom;
  memset(p, 0, sizeof *p);
  p->enabled = bypass ? 0 : 1;
  p->gc.mode = OMX_DYN_ABOVE;
  p->detect = rms ? OMX_DETECT_RMS : OMX_DETECT_PEAK;
  p->gc.thresh_db = omx_clampf(threshold_db, OMX_COMP_THRESHOLD_DB_MIN, OMX_COMP_THRESHOLD_DB_MAX);
  p->gc.ratio = omx_clampf(ratio, OMX_COMP_RATIO_MIN, OMX_COMP_RATIO_MAX);
  p->gc.knee_db = omx_clampf(knee_db, OMX_COMP_KNEE_DB_MIN, OMX_COMP_KNEE_DB_MAX);
  p->gc.range_db = 0.0f; /* ignored ACT-ABOVE */
  p->gc.makeup_lin = omx_db_to_lin(omx_clampf(makeup_db, OMX_COMP_MAKEUP_DB_MIN, OMX_COMP_MAKEUP_DB_MAX));
  p->ovs_mode = ovs_mode == OMX_DYN_OVS_OFF || ovs_mode == OMX_DYN_OVS_X4 ? ovs_mode : OMX_DYN_OVS_AUTO;
  const float a_ms = omx_clampf(attack_ms, OMX_COMP_ATTACK_MS_MIN, OMX_COMP_ATTACK_MS_MAX);
  const float r_ms = omx_clampf(release_ms, OMX_COMP_RELEASE_MS_MIN, OMX_COMP_RELEASE_MS_MAX);
  p->attack_ms = a_ms;
  p->attack_coeff = omx_pole_from_time_ms(a_ms, s->sr);
  p->release_coeff = omx_pole_from_time_ms(r_ms, s->sr);
  OMX_POST(p->gc.ratio >= OMX_COMP_RATIO_MIN && p->gc.ratio <= OMX_COMP_RATIO_MAX &&
               p->gc.knee_db >= OMX_COMP_KNEE_DB_MIN && p->gc.knee_db <= OMX_COMP_KNEE_DB_MAX &&
               p->gc.thresh_db >= OMX_COMP_THRESHOLD_DB_MIN &&
               p->gc.thresh_db <= OMX_COMP_THRESHOLD_DB_MAX && p->gc.makeup_lin >= 1.0f &&
               p->attack_ms >= OMX_COMP_ATTACK_MS_MIN && p->attack_ms <= OMX_COMP_ATTACK_MS_MAX &&
               p->attack_coeff >= 0.0f && p->attack_coeff < 1.0f && p->release_coeff >= 0.0f &&
               p->release_coeff < 1.0f,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Bind an instance to its rate (instantiate/activate): the slot's state cleared, base path,
 *        the atom resolved at the declared defaults (peak, `auto`).
 * @param s The instance.
 * @param sr The host's sample rate, Hz.
 * @return 1 when usable, 0 when not (NULL instance, rate not positive or NaN); a refused instance
 *         is the identity.
 * @note No allocation; clears the whole instance. Thread-safe on distinct instances.
 */
static inline int omx_dynamics_instance_init(OmxDynamicsInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 0.0f)) return 0;
  s->sr = sr;
  omx_dyn_state_init(&s->st, 1u);
  s->ready = 1;
  omx_dynamics_instance_resolve(s, 0, OMX_COMP_THRESHOLD_DB_DEFAULT, OMX_COMP_RATIO_DEFAULT,
                                OMX_COMP_KNEE_DB_DEFAULT, OMX_COMP_ATTACK_MS_DEFAULT,
                                OMX_COMP_RELEASE_MS_DEFAULT, OMX_COMP_MAKEUP_DB_DEFAULT, 0,
                                OMX_DYN_OVS_AUTO);
  return 1;
}

/**
 * @brief The frames of latency the instance reports for its current atom.
 * @param s The instance.
 * @return OMX_OVS_LATENCY_4X while 4x is engaged, else 0; 0 for a refused instance.
 * @note RT-safe. Thread-safe: pure in `s`.
 */
static inline float omx_dynamics_instance_latency(const OmxDynamicsInstance *s) {
  if (!s || !s->ready) return 0.0f;
  return omx_dyn_oversample_factor(&s->atom) == 4u ? (float)OMX_OVS_LATENCY_4X : 0.0f;
}

#define OMX_CONTRACT_STAGE "dynamics/instance-run"
/**
 * @brief THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's
 *        kernel in place on out.
 * @param s The instance.
 * @param in_l The left input block.
 * @param in_r The right input block.
 * @param out_l The left output block; may alias `in_l`.
 * @param out_r The right output block; may alias `in_r`.
 * @param n Frames.
 * @post `bypass-identity`: a bypassed slot leaves out equal to in.
 * @note RT-safe: no allocation, no lock. Not ready is the identity byte for byte. Thread-safe on
 *       distinct instances.
 */
static inline void omx_dynamics_instance_run(OmxDynamicsInstance *s, const float *in_l,
                                             const float *in_r, float *out_l, float *out_r,
                                             uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_dynamics(out_l, out_r, n, &s->atom, &s->st);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_DYNAMICS_INSTANCE_H */
