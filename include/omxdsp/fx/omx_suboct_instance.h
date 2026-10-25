/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_suboct_instance.h — the sub-octaver as a plugin instance (the FX-17 design): a
 * zero-crossing divider armed by hysteresis and fired at the crossing, gating a low-passed copy. It mints
 * no word: the divider is omx_divider, the bands the EQ's own sections (omx_eq_design), the level the
 * dynamics envelope (omx_env_step, peak).
 *
 *   x_t = LP12(HP12(fold; lo/2); hi)                  the tracking band of the legs' fold (L + R)/2
 *   E   = peak envelope of |x_t|                      SUBOCT_ENV_ATTACK_MS, SUBOCT_ENV_RELEASE_MS
 *   s1  = LP24(D(x_t; SUBOCT_HYSTERESIS E); post1)    the octave below
 *   s2  = LP24(D(s1; SUBOCT_HYSTERESIS E1); post2)    two below: the first octave divided again
 *   y   = dry x + G (oct1 s1 + oct2 s2)               on each leg, G the gate's ramp
 *
 * lo, hi, post1 and post2 are the range's row of SUBOCT_RANGE_LO_HZ, SUBOCT_RANGE_HI_HZ,
 * SUBOCT_POST1_HZ and SUBOCT_POST2_HZ. G moves toward 1 while E is at or over trackGateDb and toward
 * 0 under it, by 1/SUBOCT_GATE_RAMP_MS a millisecond; at 0 the octaves are exactly gone. The divider
 * toggles once per input period, so trackedHz is the rate over the samples between its last two
 * toggles, 0 while gated. Both octaves at 0 and the dry at 100 is the input bit for bit, the divider
 * still tracking, so raising an octave starts warm.
 *
 * LATENCY IS ZERO: the dry is frame-aligned; the sub's own lag is the effect's, not a latency. Nothing
 * is allocated; every word of state is in {@link OmxSuboctInstance}.
 */
#ifndef OMX_SUBOCT_INSTANCE_H
#define OMX_SUBOCT_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_divider.h>
#include <omxdsp/omx_envelope.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_SUBOCT_INSTANCE_CHANNELS 2

/** The range table, a row per SUBOCT_RANGES member. */
static const float OMX_SUBOCT_INSTANCE_LO[OMX_SUBOCT_RANGE_LO_HZ_COUNT] = OMX_SUBOCT_RANGE_LO_HZ_INIT;
static const float OMX_SUBOCT_INSTANCE_HI[OMX_SUBOCT_RANGE_HI_HZ_COUNT] = OMX_SUBOCT_RANGE_HI_HZ_INIT;
static const float OMX_SUBOCT_INSTANCE_POST1[OMX_SUBOCT_POST1_HZ_COUNT] = OMX_SUBOCT_POST1_HZ_INIT;
static const float OMX_SUBOCT_INSTANCE_POST2[OMX_SUBOCT_POST2_HZ_COUNT] = OMX_SUBOCT_POST2_HZ_INIT;
_Static_assert(OMX_SUBOCT_RANGE_LO_HZ_COUNT == OMX_SUBOCT_RANGES_COUNT && OMX_SUBOCT_RANGE_HI_HZ_COUNT == OMX_SUBOCT_RANGES_COUNT &&
                   OMX_SUBOCT_POST1_HZ_COUNT == OMX_SUBOCT_RANGES_COUNT && OMX_SUBOCT_POST2_HZ_COUNT == OMX_SUBOCT_RANGES_COUNT,
               "one row per range");
/** The envelope's times, ms: SUBOCT_ENV_ATTACK_MS and SUBOCT_ENV_RELEASE_MS. */
#define OMX_SUBOCT_INSTANCE_ENV ((float)OMX_SUBOCT_ENV_ATTACK_MS), ((float)OMX_SUBOCT_ENV_RELEASE_MS)

/** The divider's arming level for a tracked peak `e`: SUBOCT_HYSTERESIS e. */
static inline float omx_suboct_arm_level(float e) { return OMX_SUBOCT_HYSTERESIS * e; }

