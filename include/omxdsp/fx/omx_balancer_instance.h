// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_balancer_instance.h
 * @brief A spectral balancer: four wide bands, measured against a target tone, walked slowly toward it.
 *
 * The mono sum runs through each band's bandpass (BALANCER_BAND_HZ, BALANCER_Q); its power, averaged
 * over BALANCER_DETECT_MS, is the band's level `D[k]`, dB. The target is the bands' mean with the
 * spectrum's slope about their middle octave, the bands being constant-Q and so BALANCER_BAND_DB_OCT
 * wider each octave: `T[k] = mean(D) + (slope + B)·(log2(f[k]) − mean(log2 f))`. Each band
 * wants `−amount·(D[k] − T[k])`, inside ±rangeDb, and walks there at speedDbPerS on a peaking section
 * per leg. It measures ahead of the sections, so there is no loop, and the settled gains are the closed
 * form. Every 10 ms the gains step and the sections are designed; below BALANCER_GATE_DBFS, or until the
 * detector has settled, they hold.
 *
 * Console use: on MAIN or a group, a slow correction of the tone of the mix against the room's or the
 * programme's long-term shape; on a playback strip, tracks from different sources brought to one tone.
 * A band moves a few dB over seconds, so no note is chased. An amount of 0 and a bypass are the input, bit
 * for bit, and move no state. LATENCY IS ZERO: the sections are minimum phase. Nothing is allocated.
 */
#ifndef OMX_BALANCER_INSTANCE_H
#define OMX_BALANCER_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_instance_io.h>
#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_BALANCER_INSTANCE_CHANNELS 2
/** The bands. */
#define OMX_BALANCER_INSTANCE_BANDS OMX_BALANCER_BAND_HZ_COUNT
/** The ticks a second: every 10 ms. */
#define OMX_BALANCER_INSTANCE_TICKS_PER_S 100u
static const float OMX_BALANCER_INSTANCE_HZ[OMX_BALANCER_BAND_HZ_COUNT] = OMX_BALANCER_BAND_HZ_INIT;

typedef struct {
  float sr;
  float det[OMX_BALANCER_INSTANCE_BANDS][5];    /**< each band's bandpass */
  float det_s[OMX_BALANCER_INSTANCE_BANDS][4];  /**< its history */
  float sum[OMX_BALANCER_INSTANCE_BANDS];       /**< the squares of the running tick */
  float power[OMX_BALANCER_INSTANCE_BANDS];     /**< the averaged power */
  float eq[OMX_BALANCER_INSTANCE_BANDS][5];     /**< each band's peaking section */
  float eq_s[2][OMX_BALANCER_INSTANCE_BANDS][4]; /**< its history, per leg */
  float gain[OMX_BALANCER_INSTANCE_BANDS];      /**< each band's gain now, dB */
  float pole;     /**< the power average's pole per tick */
  float offset[OMX_BALANCER_INSTANCE_BANDS]; /**< each band's octaves from the bands' mean */
  float amount, range, step_db, slope; /**< the controls; step_db is the walk per tick */
  uint32_t tick, left, settled, settle;
  int enabled;
  int ready;
} OmxBalancerInstance;

static inline void omx_balancer_instance_resolve(OmxBalancerInstance *s, int bypass, float amount_pct, float range_db,
                                                 float speed_db_per_s, float slope_db_oct);

/** Each section flat and the detector empty: the engage edge.
 * @thread audio */
static inline void omx_balancer_instance_restart(OmxBalancerInstance *s) {
  memset(s->det_s, 0, sizeof s->det_s);
  memset(s->sum, 0, sizeof s->sum);
  memset(s->power, 0, sizeof s->power);
  memset(s->eq_s, 0, sizeof s->eq_s);
  for (uint32_t k = 0; k < OMX_BALANCER_INSTANCE_BANDS; k++) {
    s->gain[k] = 0.0f;
    omx_eq_design_f(OMX_EQ_PEAKING, (double)OMX_BALANCER_INSTANCE_HZ[k], OMX_BALANCER_Q_DOUBLE, 0.0, (double)s->sr, s->eq[k]);
  }
  s->left = s->tick;
  s->settled = 0u;
}

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. It comes up at its declared defaults, an amount of 0. Allocates nothing.
 * @thread control */
