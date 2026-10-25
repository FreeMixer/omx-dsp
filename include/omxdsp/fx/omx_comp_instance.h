/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_comp_instance.h (formerly omx_dynamics_instance.h) — the strip compressor as a plugin
 * instance: the shell's core, with no format in it. Named after the contract's `comp` kernel, the
 * engine's one spelling (OMX_STAGE_COMP, COMP_LIMITS).
 *
 * The shape is omx-plugins' omx_delay_instance.h's (one C core, N shells). The core here is `omx_dyn.h`'s
 * `omx_dynamics` — the self-detecting dynamics slot, THE SAME kernel the console's comp slot runs
 * — and this file adds no DSP to it. What it adds is what a host's port model needs and the
 * kernel's atom does not carry:
 *
 *   1. READY. {@link omx_comp_instance_init} binds the instance to its rate and refuses a rate
 *      that is not positive. A refused instance is the identity in
 *      {@link omx_comp_instance_run} and reports no latency: absence is a wire, not a crash.
 *   2. RESOLVE. A host publishes control ports as floats in the operator's units (dB, ratio, ms,
 *      toggles). {@link omx_comp_instance_resolve} is the console's `resolve_dyn` (mixer_rt.c)
 *      for a comp slot — ACT-ABOVE, make-up dB -> linear by omx_db_to_lin, the attack and release
 *      poles by omx_pole_from_time_ms at the instance rate, the attack's ms carried beside its pole
 *      for `auto` — with every value clamped into the declared OMX_COMP_* travel (NaN floored)
 *      because a foreign host's port is not the console's codec and nothing upstream has clamped
 *      it. The `kind` picks the detector as the console does (`console-rig.ts`): RMS for a comp,
 *      peak for a limiter. The detector oversampling is a DETECTOR_OVERSAMPLINGS index, in the
 *      order of OMX_DYN_OVS_*; the detector topology is a DETECTOR_TOPOLOGY index
 *      (OMX_DETECTOR_TOPOLOGY_*); a value outside its set reads as its declared default.
 *   3. IN -> OUT. The kernel works IN PLACE on the strip's two legs; a host connects separate
 *      input and output buffers (and may connect them to the same memory). run() copies in to out
 *      when they differ and runs the kernel on out. Without a key nothing else can alias.
 *
 * THE MIX: the contract's mixPct (percent) reaches the kernel's own dry share, `omx_dyn.dry` by
 * omx_dyn_dry_share; 100 % is the processed output bit for bit, 0 % the compensated input.
 *
 * STATE ACROSS BYPASS: the kernel's disabled path leaves the slot's state as it was, and the
 * shell adds no re-arm the console's slot does not have. {@link omx_comp_instance_init} (a
 * host's instantiate/activate) is the one place the state is cleared.
 *
 * THE KEY. The detector hears the strip's own legs, or the key handed to
 * {@link omx_comp_instance_run_keyed} (keySource), through the gate's key filter (keyHpHz, keyLpHz:
 * omx_gate.h's one Butterworth section per edge, in double) and the tilt (keyTiltDb, omx_tilt.h, about
 * KEY_TILT_PIVOT_HZ), on each leg, folded into one level by detectLink: linked the louder leg, mid
 * `(L + R)/2`, side `(L - R)/2`, max the louder of the mid and the side. keyListen puts what the
 * detector hears on the output. At the come-up values (self, both edges out, flat, linked, no listen)
 * the key path does not run and the slot is the self-detecting compressor bit for bit; a key path
 * engaged at 4x is upsampled after the fold.
 *
 * RELEASE MODE. releaseMode `fixed` is the detector's release at releaseMs, the compressor that
 * shipped bit for bit. `opto` and `auto` run the kernel's program release (omx_env_program_release,
 * PROGRAM_RELEASE_PROFILES.opto and .auto) on the gain computer's reduction, with the detector
 * releasing at its attack pole: releaseMs is inert there, kept for when fixed returns. The
 * effectiveAttackMs readout is {@link omx_comp_instance_effective_attack_ms}.
 *
 * HOLD AND DUCK. holdMs reaches the kernel's hold (omx_dyn.hold_frames, ABOVE mode: the detector
 * frozen after it last rose). duck holds the reduction to rangeDb (omx_dyn.floor_db), so the
 * programme gain is `max(rangeDb, the law at the key's level)`; a range of 0 dB ducks nothing.
 *
 * LATENCY IS PUBLISHED: OMX_OVS_LATENCY_4X while the 4x control path is engaged (the kernel's
 * compensation delay), else 0 — read off omx_dyn_oversample_factor, the kernel's own derivation.
 * A feedback slot is never oversampled, so it reports 0.
 *
 * Everything below is plain C; test/fx/comp_instance.test.c drives it with a plain `cc`.
 */
