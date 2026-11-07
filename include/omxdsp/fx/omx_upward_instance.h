// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_upward_instance.h
 * @brief An upward compressor as a plugin instance: what is quiet comes up, what is loud is left alone.
 *
 * It mints no DSP: one slot of the dynamics kernel (omx_dynamics) on omx_dyn's UPWARD law, an RMS
 * detector, stereo-linked, UPWARD_KNEE_DB of knee: the lift is `(1 − 1/R)(T − L)` under the threshold,
 * capped at the range. A ratio of 1 or a range of 0 is the input, bit for bit, and moves no state. A slot
 * that comes back into use starts from silence.
 *
 * Console use: a talk or a podcast strip, a lavalier that sits low, a quiet speaker on a conference
 * bus: the detail rises and the peaks stay where they are. LATENCY IS ZERO. Nothing is allocated; every
 * word of state is in {@link OmxUpwardInstance}.
 */
#ifndef OMX_UPWARD_INSTANCE_H
#define OMX_UPWARD_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_instance_io.h>
#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_UPWARD_INSTANCE_CHANNELS 2

typedef struct {
  float sr;
  struct omx_dyn atom;
  struct omx_dyn_state st;
  float gain_db; /**< the gainDb readout */
  int enabled;
  int ready;
} OmxUpwardInstance;

static inline void omx_upward_instance_resolve(OmxUpwardInstance *s, int bypass, float threshold_db, float ratio,
                                               float range_db, float attack_ms, float release_ms);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. It comes up at its declared defaults, a ratio of 1. Allocates nothing.
 * @thread control */
static inline int omx_upward_instance_init(OmxUpwardInstance *s, float sr) {
  if (!omx_instance_bind(s, sizeof(*s), sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  omx_dyn_state_init(&s->st, 1u);
  omx_upward_instance_resolve(s, 0, (float)OMX_UPWARD_THRESHOLD_DB_RANGE_DEFAULT, (float)OMX_UPWARD_RATIO_RANGE_DEFAULT,
                              (float)OMX_UPWARD_RANGE_DB_RANGE_DEFAULT, (float)OMX_UPWARD_ATTACK_MS_RANGE_DEFAULT,
                              (float)OMX_UPWARD_RELEASE_MS_RANGE_DEFAULT);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the upward kernel's contract controls in their declared order, each clamped into its
 * travel, a non-finite one reading as its default.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "upward/instance-resolve"
static inline void omx_upward_instance_resolve(OmxUpwardInstance *s, int bypass, float threshold_db, float ratio,
                                               float range_db, float attack_ms, float release_ms) {
  if (!s || !s->ready) return;
  const float th = omx_clamp_or(threshold_db, (float)OMX_UPWARD_THRESHOLD_DB_RANGE_MIN,
                                (float)OMX_UPWARD_THRESHOLD_DB_RANGE_MAX, (float)OMX_UPWARD_THRESHOLD_DB_RANGE_DEFAULT);
  const float ra = omx_clamp_or(ratio, (float)OMX_UPWARD_RATIO_RANGE_MIN, (float)OMX_UPWARD_RATIO_RANGE_MAX,
                                (float)OMX_UPWARD_RATIO_RANGE_DEFAULT);
  const float rg = omx_clamp_or(range_db, (float)OMX_UPWARD_RANGE_DB_RANGE_MIN, (float)OMX_UPWARD_RANGE_DB_RANGE_MAX,
                                (float)OMX_UPWARD_RANGE_DB_RANGE_DEFAULT);
  const float at = omx_clamp_or(attack_ms, (float)OMX_UPWARD_ATTACK_MS_RANGE_MIN, (float)OMX_UPWARD_ATTACK_MS_RANGE_MAX,
                                (float)OMX_UPWARD_ATTACK_MS_RANGE_DEFAULT);
  const float rl = omx_clamp_or(release_ms, (float)OMX_UPWARD_RELEASE_MS_RANGE_MIN, (float)OMX_UPWARD_RELEASE_MS_RANGE_MAX,
                                (float)OMX_UPWARD_RELEASE_MS_RANGE_DEFAULT);
  const int was = s->enabled;
  struct omx_dyn *d = &s->atom;
  memset(d, 0, sizeof(*d));
  d->enabled = 1;
  d->gc.mode = OMX_DYN_UPWARD;
  d->gc.thresh_db = th;
  d->gc.ratio = ra;
  d->gc.knee_db = (float)OMX_UPWARD_KNEE_DB;
  d->gc.range_db = rg;
  d->gc.makeup_lin = 1.0f;
  d->detect = OMX_DETECT_RMS;
  d->ovs_mode = OMX_DYN_OVS_OFF;
  d->attack_ms = at;
  d->attack_coeff = omx_pole_from_time_ms(at, s->sr);
  d->release_coeff = omx_pole_from_time_ms(rl, s->sr);
  s->enabled = !bypass && ra > 1.0f && rg > 0.0f;
  if (s->enabled && !was) omx_dyn_state_init(&s->st, 1u);
  OMX_POST(d->gc.ratio >= 1.0f && d->gc.range_db >= 0.0f, "atom-inside-the-law");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the slot over both legs, in to out. `in_*` and `out_*` may alias.
 * Not ready, bypassed, a ratio of 1 or a range of 0 is the identity and moves no state.
 * @thread audio
 */
static inline void omx_upward_instance_run(OmxUpwardInstance *s, const float *in_l, const float *in_r, float *out_l,
                                           float *out_r, uint32_t n) {
  if (!omx_instance_copy_through(in_l, in_r, out_l, out_r, n)) return;
  if (!s || !s->ready) return;
  s->gain_db = 0.0f;
  if (!s->enabled) return;
  float before = 0.0f, after = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    const float m = fmaxf(fabsf(out_l[i]), fabsf(out_r[i]));
    before = m > before ? m : before;
  }
  omx_dynamics(out_l, out_r, n, &s->atom, &s->st);
  for (uint32_t i = 0; i < n; i++) {
    const float m = fmaxf(fabsf(out_l[i]), fabsf(out_r[i]));
    after = m > after ? m : after;
  }
  s->gain_db = before > 0.0f && after > before ? omx_lin_to_db(after / before) : 0.0f;
}

/** The gainDb readout: what the last block's loudest sample rose by, dB.
 * @thread audio */
static inline float omx_upward_instance_gain_db(const OmxUpwardInstance *s) { return s ? s->gain_db : 0.0f; }

/** The frames of latency the instance introduces: none.
 * @thread audio */
static inline uint32_t omx_upward_instance_latency(const OmxUpwardInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_UPWARD_INSTANCE_H */