static inline int omx_balancer_instance_init(OmxBalancerInstance *s, float sr) {
  if (!omx_instance_bind(s, sizeof(*s), sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  s->tick = (uint32_t)sr / OMX_BALANCER_INSTANCE_TICKS_PER_S;
  const double tick_ms = 1000.0 / (double)OMX_BALANCER_INSTANCE_TICKS_PER_S;
  s->pole = (float)exp(-tick_ms / OMX_BALANCER_DETECT_MS);
  /* the detector has settled to 0.1 dB after 5 time constants */
  s->settle = (uint32_t)(5.0 * OMX_BALANCER_DETECT_MS / tick_ms);
  double mean = 0.0;
  for (uint32_t k = 0; k < OMX_BALANCER_INSTANCE_BANDS; k++) mean += log2((double)OMX_BALANCER_INSTANCE_HZ[k]);
  mean /= (double)OMX_BALANCER_INSTANCE_BANDS;
  for (uint32_t k = 0; k < OMX_BALANCER_INSTANCE_BANDS; k++) {
    s->offset[k] = (float)(log2((double)OMX_BALANCER_INSTANCE_HZ[k]) - mean);
    omx_eq_design_f(OMX_EQ_BANDPASS, (double)OMX_BALANCER_INSTANCE_HZ[k], OMX_BALANCER_Q_DOUBLE, 0.0, (double)sr, s->det[k]);
  }
  omx_balancer_instance_restart(s);
  omx_balancer_instance_resolve(s, 0, (float)OMX_BALANCER_AMOUNT_PCT_RANGE_DEFAULT, (float)OMX_BALANCER_RANGE_DB_RANGE_DEFAULT,
                                (float)OMX_BALANCER_SPEED_DB_PER_S_RANGE_DEFAULT, (float)OMX_BALANCER_SLOPE_DB_OCT_RANGE_DEFAULT);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the kernel's contract controls in their declared order, each clamped into its travel, a
 * non-finite one reading as its default. A face that comes back into use starts over.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "balancer/instance-resolve"
static inline void omx_balancer_instance_resolve(OmxBalancerInstance *s, int bypass, float amount_pct, float range_db,
                                                 float speed_db_per_s, float slope_db_oct) {
  if (!s || !s->ready) return;
  const float am = omx_clamp_or(amount_pct, (float)OMX_BALANCER_AMOUNT_PCT_RANGE_MIN, (float)OMX_BALANCER_AMOUNT_PCT_RANGE_MAX,
                                (float)OMX_BALANCER_AMOUNT_PCT_RANGE_DEFAULT);
  const float rg = omx_clamp_or(range_db, (float)OMX_BALANCER_RANGE_DB_RANGE_MIN, (float)OMX_BALANCER_RANGE_DB_RANGE_MAX,
                                (float)OMX_BALANCER_RANGE_DB_RANGE_DEFAULT);
  const float sp = omx_clamp_or(speed_db_per_s, (float)OMX_BALANCER_SPEED_DB_PER_S_RANGE_MIN,
                                (float)OMX_BALANCER_SPEED_DB_PER_S_RANGE_MAX, (float)OMX_BALANCER_SPEED_DB_PER_S_RANGE_DEFAULT);
  const float sl = omx_clamp_or(slope_db_oct, (float)OMX_BALANCER_SLOPE_DB_OCT_RANGE_MIN, (float)OMX_BALANCER_SLOPE_DB_OCT_RANGE_MAX,
                                (float)OMX_BALANCER_SLOPE_DB_OCT_RANGE_DEFAULT);
  const int was = s->enabled;
  s->amount = am * 0.01f;
  s->range = rg;
  s->step_db = sp / (float)OMX_BALANCER_INSTANCE_TICKS_PER_S;
  s->slope = sl;
  s->enabled = !bypass && am > 0.0f;
  if (s->enabled && !was) omx_balancer_instance_restart(s);
  OMX_POST(s->amount >= 0.0f && s->amount <= 1.0f && s->range > 0.0f && s->step_db > 0.0f, "controls-inside-the-travels");
}
#undef OMX_CONTRACT_STAGE

/** One tick's decision: average the power, read the target, step each gain, design what moved. RT-safe: the
 * designer's exp, cos, sin and sqrt, each, classified maths.
 * @thread audio */
static inline void omx_balancer_tick(OmxBalancerInstance *s) {
  float db[OMX_BALANCER_INSTANCE_BANDS], mean = 0.0f;
  for (uint32_t k = 0; k < OMX_BALANCER_INSTANCE_BANDS; k++) {
    const float p = s->sum[k] / (float)s->tick;
    s->sum[k] = 0.0f;
    s->power[k] = omx_flush(p + s->pole * (s->power[k] - p));
    db[k] = 10.0f * log10f(s->power[k] > 1e-20f ? s->power[k] : 1e-20f);
    mean += db[k];
  }
  mean /= (float)OMX_BALANCER_INSTANCE_BANDS;
  if (s->settled < s->settle) s->settled++;
  if (s->settled < s->settle || mean <= (float)OMX_BALANCER_GATE_DBFS) return;
  for (uint32_t k = 0; k < OMX_BALANCER_INSTANCE_BANDS; k++) {
    float want = -s->amount * (db[k] - (mean + (s->slope + (float)OMX_BALANCER_BAND_DB_OCT) * s->offset[k]));
    want = want > s->range ? s->range : want < -s->range ? -s->range : want;
    const float d = want - s->gain[k];
    const float next = d > s->step_db ? s->gain[k] + s->step_db : d < -s->step_db ? s->gain[k] - s->step_db : want;
    if (next != s->gain[k]) {
      s->gain[k] = next;
      omx_eq_design_f(OMX_EQ_PEAKING, (double)OMX_BALANCER_INSTANCE_HZ[k], OMX_BALANCER_Q_DOUBLE, (double)next, (double)s->sr, s->eq[k]);
    }
  }
}

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the detector over the mono sum, the sections over both legs, in to
 * out. `in_*` and `out_*` may alias. Not ready, bypassed or an amount of 0 is the identity and moves no state.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "balancer/instance-run"
static inline void omx_balancer_instance_run(OmxBalancerInstance *s, const float *in_l, const float *in_r, float *out_l,
                                             float *out_r, uint32_t n) {
  uint32_t o = 0u;
  if (!omx_instance_copy_through(in_l, in_r, out_l, out_r, n)) return;
  if (!s || !s->ready || !s->enabled) return;
  OMX_PRE(omx_block_pair_finite(out_l, out_r, n), "finite-in");
  while (o < n) {
    const uint32_t m = s->left < n - o ? s->left : n - o;
    for (uint32_t i = o; i < o + m; i++) {
      float l = out_l[i], r = out_r[i];
      const float mono = 0.5f * (l + r);
      for (uint32_t k = 0; k < OMX_BALANCER_INSTANCE_BANDS; k++) {
        const float b = omx_biquad(mono, s->det[k], s->det_s[k]);
        s->sum[k] += b * b;
        l = omx_biquad(l, s->eq[k], s->eq_s[0][k]);
        r = omx_biquad(r, s->eq[k], s->eq_s[1][k]);
      }
      out_l[i] = l;
      out_r[i] = r;
    }
    s->left -= m;
    o += m;
    if (s->left == 0u) {
      omx_balancer_tick(s);
      s->left = s->tick;
    }
  }
}
#undef OMX_CONTRACT_STAGE

/** The band<n>GainDb readouts: band `k`'s gain now, dB, `k` from 0.
 * @thread audio */
static inline float omx_balancer_instance_band_gain_db(const OmxBalancerInstance *s, uint32_t k) {
  return s && k < OMX_BALANCER_INSTANCE_BANDS ? s->gain[k] : 0.0f;
}

/** The frames of latency the instance introduces: none.
 * @thread audio */
static inline uint32_t omx_balancer_instance_latency(const OmxBalancerInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_BALANCER_INSTANCE_H */