#ifndef OMX_COMP_INSTANCE_H
#define OMX_COMP_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/omxdsp_definitions.h>
#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_gate.h>
#include <omxdsp/omx_tilt.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_units.h>

/** @brief Audio legs in and out: TWO, the strip's stereo pair (one detector, linked gain). */
#define OMX_COMP_INSTANCE_CHANNELS 2

/** @brief Frames the key path handles per pass; nothing observable depends on it. */
#define OMX_COMP_INSTANCE_CHUNK 256u

/** @brief Where the key's tilt pivots, Hz: KEY_TILT_PIVOT_HZ. */
#define OMX_COMP_INSTANCE_TILT_PIVOT_HZ ((double)OMX_KEY_TILT_PIVOT_HZ)

/** @brief One instance: the rate, the resolved atom, the slot's state and the key path. */
typedef struct {
  float sr;
  struct omx_dyn atom;
  struct omx_dyn_state st;
  int ready;
  int src;    /**< keySource: 1 the sidechain, 0 the strip's own legs */
  int listen; /**< keyListen: 1 puts what the detector hears on the output */
  int fold;   /**< detectLink, an OMX_COMP_DETECT_LINKS_* member */
  struct omx_gate_key filt[2]; /**< the key filter, per key leg */
  struct omx_tilt tilt;        /**< the key's tilt */
  double tilt_s[2][4];         /**< the tilt's history, per key leg */
  float ka[OMX_COMP_INSTANCE_CHUNK], kb[OMX_COMP_INSTANCE_CHUNK], kd[OMX_COMP_INSTANCE_CHUNK];
} OmxCompInstance;

/* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE the kernel's gain
 * computer states — ratio at least one, knee not negative, make-up positive — and sits inside the
 * declared travel with poles in [0, 1), whatever the host's ports held. This is the shell's whole
 * promise to the kernel, and the POST below is that promise verbatim. */
#define OMX_CONTRACT_STAGE "comp/instance-resolve"
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
 * @param mix_pct The mix, percent (OMX_COMP_MIX_PCT_*): 100 fully processed, 0 the dry input; a
 *        non-finite word reads as the declared default.
 * @param kind A COMP_KINDS index: comp detects RMS, limiter peak; anything else reads as comp.
 * @param detector_oversampling A DETECTOR_OVERSAMPLINGS index (auto, off, x4: OMX_DYN_OVS_*);
 *        anything else reads as `auto`.
 * @param detector_topology A DETECTOR_TOPOLOGY index (OMX_DETECTOR_TOPOLOGY_*): `feedback` runs
 *        the kernel's feedback loop at factor 1 whatever the oversampling says; anything else
 *        reads as `feedforward`.
 * @param key_source A COMP_KEY_SOURCES index: `sidechain` listens to the key run_keyed() is handed.
 * @param key_hp_hz The key filter's high-pass corner, Hz; 0 is out.
 * @param key_lp_hz The key filter's low-pass corner, Hz; 0 is out.
 * @param key_tilt_db The key's tilt, dB per octave at the pivot; 0 is flat.
 * @param key_listen A COMP_KEY_LISTENS index: `on` puts what the detector hears on the output.
 * @param hold_ms The hold, ms; 0 is none.
 * @param detect_link A COMP_DETECT_LINKS index: linked, mid, side, max.
 * @param duck A COMP_DUCKS index: `on` holds the reduction to `range_db`.
 * @param range_db The ducker's depth, dB.
 * @param release_mode A COMP_RELEASE_MODES index: fixed, opto, auto; anything else reads as fixed.
 *        Every travel is clamped into its declared range, a non-finite word reading as its default;
 *        a choice outside its ids reads as its default.
 * @post `atom-meets-the-kernel-preconditions`: every field inside the declared OMX_COMP_* travel.
 * @note RT-safe and allocation-free: two expf and one powf per call, the key filter's design
 *       (omx_gate_key_design: cos, sin, exp, sqrt) for each leg whose edge moved, and the tilt's design
 *       (omx_tilt_design: a tan and a sqrt) when its slope moved. Thread-safe on distinct instances.
 */
