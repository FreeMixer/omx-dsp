/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_deesser_instance.h — the native de-esser as a plugin instance: the shell's core, with no
 * format in it. The same shape as omx_rotary_instance.h (one C core, N shells): the core is
 * `omx_deesser.h`'s `omx_deess_process`, THE SAME INLINE the console's de-esser stage runs (the
 * band-dynamics word, omx_band_dyn.h, with the detector on the band), and this file adds no DSP to
 * it. It adds what a host's port model needs and the atom does not carry:
 *
 *   1. RESOLVE. {@link omx_deesser_instance_resolve} takes the kernel's contract controls in their
 *      declared order and user units (Hz, octaves, dB, a ratio, dB, ms, ms, the DEESS_MODES
 *      index), clamps each into its declared travel (DEESS_*_RANGE, omx_contract_limits.h; a
 *      non-finite word reads as the declared default) and fills the atom as the console's
 *      resolve_fx_deess (mixer_rt.c) does: ABOVE, PEAK detection, unity make-up, no oversampling,
 *      the stage's knee (DEESS_KNEE_DB), the attack and release poles through
 *      omx_pole_from_time_ms. The detector band is the section the console's controller designs
 *      (core's `deEsserBandSection`): the cookbook bandpass at the frequency, its Q from the
 *      width by omx_eq_bandwidth_q, designed by omx_eq_design_f — only when the frequency or the
 *      width MOVED.
 *   2. IN -> OUT. The kernel works IN PLACE; run() copies in to out where they differ (they may
 *      alias) and runs the kernel on out. Identity when bypassed or not ready.
 *
 * Re-engaging from bypass keeps the state, as the console's stage does: a disabled de-esser
 * touches no state word, and it holds no buffered audio to replay. LATENCY IS ZERO at every rate
 * (omx_deess_latency). The state is plain inline floats; nothing is caller-owned or allocated.
 *
 * No mutable globals: every word of state is in the caller's {@link OmxDeesserInstance}.
 */
#ifndef OMX_DEESSER_INSTANCE_H
#define OMX_DEESSER_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_deesser.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_param.h>

#define OMX_DEESSER_INSTANCE_CHANNELS 2

/** Port defaults: the declaration's (DEESS_*_RANGE.default). The mode set declares no default;
 * the stage comes up at `split`, the set's first member. */
#define OMX_DEESSER_INSTANCE_FREQ_DEFAULT ((float)OMX_DEESS_FREQ_RANGE_DEFAULT)
#define OMX_DEESSER_INSTANCE_WIDTH_DEFAULT ((float)OMX_DEESS_WIDTH_RANGE_DEFAULT)
#define OMX_DEESSER_INSTANCE_THRESHOLD_DEFAULT ((float)OMX_DEESS_THRESHOLD_RANGE_DEFAULT)
#define OMX_DEESSER_INSTANCE_RATIO_DEFAULT ((float)OMX_DEESS_RATIO_RANGE_DEFAULT)
#define OMX_DEESSER_INSTANCE_RANGE_DEFAULT ((float)OMX_DEESS_RANGE_RANGE_DEFAULT)
#define OMX_DEESSER_INSTANCE_ATTACK_DEFAULT ((float)OMX_DEESS_ATTACK_RANGE_DEFAULT)
#define OMX_DEESSER_INSTANCE_RELEASE_DEFAULT ((float)OMX_DEESS_RELEASE_RANGE_DEFAULT)
#define OMX_DEESSER_INSTANCE_MODE_DEFAULT OMX_DEESS_SPLIT

/** One instance: the atom the host's ports resolve to and the kernel's inline state. */
typedef struct {
  float sr;
  struct omx_deess atom;
  struct omx_deess_state state;
  /** The clamped frequency and width the band was last designed from; `designed` 0 until the first. */
  float freq, width;
  int designed;
  int ready;
} OmxDeesserInstance;

/**
 * Bind an instance to its rate. Returns 1 when usable, 0 when not (a refused init leaves `ready`
 * clear and run() is the identity): the rate must be a declared one. Allocates nothing.
 */
static inline int omx_deesser_instance_init(OmxDeesserInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_declared(sr)) return 0;
  omx_deess_state_init(&s->state);
  s->sr = sr;
  s->atom.enabled = 0;
  s->ready = 1;
  return 1;
}

/**
 * Resolve the host's control-port values into the kernel's atom for one cycle. `bypass` non-zero
 * disables the atom. The arguments are the kernel's contract controls, in their declared order and
 * user units: `deess_freq` (Hz), `deess_width` (octaves), `deess_threshold` (dB), `deess_ratio`,
 * `deess_range` (dB, an attenuation), `deess_attack` and `deess_release` (ms), each clamped into
 * its declared travel, a non-finite word reading as the declared default; `deess_mode` is the
 * DEESS_MODES index, an `enum omx_deess_mode` member (any other value reads as `split`).
 */
