// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_spring_instance.h
 * @brief A spring reverb: a delay, a chain of all-pass sections and a darkening loop, per leg.
 *
 * Each leg's loop is `w = x + g·LP(A(z)^N·z^−d·w)`, the wet being the loop's read: SPRING_SECTIONS
 * first-order all-pass sections (omx_allpass1) with their corner from the tension smear the return, the low
 * notes after the high ones, by `N·(1 − a²)/(1 + 2a·cos ω + a²)` samples. `d` is the length at the live
 * rate, the right leg's SPRING_RIGHT_RATIO longer. `g = 10^(−3·T/decay)`, `T` the loop's trip at DC with
 * the chain, so the decay is the same at every rate; SPRING_LOOP_MAX holds it under 1.
 *
 * Console use: an FX return on a guitar or vocal aux, the sound a desk does not otherwise have. A mix of
 * 0 and a bypass are the input, bit for bit, and move no state. LATENCY IS ZERO. Nothing is allocated;
 * every word of state is in {@link OmxSpringInstance}.
 */
#ifndef OMX_SPRING_INSTANCE_H
#define OMX_SPRING_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_instance_io.h>
#include <omxdsp/omx_allpass.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_SPRING_INSTANCE_CHANNELS 2
/** The ring: the longest spring at the top rate, the longer leg. */
#define OMX_SPRING_INSTANCE_RING 13100u
_Static_assert((double)OMX_SPRING_LENGTH_MS_RANGE_MAX * 192.0 * OMX_SPRING_RIGHT_RATIO_DOUBLE < (double)OMX_SPRING_INSTANCE_RING - 2.0,
               "the ring holds the longest spring at the top rate");
static const float OMX_SPRING_INSTANCE_CHIRP[OMX_SPRING_CHIRP_HZ_COUNT] = OMX_SPRING_CHIRP_HZ_INIT;
_Static_assert(OMX_SPRING_CHIRP_HZ_COUNT == 2u, "a corner at each end of the tension");

/** One leg's loop. */
struct omx_spring_leg {
  float ring[OMX_SPRING_INSTANCE_RING];
  struct omx_allpass1 chain[OMX_SPRING_SECTIONS];
  float lp;     /**< the loop's low-pass state */
  uint32_t pos; /**< the write position */
  uint32_t d;   /**< the delay in samples */
  float g;      /**< the loop gain */
};

typedef struct {
  float sr;
  struct omx_spring_leg leg[2];
  float a;   /**< the all-pass coefficient */
  float pole; /**< the loop low-pass's pole */
  float wet, dry;
  int enabled;
  int ready;
} OmxSpringInstance;

static inline void omx_spring_instance_resolve(OmxSpringInstance *s, int bypass, float decay_s, float length_ms,
                                               float tension_pct, float damp_hz, float mix_pct);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. It comes up at its declared defaults, a mix of 0. Allocates nothing.
 * @thread control */