typedef struct {
  float sr;
  int rng;                      /**< the range as resolved, an OMX_SUBOCT_RANGES_* member */
  float o1, o2, dr, gate_db;    /**< the levels (shares) and the gate (dBFS) as resolved */
  float gate_lin;               /**< the gate, linear */
  float ramp_step;              /**< the gate ramp's step a sample */
  double hp[5], lp[5];          /**< the tracking band */
  double p1[2][5], p2[2][5];    /**< the octaves' low-passes, two sections each */
  struct omx_env_params ep;     /**< the envelope's poles, peak */
  float ac, rc;                 /**< its per-stage poles */
  /* state */
  double hs[4], ls[4], p1s[2][4], p2s[2][4];
  struct omx_env e0, e1;
  struct omx_divider d1, d2;
  float g;                      /**< the gate's ramp, in [0, 1] */
  uint64_t n;                   /**< samples run */
  uint64_t last_toggle;         /**< the sample of the first divider's last toggle */
  uint32_t period;              /**< samples between its last two toggles; 0 until two */
  int enabled;
  int ready;
} OmxSuboctInstance;

static inline void omx_suboct_instance_resolve(OmxSuboctInstance *s, int bypass, int range, float oct1, float oct2, float dry,
                                               float track_gate_db);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. The sub-octaver comes up at its declared defaults. Allocates nothing. */
static inline int omx_suboct_instance_init(OmxSuboctInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  s->rng = -1;
  omx_divider_init(&s->d1);
  omx_divider_init(&s->d2);
  omx_suboct_instance_resolve(s, 0, (int)OMX_SUBOCT_RANGES_DEFAULT, (float)OMX_SUBOCT_OCT1_RANGE_DEFAULT,
                              (float)OMX_SUBOCT_OCT2_RANGE_DEFAULT, (float)OMX_SUBOCT_DRY_RANGE_DEFAULT,
                              (float)OMX_SUBOCT_TRACK_GATE_DB_RANGE_DEFAULT);
  return 1;
}

/** Two sections of the 24 dB Butterworth low-pass at `hz`. */
static inline void omx_suboct_lp24(double c[2][5], double hz, double sr) {
  double q[2];
  omx_eq_butterworth_qs(1, q);
  omx_eq_design(OMX_EQ_LOWPASS, hz, q[0], 0.0, sr, c[0]);
  omx_eq_design(OMX_EQ_LOWPASS, hz, q[1], 0.0, sr, c[1]);
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the suboct kernel's contract controls in their declared order: the range (a
 * SUBOCT_RANGES index), oct1, oct2 and dry (percent) and trackGateDb (dBFS).
 */