static inline void omx_comp_instance_resolve(OmxCompInstance *s, int bypass,
                                             float threshold_db, float ratio, float knee_db,
                                                 float attack_ms, float release_ms,
                                                 float makeup_db, float mix_pct, int kind,
                                                 int detector_oversampling, int detector_topology,
                                                 int key_source, float key_hp_hz, float key_lp_hz,
                                                 float key_tilt_db, int key_listen, float hold_ms,
                                                 int detect_link, int duck, float range_db, int release_mode) {
  if (!s || !s->ready) return;
  struct omx_dyn *p = &s->atom;
  memset(p, 0, sizeof *p);
  p->enabled = bypass ? 0 : 1;
  p->gc.mode = OMX_DYN_ABOVE;
  p->detect = kind == OMX_COMP_KINDS_LIMITER ? OMX_DETECT_PEAK : OMX_DETECT_RMS;
  p->gc.thresh_db = omx_clampf(threshold_db, OMX_COMP_THRESHOLD_DB_MIN, OMX_COMP_THRESHOLD_DB_MAX);
  p->gc.ratio = omx_clampf(ratio, OMX_COMP_RATIO_MIN, OMX_COMP_RATIO_MAX);
  p->gc.knee_db = omx_clampf(knee_db, OMX_COMP_KNEE_DB_MIN, OMX_COMP_KNEE_DB_MAX);
  p->gc.range_db = 0.0f; /* ignored ACT-ABOVE */
  p->gc.makeup_lin = omx_db_to_lin(omx_clampf(makeup_db, OMX_COMP_MAKEUP_DB_MIN, OMX_COMP_MAKEUP_DB_MAX));
  p->dry = omx_dyn_dry_share(omx_clamp_or(mix_pct, OMX_COMP_MIX_PCT_MIN, OMX_COMP_MIX_PCT_MAX, OMX_COMP_MIX_PCT_DEFAULT));
  p->ovs_mode = detector_oversampling == OMX_DETECTOR_OVERSAMPLINGS_OFF ? OMX_DYN_OVS_OFF
                : detector_oversampling == OMX_DETECTOR_OVERSAMPLINGS_4X ? OMX_DYN_OVS_X4
                                                                          : OMX_DYN_OVS_AUTO;
  p->topology = detector_topology == OMX_DETECTOR_TOPOLOGY_FEEDBACK ? OMX_DETECTOR_TOPOLOGY_FEEDBACK
                                                                     : OMX_DETECTOR_TOPOLOGY_FEEDFORWARD;
  const float a_ms = omx_clampf(attack_ms, OMX_COMP_ATTACK_MS_MIN, OMX_COMP_ATTACK_MS_MAX);
  const float r_ms = omx_clampf(release_ms, OMX_COMP_RELEASE_MS_MIN, OMX_COMP_RELEASE_MS_MAX);
  p->attack_ms = a_ms;
  p->attack_coeff = omx_pole_from_time_ms(a_ms, s->sr);
  p->release_coeff = omx_pole_from_time_ms(r_ms, s->sr);
  const int key_src = key_source == OMX_COMP_KEY_SOURCES_SIDECHAIN ? 1 : 0;
  const float hp = omx_clamp_or(key_hp_hz, (float)OMX_COMP_KEY_HP_HZ_RANGE_MIN, (float)OMX_COMP_KEY_HP_HZ_RANGE_MAX,
                                (float)OMX_COMP_KEY_HP_HZ_RANGE_DEFAULT);
  const float lp = omx_clamp_or(key_lp_hz, (float)OMX_COMP_KEY_LP_HZ_RANGE_MIN, (float)OMX_COMP_KEY_LP_HZ_RANGE_MAX,
                                (float)OMX_COMP_KEY_LP_HZ_RANGE_DEFAULT);
  const float tilt_slope = omx_clamp_or(key_tilt_db, (float)OMX_COMP_KEY_TILT_DB_RANGE_MIN,
                                        (float)OMX_COMP_KEY_TILT_DB_RANGE_MAX, (float)OMX_COMP_KEY_TILT_DB_RANGE_DEFAULT);
  const int lis = key_listen == OMX_COMP_KEY_LISTENS_ON ? 1 : 0;
  const float hold = omx_clamp_or(hold_ms, (float)OMX_COMP_HOLD_MS_RANGE_MIN, (float)OMX_COMP_HOLD_MS_RANGE_MAX,
                                  (float)OMX_COMP_HOLD_MS_RANGE_DEFAULT);
  const int link_fold = detect_link >= 0 && detect_link < (int)OMX_COMP_DETECT_LINKS_COUNT
                            ? detect_link
                            : (int)OMX_COMP_DETECT_LINKS_DEFAULT;
  const int duck_on = duck == OMX_COMP_DUCKS_ON ? 1 : 0;
  const float duck_range = omx_clamp_or(range_db, (float)OMX_COMP_RANGE_DB_RANGE_MIN, (float)OMX_COMP_RANGE_DB_RANGE_MAX,
                                        (float)OMX_COMP_RANGE_DB_RANGE_DEFAULT);
  s->src = key_src;
  s->listen = lis;
  s->fold = link_fold;
  omx_gate_key_design(&s->filt[0], s->sr, hp, lp);
  omx_gate_key_design(&s->filt[1], s->sr, hp, lp);
  const double a = omx_tilt_a_of_slope((double)tilt_slope);
  if (a != s->tilt.a) {
    if (a == 1.0 || s->tilt.a == 1.0) memset(s->tilt_s, 0, sizeof s->tilt_s);
    omx_tilt_design(&s->tilt, a, OMX_COMP_INSTANCE_TILT_PIVOT_HZ, (double)s->sr);
  }
  const int rel_mode = release_mode == OMX_COMP_RELEASE_MODES_OPTO || release_mode == OMX_COMP_RELEASE_MODES_AUTO
                           ? release_mode
                           : (int)OMX_COMP_RELEASE_MODES_FIXED;
  p->release_mode = rel_mode;
  if (rel_mode != OMX_COMP_RELEASE_MODES_FIXED) {
    /* The detector a symmetric smoother; every release character lives in the follower. */
    p->release_coeff = p->attack_coeff;
    for (uint32_t k = 0; k < 2u; k++) {
      if (rel_mode == OMX_COMP_RELEASE_MODES_OPTO) omx_env_program_release_opto(&p->prp[k], s->sr, k ? 4u : 1u);
      else omx_env_program_release_auto(&p->prp[k], s->sr, k ? 4u : 1u);
    }
  }
  p->hold_frames = omx_dyn_hold_frames(hold, s->sr);
  p->floor_db = duck_on && duck_range < 0.0f ? duck_range : 0.0f;
  if (duck_on && !(duck_range < 0.0f)) p->gc.ratio = OMX_COMP_RATIO_MIN;
  OMX_POST(p->gc.ratio >= OMX_COMP_RATIO_MIN && p->gc.ratio <= OMX_COMP_RATIO_MAX &&
               p->gc.knee_db >= OMX_COMP_KNEE_DB_MIN && p->gc.knee_db <= OMX_COMP_KNEE_DB_MAX &&
               p->gc.thresh_db >= OMX_COMP_THRESHOLD_DB_MIN &&
               p->gc.thresh_db <= OMX_COMP_THRESHOLD_DB_MAX && p->gc.makeup_lin >= 1.0f &&
               p->attack_ms >= OMX_COMP_ATTACK_MS_MIN && p->attack_ms <= OMX_COMP_ATTACK_MS_MAX &&
               p->attack_coeff >= 0.0f && p->attack_coeff < 1.0f && p->release_coeff >= 0.0f &&
               p->release_coeff < 1.0f && p->dry >= 0.0f && p->dry <= 1.0f && p->floor_db <= 0.0f &&
               p->floor_db >= OMX_COMP_RANGE_DB_RANGE_MIN,
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Bind an instance to its rate (instantiate/activate): the slot's state cleared, base path,
 *        the atom resolved at the declared defaults (comp, so RMS; `auto`).
 * @param s The instance.
 * @param sr The host's sample rate, Hz.
 * @return 1 when usable, 0 when not (NULL instance, rate not positive or NaN); a refused instance
 *         is the identity.
 * @note No allocation; clears the whole instance. Thread-safe on distinct instances.
 */
static inline int omx_comp_instance_init(OmxCompInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 0.0f)) return 0;
  s->sr = sr;
  omx_dyn_state_init(&s->st, 1u);
  omx_tilt_design(&s->tilt, 1.0, OMX_COMP_INSTANCE_TILT_PIVOT_HZ, (double)sr);
  s->ready = 1;
  omx_comp_instance_resolve(s, 0, OMX_COMP_THRESHOLD_DB_DEFAULT, OMX_COMP_RATIO_DEFAULT,
                                OMX_COMP_KNEE_DB_DEFAULT, OMX_COMP_ATTACK_MS_DEFAULT,
                                OMX_COMP_RELEASE_MS_DEFAULT, OMX_COMP_MAKEUP_DB_DEFAULT, OMX_COMP_MIX_PCT_DEFAULT,
                                (int)OMX_COMP_KINDS_DEFAULT, (int)OMX_DETECTOR_OVERSAMPLINGS_DEFAULT,
                                (int)OMX_DETECTOR_TOPOLOGY_DEFAULT, (int)OMX_COMP_KEY_SOURCES_DEFAULT,
                                (float)OMX_COMP_KEY_HP_HZ_RANGE_DEFAULT, (float)OMX_COMP_KEY_LP_HZ_RANGE_DEFAULT,
                                (float)OMX_COMP_KEY_TILT_DB_RANGE_DEFAULT, (int)OMX_COMP_KEY_LISTENS_DEFAULT,
                                (float)OMX_COMP_HOLD_MS_RANGE_DEFAULT, (int)OMX_COMP_DETECT_LINKS_DEFAULT,
                                (int)OMX_COMP_DUCKS_DEFAULT, (float)OMX_COMP_RANGE_DB_RANGE_DEFAULT,
                                (int)OMX_COMP_RELEASE_MODES_DEFAULT);
  return 1;
}