#define OMX_CONTRACT_STAGE "deesser/instance-resolve"
static inline void omx_deesser_instance_resolve(OmxDeesserInstance *s, int bypass, float deess_freq,
                                                float deess_width, float deess_threshold, float deess_ratio,
                                                float deess_range, float deess_attack, float deess_release,
                                                int deess_mode) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). The atom this leaves behind satisfies every PRE
   * omx_deess_process states, whatever the host's ports held. */
  deess_freq = omx_clamp_or(deess_freq, (float)OMX_DEESS_FREQ_RANGE_MIN, (float)OMX_DEESS_FREQ_RANGE_MAX,
                            OMX_DEESSER_INSTANCE_FREQ_DEFAULT);
  deess_width = omx_clamp_or(deess_width, (float)OMX_DEESS_WIDTH_RANGE_MIN, (float)OMX_DEESS_WIDTH_RANGE_MAX,
                             OMX_DEESSER_INSTANCE_WIDTH_DEFAULT);
  deess_threshold = omx_clamp_or(deess_threshold, (float)OMX_DEESS_THRESHOLD_RANGE_MIN,
                                 (float)OMX_DEESS_THRESHOLD_RANGE_MAX, OMX_DEESSER_INSTANCE_THRESHOLD_DEFAULT);
  deess_ratio = omx_clamp_or(deess_ratio, (float)OMX_DEESS_RATIO_RANGE_MIN, (float)OMX_DEESS_RATIO_RANGE_MAX,
                             OMX_DEESSER_INSTANCE_RATIO_DEFAULT);
  deess_range = omx_clamp_or(deess_range, (float)OMX_DEESS_RANGE_RANGE_MIN, (float)OMX_DEESS_RANGE_RANGE_MAX,
                             OMX_DEESSER_INSTANCE_RANGE_DEFAULT);
  deess_attack = omx_clamp_or(deess_attack, (float)OMX_DEESS_ATTACK_RANGE_MIN,
                              (float)OMX_DEESS_ATTACK_RANGE_MAX, OMX_DEESSER_INSTANCE_ATTACK_DEFAULT);
  deess_release = omx_clamp_or(deess_release, (float)OMX_DEESS_RELEASE_RANGE_MIN,
                               (float)OMX_DEESS_RELEASE_RANGE_MAX, OMX_DEESSER_INSTANCE_RELEASE_DEFAULT);
  if (deess_mode != OMX_DEESS_SPLIT && deess_mode != OMX_DEESS_WIDEBAND) deess_mode = OMX_DEESSER_INSTANCE_MODE_DEFAULT;
  struct omx_deess *o = &s->atom;
  o->enabled = bypass ? 0 : 1;
  o->mode = deess_mode;
  o->dyn.enabled = 1;
  o->dyn.gc.mode = OMX_DYN_ABOVE;
  o->dyn.detect = OMX_DETECT_PEAK;
  o->dyn.gc.makeup_lin = 1.0f;
  o->dyn.ovs_mode = OMX_DYN_OVS_OFF;
  o->dyn.gc.thresh_db = deess_threshold;
  o->dyn.gc.ratio = deess_ratio;
  o->dyn.gc.knee_db = (float)OMX_DEESS_KNEE_DB;
  o->dyn.gc.range_db = deess_range;
  o->dyn.attack_ms = deess_attack;
  o->dyn.attack_coeff = omx_pole_from_time_ms(deess_attack, s->sr);
  o->dyn.release_coeff = omx_pole_from_time_ms(deess_release, s->sr);
  if (!s->designed || deess_freq != s->freq || deess_width != s->width) {
    const double q = omx_eq_bandwidth_q((double)deess_freq, (double)deess_width, (double)s->sr);
    omx_eq_design_f(OMX_EQ_BANDPASS, (double)deess_freq, q, 0.0, (double)s->sr, o->bp_c);
    s->freq = deess_freq;
    s->width = deess_width;
    s->designed = 1;
  }
  OMX_POST(o->dyn.gc.range_db <= 0.0f && o->dyn.gc.ratio >= 1.0f && o->dyn.gc.mode == OMX_DYN_ABOVE &&
               (o->mode == OMX_DEESS_SPLIT || o->mode == OMX_DEESS_WIDEBAND),
           "atom-meets-the-kernel-preconditions");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel
 * in place on out. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "deesser/instance-run"
static inline void omx_deesser_instance_run(OmxDeesserInstance *s, const float *in_l, const float *in_r,
                                            float *out_l, float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_deess_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces: the kernel's, zero at every rate. */
static inline uint32_t omx_deesser_instance_latency(const OmxDeesserInstance *s) {
  return s ? (uint32_t)omx_deess_latency(&s->atom) : 0u;
}

#endif /* OMX_DEESSER_INSTANCE_H */