static inline int omx_spring_instance_init(OmxSpringInstance *s, float sr) {
  if (!omx_instance_bind(s, sizeof(*s), sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  omx_spring_instance_resolve(s, 0, (float)OMX_SPRING_DECAY_S_RANGE_DEFAULT, (float)OMX_SPRING_LENGTH_MS_RANGE_DEFAULT,
                              (float)OMX_SPRING_TENSION_PCT_RANGE_DEFAULT, (float)OMX_SPRING_DAMP_HZ_RANGE_DEFAULT,
                              (float)OMX_SPRING_MIX_PCT_RANGE_DEFAULT);
  return 1;
}

/** The chain's group delay, samples, at angular frequency `w`, radians per sample, for coefficient `a`.
 * @thread audio */
static inline double omx_spring_chain_delay(double a, double w) {
  return (double)OMX_SPRING_SECTIONS * (1.0 - a * a) / (1.0 + 2.0 * a * cos(w) + a * a);
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the spring kernel's contract controls in their declared order, each clamped into its
 * travel, a non-finite one reading as its default. A length or tension change clears the tank, as does
 * the engage edge: the old tail is at another pitch.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "spring/instance-resolve"
static inline void omx_spring_instance_resolve(OmxSpringInstance *s, int bypass, float decay_s, float length_ms,
                                               float tension_pct, float damp_hz, float mix_pct) {
  if (!s || !s->ready) return;
  const float dc = omx_clamp_or(decay_s, (float)OMX_SPRING_DECAY_S_RANGE_MIN, (float)OMX_SPRING_DECAY_S_RANGE_MAX,
                                (float)OMX_SPRING_DECAY_S_RANGE_DEFAULT);
  const float ln = omx_clamp_or(length_ms, (float)OMX_SPRING_LENGTH_MS_RANGE_MIN, (float)OMX_SPRING_LENGTH_MS_RANGE_MAX,
                                (float)OMX_SPRING_LENGTH_MS_RANGE_DEFAULT);
  const float tn = omx_clamp_or(tension_pct, (float)OMX_SPRING_TENSION_PCT_RANGE_MIN, (float)OMX_SPRING_TENSION_PCT_RANGE_MAX,
                                (float)OMX_SPRING_TENSION_PCT_RANGE_DEFAULT);
  const float dp = omx_clamp_or(damp_hz, (float)OMX_SPRING_DAMP_HZ_RANGE_MIN, (float)OMX_SPRING_DAMP_HZ_RANGE_MAX,
                                (float)OMX_SPRING_DAMP_HZ_RANGE_DEFAULT);
  const float mx = omx_clamp_or(mix_pct, (float)OMX_SPRING_MIX_PCT_RANGE_MIN, (float)OMX_SPRING_MIX_PCT_RANGE_MAX,
                                (float)OMX_SPRING_MIX_PCT_RANGE_DEFAULT);
  const int was = s->enabled;
  const float fc = OMX_SPRING_INSTANCE_CHIRP[0] + (OMX_SPRING_INSTANCE_CHIRP[1] - OMX_SPRING_INSTANCE_CHIRP[0]) * tn * 0.01f;
  const double t = tan(M_PI * (double)fc / (double)s->sr);
  const float a = (float)((t - 1.0) / (t + 1.0));
  const uint32_t d0 = (uint32_t)lrint((double)ln * 0.001 * (double)s->sr);
  const uint32_t d1 = (uint32_t)lrint((double)ln * OMX_SPRING_RIGHT_RATIO_DOUBLE * 0.001 * (double)s->sr);
  const int moved = a != s->a || d0 != s->leg[0].d;
  const double chain = omx_spring_chain_delay((double)a, 0.0);
  for (int k = 0; k < 2; k++) {
    struct omx_spring_leg *l = &s->leg[k];
    l->d = k ? d1 : d0;
    const double g = pow(10.0, -3.0 * (((double)l->d + chain) / (double)s->sr) / (double)dc);
    l->g = (float)(g < OMX_SPRING_LOOP_MAX_DOUBLE ? g : OMX_SPRING_LOOP_MAX_DOUBLE);
  }
  s->a = a;
  s->pole = (float)exp(-2.0 * M_PI * (double)dp / (double)s->sr);
  s->wet = mx * 0.01f;
  s->dry = 1.0f - s->wet;
  s->enabled = !bypass && mx > 0.0f;
  if (s->enabled && (!was || moved)) {
    for (int k = 0; k < 2; k++) {
      struct omx_spring_leg *l = &s->leg[k];
      memset(l->ring, 0, sizeof l->ring);
      memset(l->chain, 0, sizeof l->chain);
      l->lp = 0.0f;
      l->pos = 0u;
    }
  }
  OMX_POST(s->leg[0].g > 0.0f && s->leg[0].g < 1.0f && s->leg[1].g > 0.0f && s->leg[1].g < 1.0f, "loop-under-unity");
}
#undef OMX_CONTRACT_STAGE

/** One leg, one sample: the loop's read, the chain and the low-pass, and the input written back. RT-safe.
 * @thread audio */
static inline float omx_spring_leg_tick(struct omx_spring_leg *l, float x, float a, float pole) {
  uint32_t rd = l->pos + OMX_SPRING_INSTANCE_RING - l->d;
  rd = rd >= OMX_SPRING_INSTANCE_RING ? rd - OMX_SPRING_INSTANCE_RING : rd;
  float y = l->ring[rd];
  for (uint32_t k = 0; k < OMX_SPRING_SECTIONS; k++) y = omx_allpass1_tick(&l->chain[k], y, a);
  l->lp = omx_flush(y + pole * (l->lp - y));
  l->ring[l->pos] = omx_flush(x + l->g * l->lp);
  l->pos = l->pos + 1u == OMX_SPRING_INSTANCE_RING ? 0u : l->pos + 1u;
  return y;
}

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: each leg through its spring, in to out. `in_*` and `out_*` may alias.
 * Not ready, bypassed or a mix of 0 is the identity and moves no state.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "spring/instance-run"
static inline void omx_spring_instance_run(OmxSpringInstance *s, const float *in_l, const float *in_r, float *out_l,
                                           float *out_r, uint32_t n) {
  if (!omx_instance_copy_through(in_l, in_r, out_l, out_r, n)) return;
  if (!s || !s->ready || !s->enabled) return;
  OMX_PRE(omx_block_pair_finite(out_l, out_r, n), "finite-in");
  const float a = s->a, pole = s->pole, wet = s->wet, dry = s->dry;
  for (uint32_t i = 0u; i != n; ++i) {
    const float xl = out_l[i], xr = out_r[i];
    const float wl = omx_spring_leg_tick(&s->leg[0], xl, a, pole), wr = omx_spring_leg_tick(&s->leg[1], xr, a, pole);
    out_l[i] = dry * xl + wet * wl;
    out_r[i] = dry * xr + wet * wr;
  }
  OMX_POST(omx_block_pair_finite(out_l, out_r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces: none.
 * @thread audio */
static inline uint32_t omx_spring_instance_latency(const OmxSpringInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_SPRING_INSTANCE_H */