/**
 * @brief The frames of latency the instance reports for its current atom.
 * @param s The instance.
 * @return OMX_OVS_LATENCY_4X while 4x is engaged, else 0; 0 for a refused instance.
 * @note RT-safe. Thread-safe: pure in `s`.
 */
static inline float omx_comp_instance_latency(const OmxCompInstance *s) {
  if (!s || !s->ready) return 0.0f;
  return omx_dyn_oversample_factor(&s->atom) == 4u ? (float)OMX_OVS_LATENCY_4X : 0.0f;
}

#define OMX_CONTRACT_STAGE "comp/instance-run"
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
static inline void omx_comp_instance_run(OmxCompInstance *s, const float *in_l,
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

/**
 * @brief 1 while the key path runs: a sidechain is listened to, a key filter edge is in, the key
 *        tilts, the fold is not `linked` or keyListen is on. At 0 the slot detects on its own legs.
 * @param s The instance.
 * @param key_l The sidechain's left (or mono) leg, or NULL (unconnected).
 * @return 1 or 0.
 * @note RT-safe. Thread-safe: pure in `s`.
 */
static inline int omx_comp_instance_keyed(const OmxCompInstance *s, const float *key_l) {
  return (s->src && key_l) || omx_gate_key_active(&s->filt[0]) || s->tilt.a != 1.0 ||
         s->fold != OMX_COMP_DETECT_LINKS_LINKED || s->listen;
}

#define OMX_CONTRACT_STAGE "comp/instance-key"
/**
 * @brief One chunk of the key path: the key legs filtered and tilted in the scratch (`ka`, `kb`), the
 *        detector's one signal folded into `kd`.
 * @param s The instance.
 * @param kl The key's left leg, `m` frames.
 * @param kr The key's right leg, `m` frames.
 * @param m Frames, at most OMX_COMP_INSTANCE_CHUNK.
 * @post `finite-key`.
 * @note RT-safe: two filter chains and a fold, no allocation. Thread-safe on distinct instances.
 */
static inline void omx_comp_instance_key(OmxCompInstance *s, const float *kl, const float *kr, uint32_t m) {
  memcpy(s->ka, kl, m * sizeof(float));
  memcpy(s->kb, kr, m * sizeof(float));
  omx_gate_key_run(&s->filt[0], s->ka, m);
  omx_gate_key_run(&s->filt[1], s->kb, m);
  if (s->tilt.a != 1.0)
    for (uint32_t i = 0; i < m; i++) {
      s->ka[i] = omx_tilt_tick(&s->tilt, s->ka[i], s->tilt_s[0]);
      s->kb[i] = omx_tilt_tick(&s->tilt, s->kb[i], s->tilt_s[1]);
    }
  for (uint32_t i = 0; i < m; i++) {
    const float a = s->ka[i], b = s->kb[i], mid = 0.5f * (a + b), side = 0.5f * (a - b);
    switch (s->fold) {
      case OMX_COMP_DETECT_LINKS_MID: s->kd[i] = mid; break;
      case OMX_COMP_DETECT_LINKS_SIDE: s->kd[i] = side; break;
      case OMX_COMP_DETECT_LINKS_MAX: s->kd[i] = fabsf(mid) > fabsf(side) ? fabsf(mid) : fabsf(side); break;
      default: s->kd[i] = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b); break;
    }
  }
  OMX_POST(omx_block_finite(s->kd, m), "finite-key");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "comp/instance-run-keyed"
/**
 * @brief The audio callback's whole share with a sidechain: the detector listens to the key path
 *        (see the header note), the gain lands on the strip's legs.
 * @param s The instance.
 * @param key_l The sidechain's left (or mono) leg, or NULL (unconnected: the strip's own legs).
 * @param key_r The sidechain's right leg, or NULL (a mono key, read on both legs).
 * @param in_l The left input block.
 * @param in_r The right input block.
 * @param out_l The left output block; may alias any input or key.
 * @param out_r The right output block; may alias any input or key.
 * @param n Frames.
 * @post `bypass-identity`: a bypassed slot leaves out equal to in.
 * @note RT-safe: no allocation, no lock. With the key path idle it is omx_comp_instance_run() bit
 *       for bit. Thread-safe on distinct instances.
 */
static inline void omx_comp_instance_run_keyed(OmxCompInstance *s, const float *key_l, const float *key_r,
                                               const float *in_l, const float *in_r, float *out_l, float *out_r,
                                               uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (!s || !s->ready || !s->atom.enabled || !omx_comp_instance_keyed(s, key_l)) {
    omx_comp_instance_run(s, in_l, in_r, out_l, out_r, n);
    return;
  }
  const int ext = s->src && key_l;
  for (uint32_t o = 0; o < n; o += OMX_COMP_INSTANCE_CHUNK) {
    const uint32_t m = n - o < OMX_COMP_INSTANCE_CHUNK ? n - o : OMX_COMP_INSTANCE_CHUNK;
    omx_comp_instance_key(s, ext ? key_l + o : in_l + o, ext ? (key_r ? key_r + o : key_l + o) : in_r + o, m);
    if (out_l != in_l) memmove(out_l + o, in_l + o, (size_t)m * sizeof(float));
    if (out_r != in_r) memmove(out_r + o, in_r + o, (size_t)m * sizeof(float));
    omx_dynamics_keyed(out_l + o, out_r + o, s->kd, m, &s->atom, &s->st);
    if (s->listen) {
      const int ms = s->fold == OMX_COMP_DETECT_LINKS_MID || s->fold == OMX_COMP_DETECT_LINKS_SIDE ||
                     s->fold == OMX_COMP_DETECT_LINKS_MAX;
      for (uint32_t i = 0; i < m; i++) {
        const float a = s->ka[i], b = s->kb[i], mid = 0.5f * (a + b), side = 0.5f * (a - b);
        out_l[o + i] = !ms ? a : s->fold == OMX_COMP_DETECT_LINKS_SIDE ? side : mid;
        out_r[o + i] = !ms ? b : s->fold == OMX_COMP_DETECT_LINKS_MID ? mid : side;
      }
    }
  }
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The keyLevelDb readout: the level the detector reads now, dBFS (omx_lin_to_db() of the
 *        envelope's level, the feedback loop's own stage while the loop ran).
 * @param s The instance.
 * @return The level, dBFS; the floor of omx_lin_to_db() for a refused instance or a silent detector.
 * @note RT-safe: one log10f. Thread-safe: reads only.
 */
static inline float omx_comp_instance_key_level_db(const OmxCompInstance *s) {
  if (!s || !s->ready) return omx_lin_to_db(0.0f);
  const struct omx_env_params ep = omx_dyn_env_params(&s->atom);
  const float fb = ep.detect == OMX_DETECT_RMS ? sqrtf(s->st.fb_env > 0.0f ? s->st.fb_env : 0.0f) : s->st.fb_env;
  return omx_lin_to_db(s->st.fb_ran ? fb : omx_env_level(&s->st.env, &ep));
}

/**
 * @brief The effectiveAttackMs readout: the attack the detector runs, ms. attackMs itself, but in the
 *        feedback topology where omx_dyn_fb_attack_pole() floors the loop's pole at `(R − 1)/R`: then
 *        the time constant of that floor at the instance's rate, `−1000/(sr·ln((R − 1)/R))`.
 * @param s The instance.
 * @return The attack, ms; 0 for a refused instance.
 * @note RT-safe: one logf when the floor is reached. Thread-safe: reads only.
 * @twin effectiveAttackMsOf
 */
static inline float omx_comp_instance_effective_attack_ms(const OmxCompInstance *s) {
  if (!s || !s->ready) return 0.0f;
  const struct omx_dyn *p = &s->atom;
  if (p->topology != OMX_DETECTOR_TOPOLOGY_FEEDBACK) return p->attack_ms;
  const float pole = omx_dyn_fb_attack_pole(p->attack_coeff, p->gc.ratio);
  return pole == p->attack_coeff ? p->attack_ms : -1000.0f / (s->sr * logf(pole));
}

#endif /* OMX_COMP_INSTANCE_H */