#define OMX_CONTRACT_STAGE "suboct/instance-resolve"
static inline void omx_suboct_instance_resolve(OmxSuboctInstance *s, int bypass, int range, float oct1, float oct2, float dry,
                                               float track_gate_db) {
  if (!s || !s->ready) return;
  const int range_row = range >= 0 && range < (int)OMX_SUBOCT_RANGES_COUNT ? range : (int)OMX_SUBOCT_RANGES_DEFAULT;
  s->o1 = omx_clamp_or(oct1, (float)OMX_SUBOCT_OCT1_RANGE_MIN, (float)OMX_SUBOCT_OCT1_RANGE_MAX,
                       (float)OMX_SUBOCT_OCT1_RANGE_DEFAULT) * 0.01f;
  s->o2 = omx_clamp_or(oct2, (float)OMX_SUBOCT_OCT2_RANGE_MIN, (float)OMX_SUBOCT_OCT2_RANGE_MAX,
                       (float)OMX_SUBOCT_OCT2_RANGE_DEFAULT) * 0.01f;
  s->dr = omx_clamp_or(dry, (float)OMX_SUBOCT_DRY_RANGE_MIN, (float)OMX_SUBOCT_DRY_RANGE_MAX, (float)OMX_SUBOCT_DRY_RANGE_DEFAULT) *
          0.01f;
  s->gate_db = omx_clamp_or(track_gate_db, (float)OMX_SUBOCT_TRACK_GATE_DB_RANGE_MIN, (float)OMX_SUBOCT_TRACK_GATE_DB_RANGE_MAX,
                            (float)OMX_SUBOCT_TRACK_GATE_DB_RANGE_DEFAULT);
  s->gate_lin = omx_db_to_lin(s->gate_db);
  s->ramp_step = 1.0f / ((float)OMX_SUBOCT_GATE_RAMP_MS * 0.001f * s->sr);
  if (range_row != s->rng) {
    /* the range's corners, designed on a change of range only; the sections keep their history */
    omx_eq_design(OMX_EQ_HIGHPASS, 0.5 * OMX_SUBOCT_INSTANCE_LO[range_row], 0.70710678118654752, 0.0, s->sr, s->hp);
    omx_eq_design(OMX_EQ_LOWPASS, OMX_SUBOCT_INSTANCE_HI[range_row], 0.70710678118654752, 0.0, s->sr, s->lp);
    omx_suboct_lp24(s->p1, OMX_SUBOCT_INSTANCE_POST1[range_row], s->sr);
    omx_suboct_lp24(s->p2, OMX_SUBOCT_INSTANCE_POST2[range_row], s->sr);
    s->ep = (struct omx_env_params){omx_pole_from_time_ms((float)OMX_SUBOCT_ENV_ATTACK_MS, s->sr),
                                    omx_pole_from_time_ms((float)OMX_SUBOCT_ENV_RELEASE_MS, s->sr), OMX_DETECT_PEAK};
    omx_env_stage_poles(&s->ep, 1u, &s->ac, &s->rc);
    s->rng = range_row;
  }
  s->enabled = bypass ? 0 : 1;
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the legs' fold through the tracking band, the dividers and the
 * octaves' low-passes, the gated octaves added to each leg over the dry, in to out. `in_*` and `out_*`
 * may alias. Not ready or bypassed is the identity and moves no state.
 */
#define OMX_CONTRACT_STAGE "suboct/instance-run"
static inline void omx_suboct_instance_run(OmxSuboctInstance *s, const float *in_l, const float *in_r, float *out_l,
                                           float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || !s->enabled) return;
  OMX_PRE(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-in");
  const int wire = s->o1 == 0.0f && s->o2 == 0.0f && s->dr == 1.0f;
  for (uint32_t i = 0; i < n; i++, s->n++) {
    const float x = 0.5f * (out_l[i] + out_r[i]);
    const float xt = omx_biquad_d(omx_biquad_d(x, s->hp, s->hs), s->lp, s->ls);
    const float e = omx_env_step(&s->e0, &s->ep, fabsf(xt), s->ac, s->rc);
    const uint32_t t0 = s->d1.toggles;
    float s1 = omx_divider_step(&s->d1, xt, omx_suboct_arm_level(e));
    if (s->d1.toggles != t0) {
      s->period = s->n > s->last_toggle && s->last_toggle != 0u ? (uint32_t)(s->n - s->last_toggle) : 0u;
      s->last_toggle = s->n;
    }
    s1 = omx_biquad_d(omx_biquad_d(s1, s->p1[0], s->p1s[0]), s->p1[1], s->p1s[1]);
    const float e1 = omx_env_step(&s->e1, &s->ep, fabsf(s1), s->ac, s->rc);
    float s2 = omx_divider_step(&s->d2, s1, omx_suboct_arm_level(e1));
    s2 = omx_biquad_d(omx_biquad_d(s2, s->p2[0], s->p2s[0]), s->p2[1], s->p2s[1]);
    const float target = e >= s->gate_lin ? 1.0f : 0.0f;
    s->g = s->g < target ? fminf(target, s->g + s->ramp_step) : fmaxf(target, s->g - s->ramp_step);
    if (wire) continue;
    const float sub = s->g > 0.0f ? s->g * (s->o1 * s1 + s->o2 * s2) : 0.0f;
    out_l[i] = s->dr * out_l[i] + sub;
    out_r[i] = s->dr * out_r[i] + sub;
  }
  for (int k = 0; k < OMX_DYN_ENV_STAGES; k++) s->e0.stage[k] = omx_flush(s->e0.stage[k]), s->e1.stage[k] = omx_flush(s->e1.stage[k]);
  OMX_POST(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/** The trackedHz readout: the rate over the samples between the divider's last two toggles; 0 while
 * gated or before two toggles. */
static inline float omx_suboct_instance_tracked_hz(const OmxSuboctInstance *s) {
  return s && s->ready && s->g > 0.0f && s->period > 0u ? s->sr / (float)s->period : 0.0f;
}

/** The frames of latency the instance introduces: none. */
static inline uint32_t omx_suboct_instance_latency(const OmxSuboctInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_SUBOCT_INSTANCE_H */
