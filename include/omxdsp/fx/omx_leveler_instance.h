/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_leveler_instance.h — one-knob dynamics as a plugin instance: an amount, a direction and a speed
 * for a second engineer, a volunteer desk or a speech channel. It mints no DSP: it is two slots of the
 * dynamics kernel (omx_dynamics), a downward compressor and an upward one (omx_dyn's UPWARD law), both
 * on an RMS detector with LEVELER_KNEE_DB of knee, stereo-linked.
 *
 *   1. RESOLVE. {@link omx_leveler_instance_resolve} takes the leveler kernel's contract controls in
 *      their declared order (data/kernels/leveler.json). The amount reads the declared table: at each
 *      point of LEVELER_AMOUNT_POINTS a row of the downward slot's threshold, ratio and make-up
 *      (LEVELER_DOWN_*) and of the upward slot's threshold, ratio and largest boost (LEVELER_UP_*),
 *      linear between two points ({@link omx_leveler_table}). The speed picks an attack and a release
 *      from LEVELER_SPEED_ATTACK_MS and LEVELER_SPEED_RELEASE_MS. The direction runs the downward
 *      slot, the upward one, or both in series, downward first.
 *   2. RUN. Each engaged slot over both legs in place. An amount of 0 is the input, bit for bit, and
 *      moves no state.
 *
 * LATENCY IS ZERO: the slots never take the 4x control path. Nothing is allocated; every word of state
 * is in {@link OmxLevelerInstance}.
 */
#ifndef OMX_LEVELER_INSTANCE_H
#define OMX_LEVELER_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_LEVELER_INSTANCE_CHANNELS 2

/** The table: LEVELER_AMOUNT_POINTS and its rows, the speeds, the knee. */
static const float OMX_LEVELER_INSTANCE_POINTS[OMX_LEVELER_AMOUNT_POINTS_COUNT] = OMX_LEVELER_AMOUNT_POINTS_INIT;
static const float OMX_LEVELER_INSTANCE_DOWN_T[OMX_LEVELER_DOWN_THRESHOLD_DB_COUNT] = OMX_LEVELER_DOWN_THRESHOLD_DB_INIT;
static const float OMX_LEVELER_INSTANCE_DOWN_R[OMX_LEVELER_DOWN_RATIO_COUNT] = OMX_LEVELER_DOWN_RATIO_INIT;
static const float OMX_LEVELER_INSTANCE_DOWN_M[OMX_LEVELER_DOWN_MAKEUP_DB_COUNT] = OMX_LEVELER_DOWN_MAKEUP_DB_INIT;
static const float OMX_LEVELER_INSTANCE_UP_T[OMX_LEVELER_UP_THRESHOLD_DB_COUNT] = OMX_LEVELER_UP_THRESHOLD_DB_INIT;
static const float OMX_LEVELER_INSTANCE_UP_R[OMX_LEVELER_UP_RATIO_COUNT] = OMX_LEVELER_UP_RATIO_INIT;
static const float OMX_LEVELER_INSTANCE_UP_RANGE[OMX_LEVELER_UP_RANGE_DB_COUNT] = OMX_LEVELER_UP_RANGE_DB_INIT;
static const float OMX_LEVELER_INSTANCE_ATTACK[OMX_LEVELER_SPEED_ATTACK_MS_COUNT] = OMX_LEVELER_SPEED_ATTACK_MS_INIT;
static const float OMX_LEVELER_INSTANCE_RELEASE[OMX_LEVELER_SPEED_RELEASE_MS_COUNT] = OMX_LEVELER_SPEED_RELEASE_MS_INIT;
#define OMX_LEVELER_INSTANCE_KNEE ((float)OMX_LEVELER_KNEE_DB)
_Static_assert(OMX_LEVELER_DOWN_THRESHOLD_DB_COUNT == OMX_LEVELER_AMOUNT_POINTS_COUNT &&
                   OMX_LEVELER_DOWN_RATIO_COUNT == OMX_LEVELER_AMOUNT_POINTS_COUNT &&
                   OMX_LEVELER_DOWN_MAKEUP_DB_COUNT == OMX_LEVELER_AMOUNT_POINTS_COUNT &&
                   OMX_LEVELER_UP_THRESHOLD_DB_COUNT == OMX_LEVELER_AMOUNT_POINTS_COUNT &&
                   OMX_LEVELER_UP_RATIO_COUNT == OMX_LEVELER_AMOUNT_POINTS_COUNT &&
                   OMX_LEVELER_UP_RANGE_DB_COUNT == OMX_LEVELER_AMOUNT_POINTS_COUNT,
               "one row entry per amount point");
_Static_assert(OMX_LEVELER_SPEED_ATTACK_MS_COUNT == OMX_LEVELER_SPEEDS_COUNT &&
                   OMX_LEVELER_SPEED_RELEASE_MS_COUNT == OMX_LEVELER_SPEEDS_COUNT,
               "one attack and one release per speed");

/** One row of the table read at an amount. */
struct omx_leveler_row {
  float down_t, down_r, down_m; /**< the downward slot's threshold (dB), ratio and make-up (dB) */
  float up_t, up_r, up_range;   /**< the upward slot's threshold (dB), ratio and largest boost (dB) */
};

/**
 * The table read at `amount` (percent, inside LEVELER_AMOUNT_POINTS' span): each column linear between
 * the two points around it, exactly the row at a point. RT-safe: arithmetic. Pure.
 */
static inline struct omx_leveler_row omx_leveler_table(float amount) {
  uint32_t k = 0u;
  while (k + 2u < OMX_LEVELER_AMOUNT_POINTS_COUNT && amount > OMX_LEVELER_INSTANCE_POINTS[k + 1u]) k++;
  const float u = (amount - OMX_LEVELER_INSTANCE_POINTS[k]) / (OMX_LEVELER_INSTANCE_POINTS[k + 1u] - OMX_LEVELER_INSTANCE_POINTS[k]);
#define OMX_LEVELER_LERP(t) ((t)[k] + u * ((t)[k + 1u] - (t)[k]))
  const struct omx_leveler_row r = {OMX_LEVELER_LERP(OMX_LEVELER_INSTANCE_DOWN_T), OMX_LEVELER_LERP(OMX_LEVELER_INSTANCE_DOWN_R),
                                    OMX_LEVELER_LERP(OMX_LEVELER_INSTANCE_DOWN_M), OMX_LEVELER_LERP(OMX_LEVELER_INSTANCE_UP_T),
                                    OMX_LEVELER_LERP(OMX_LEVELER_INSTANCE_UP_R), OMX_LEVELER_LERP(OMX_LEVELER_INSTANCE_UP_RANGE)};
#undef OMX_LEVELER_LERP
  return r;
}

typedef struct {
  float sr;
  struct omx_dyn down, up;           /**< the two slots' atoms */
  struct omx_dyn_state down_st, up_st;
  float amt; /**< the amount as resolved, percent */
  int dir;   /**< the direction as resolved, an OMX_LEVELER_DIRECTIONS_* member */
  int spd;   /**< the speed as resolved, an OMX_LEVELER_SPEEDS_* member */
  int enabled;
  int ready;
} OmxLevelerInstance;

static inline void omx_leveler_instance_resolve(OmxLevelerInstance *s, int bypass, float amount, int direction, int speed);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. The leveler comes up at its declared defaults, an amount of 0. Allocates nothing. */
static inline int omx_leveler_instance_init(OmxLevelerInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  omx_dyn_state_init(&s->down_st, 1u);
  omx_dyn_state_init(&s->up_st, 1u);
  omx_leveler_instance_resolve(s, 0, (float)OMX_LEVELER_AMOUNT_RANGE_DEFAULT, (int)OMX_LEVELER_DIRECTIONS_DEFAULT,
                               (int)OMX_LEVELER_SPEEDS_DEFAULT);
  return 1;
}

/** One slot's atom: `mode`, the row's threshold, ratio, boost and make-up, the speed's poles. */
static inline void omx_leveler_slot(struct omx_dyn *d, int mode, float t, float r, float range, float makeup_db,
                                    float att_ms, float rel_ms, float sr) {
  memset(d, 0, sizeof(*d));
  d->enabled = 1;
  d->gc.mode = mode;
  d->gc.thresh_db = t;
  d->gc.ratio = r;
  d->gc.knee_db = OMX_LEVELER_INSTANCE_KNEE;
  d->gc.range_db = range;
  d->gc.makeup_lin = omx_db_to_lin(makeup_db);
  d->detect = OMX_DETECT_RMS;
  d->ovs_mode = OMX_DYN_OVS_OFF;
  d->attack_ms = att_ms;
  d->attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d->release_coeff = omx_pole_from_time_ms(rel_ms, sr);
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the leveler kernel's contract controls in their declared order: the amount (percent),
 * the direction (a LEVELER_DIRECTIONS index) and the speed (a LEVELER_SPEEDS index); an index outside
 * its set reads as its default.
 */
#define OMX_CONTRACT_STAGE "leveler/instance-resolve"
static inline void omx_leveler_instance_resolve(OmxLevelerInstance *s, int bypass, float amount, int direction, int speed) {
  if (!s || !s->ready) return;
  s->amt = omx_clamp_or(amount, (float)OMX_LEVELER_AMOUNT_RANGE_MIN, (float)OMX_LEVELER_AMOUNT_RANGE_MAX,
                        (float)OMX_LEVELER_AMOUNT_RANGE_DEFAULT);
  s->dir = direction >= 0 && direction < (int)OMX_LEVELER_DIRECTIONS_COUNT ? direction : (int)OMX_LEVELER_DIRECTIONS_DEFAULT;
  s->spd = speed >= 0 && speed < (int)OMX_LEVELER_SPEEDS_COUNT ? speed : (int)OMX_LEVELER_SPEEDS_DEFAULT;
  const struct omx_leveler_row row = omx_leveler_table(s->amt);
  const float att = OMX_LEVELER_INSTANCE_ATTACK[s->spd], rel = OMX_LEVELER_INSTANCE_RELEASE[s->spd];
  omx_leveler_slot(&s->down, OMX_DYN_ABOVE, row.down_t, row.down_r, 0.0f, row.down_m, att, rel, s->sr);
  omx_leveler_slot(&s->up, OMX_DYN_UPWARD, row.up_t, row.up_r, row.up_range, 0.0f, att, rel, s->sr);
  s->down.enabled = s->dir != OMX_LEVELER_DIRECTIONS_UP;
  s->up.enabled = s->dir != OMX_LEVELER_DIRECTIONS_DOWN;
  s->enabled = !bypass && s->amt > 0.0f;
  OMX_POST(s->down.gc.ratio >= 1.0f && s->up.gc.ratio >= 1.0f && s->up.gc.range_db >= 0.0f, "row-inside-the-laws");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the engaged slots over both legs, in to out. `in_*` and `out_*`
 * may alias. Not ready, bypassed or an amount of 0 is the identity and moves no state.
 */
static inline void omx_leveler_instance_run(OmxLevelerInstance *s, const float *in_l, const float *in_r, float *out_l,
                                            float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || !s->enabled) return;
  omx_dynamics(out_l, out_r, n, &s->down, &s->down_st);
  omx_dynamics(out_l, out_r, n, &s->up, &s->up_st);
}

/** The frames of latency the instance introduces: none. */
static inline uint32_t omx_leveler_instance_latency(const OmxLevelerInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_LEVELER_INSTANCE_H */
