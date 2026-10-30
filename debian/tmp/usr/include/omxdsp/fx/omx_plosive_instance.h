/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_plosive_instance.h — the plosive and consonant tamer as a plugin instance (the FX-13 design): a
 * transient detector listening to two bands and cutting only them. It mints no
 * word: the detector is omx_envdiff_step with the transient designer's fast follower and its ramp
 * omx_envdiff_ramp at an offset, the pop split is omx_allpass1's power-complementary pair, the
 * consonant band is the de-esser's cookbook bandpass in its split topology.
 *
 *   pop        LP = (x + A x)/2, A the first-order allpass at popHz; dP the legs' 0.5/0.5 fold of |LP|;
 *              gP = 10^(-popDb ramp(dP's contrast, popSensitivityDb)/20); x1 = x - (1 - gP) LP
 *   consonant  B the bandpass of x1 at bandHz, bandWidthOct; dC the fold of |B|;
 *              gC = 10^(-consonantDb ramp(dC's contrast, sensitivityDb)/20); y = x1 + (gC - 1) B
 *
 * Each half is bounded by 1 at every frequency (|((1 + g) + (g - 1) A)/2| <= 1 and the de-esser's
 * split), so no gain is added (L4). A gain of exactly 1 takes the half's unity branch, so a sustained
 * vowel whose contrast stays under the sensitivity passes bit for bit (L2), and consonantDb = popDb = 0
 * is the input bit for bit with the detectors running (L5). The pop half runs first so the consonant
 * detector reads the voice without the pop's spill. Both legs share each detector, so the image never
 * shifts.
 *
 *   RESOLVE. {@link omx_plosive_instance_resolve} takes the plosive kernel's contract controls in their
 *   declared order (data/kernels/plosive.json), each clamped into its travel, a non-finite word reading
 *   as its default. The band's edges are bandHz / 2^(w/2) and bandHz 2^(w/2), the top one held under
 *   PLOSIVE_BAND_EDGE_FRACTION of the rate, and the section is designed from the held edges.
 *   RUN. Per sample as above; the deepest cut of each half in the block is its readout
 *   ({@link omx_plosive_instance_consonant_reduction_db}, {@link omx_plosive_instance_pop_reduction_db}).
 *
 * LATENCY IS ZERO (L1). Nothing is allocated; every word of state is in {@link OmxPlosiveInstance}.
 */
#ifndef OMX_PLOSIVE_INSTANCE_H
#define OMX_PLOSIVE_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_allpass.h>
#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_envelope.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_PLOSIVE_INSTANCE_CHANNELS 2

/** The consonant band's top edge, as a share of the rate: PLOSIVE_BAND_EDGE_FRACTION. */
#define OMX_PLOSIVE_INSTANCE_EDGE OMX_PLOSIVE_BAND_EDGE_FRACTION_DOUBLE

/** One half's detector: the transient designer's fast follower and a slow one differing in its attack. */
struct omx_plosive_detector {
  struct omx_env fast, slow;
};

typedef struct {
  float sr;
  /* the resolved controls */
  float cons_db, cons_s, cons_rec, band_hz, band_oct, pop_db, pop_s, pop_rec, pop_hz;
  /* the designs */
  float a;                       /**< the pop split's allpass coefficient */
  float bp[5];                   /**< the consonant band's section */
  struct omx_env_params fe;      /**< the fast follower's parameters, peak */
  struct omx_envdiff_poles pp;   /**< the pop detector's poles */
  struct omx_envdiff_poles cp;   /**< the consonant detector's poles */
  /* the state */
  struct omx_allpass1 ap[2];     /**< the pop split's allpass, per leg */
  float bs[2][4];                /**< the band section's history, per leg */
  struct omx_plosive_detector pd, cd;
  float pop_min, cons_min;       /**< the deepest gain of each half in the last block, linear */
  int enabled;
  int ready;
} OmxPlosiveInstance;

/** The half's gain for a contrast: 10^(-amount ramp(delta, s)/20), exactly 1 below the offset. */
static inline float omx_plosive_gain(float amount_db, float delta, float s_db) {
  const float u = omx_envdiff_ramp(delta, s_db);
  return u > 0.0f && amount_db > 0.0f ? omx_db_to_lin_poly(-amount_db * u) : 1.0f;
}

/** One leg's pop band, LP = (x + A x)/2, the allpass's word advanced. */
static inline float omx_plosive_pop_band(struct omx_allpass1 *ap, float a, float x) {
  return 0.5f * (x + omx_allpass1_tick(ap, x, a));
}

/** The pop half at gain g: x - (1 - g) LP, the input itself at g = 1 (L2). */
static inline float omx_plosive_pop_apply(float x, float lp, float g) { return g != 1.0f ? x - (1.0f - g) * lp : x; }

/** The consonant half at gain g: x1 + (g - 1) B, x1 itself at g = 1 (L2). */
static inline float omx_plosive_cons_apply(float x1, float b, float g) { return g != 1.0f ? x1 + (g - 1.0f) * b : x1; }

static inline void omx_plosive_instance_resolve(OmxPlosiveInstance *s, int bypass, float consonant_db, float sensitivity_db,
                                                float recovery_ms, float band_hz, float band_width_oct, float pop_db,
                                                float pop_sensitivity_db, float pop_recovery_ms, float pop_hz);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. The tamer comes up at its declared defaults. Allocates nothing. */
static inline int omx_plosive_instance_init(OmxPlosiveInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  s->pop_min = s->cons_min = 1.0f;
  omx_plosive_instance_resolve(s, 0, (float)OMX_PLOSIVE_CONSONANT_DB_RANGE_DEFAULT, (float)OMX_PLOSIVE_SENSITIVITY_DB_RANGE_DEFAULT,
                               (float)OMX_PLOSIVE_RECOVERY_MS_RANGE_DEFAULT, (float)OMX_PLOSIVE_BAND_HZ_RANGE_DEFAULT,
                               (float)OMX_PLOSIVE_BAND_WIDTH_OCT_RANGE_DEFAULT, (float)OMX_PLOSIVE_POP_DB_RANGE_DEFAULT,
                               (float)OMX_PLOSIVE_POP_SENSITIVITY_DB_RANGE_DEFAULT, (float)OMX_PLOSIVE_POP_RECOVERY_MS_RANGE_DEFAULT,
                               (float)OMX_PLOSIVE_POP_HZ_RANGE_DEFAULT);
  return 1;
}

/** The detector poles of a half whose slow follower attacks in `slow_attack_ms`. */
static inline void omx_plosive_poles(const struct omx_env_params *fe, float slow_attack_ms, float sr,
                                     struct omx_envdiff_poles *p) {
  omx_env_stage_poles(fe, 1u, &p->fast_attack, &p->fast_release);
  const struct omx_env_params sa = {omx_pole_from_time_ms(slow_attack_ms, sr), fe->release_pole, OMX_DETECT_PEAK};
  omx_env_stage_poles(&sa, 1u, &p->slow_attack, &p->slow_release);
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the plosive kernel's contract controls in their declared order (dB, dB, ms, Hz,
 * octaves, dB, dB, ms, Hz).
 */
#define OMX_CONTRACT_STAGE "plosive/instance-resolve"
static inline void omx_plosive_instance_resolve(OmxPlosiveInstance *s, int bypass, float consonant_db, float sensitivity_db,
                                                float recovery_ms, float band_hz, float band_width_oct, float pop_db,
                                                float pop_sensitivity_db, float pop_recovery_ms, float pop_hz) {
  if (!s || !s->ready) return;
  s->cons_db = omx_clamp_or(consonant_db, (float)OMX_PLOSIVE_CONSONANT_DB_RANGE_MIN, (float)OMX_PLOSIVE_CONSONANT_DB_RANGE_MAX,
                         (float)OMX_PLOSIVE_CONSONANT_DB_RANGE_DEFAULT);
  s->cons_s = omx_clamp_or(sensitivity_db, (float)OMX_PLOSIVE_SENSITIVITY_DB_RANGE_MIN, (float)OMX_PLOSIVE_SENSITIVITY_DB_RANGE_MAX,
                         (float)OMX_PLOSIVE_SENSITIVITY_DB_RANGE_DEFAULT);
  s->cons_rec = omx_clamp_or(recovery_ms, (float)OMX_PLOSIVE_RECOVERY_MS_RANGE_MIN, (float)OMX_PLOSIVE_RECOVERY_MS_RANGE_MAX,
                         (float)OMX_PLOSIVE_RECOVERY_MS_RANGE_DEFAULT);
  s->band_hz = omx_clamp_or(band_hz, (float)OMX_PLOSIVE_BAND_HZ_RANGE_MIN, (float)OMX_PLOSIVE_BAND_HZ_RANGE_MAX,
                         (float)OMX_PLOSIVE_BAND_HZ_RANGE_DEFAULT);
  s->band_oct = omx_clamp_or(band_width_oct, (float)OMX_PLOSIVE_BAND_WIDTH_OCT_RANGE_MIN, (float)OMX_PLOSIVE_BAND_WIDTH_OCT_RANGE_MAX,
                         (float)OMX_PLOSIVE_BAND_WIDTH_OCT_RANGE_DEFAULT);
  s->pop_db = omx_clamp_or(pop_db, (float)OMX_PLOSIVE_POP_DB_RANGE_MIN, (float)OMX_PLOSIVE_POP_DB_RANGE_MAX,
                         (float)OMX_PLOSIVE_POP_DB_RANGE_DEFAULT);
  s->pop_s = omx_clamp_or(pop_sensitivity_db, (float)OMX_PLOSIVE_POP_SENSITIVITY_DB_RANGE_MIN, (float)OMX_PLOSIVE_POP_SENSITIVITY_DB_RANGE_MAX,
                         (float)OMX_PLOSIVE_POP_SENSITIVITY_DB_RANGE_DEFAULT);
  s->pop_rec = omx_clamp_or(pop_recovery_ms, (float)OMX_PLOSIVE_POP_RECOVERY_MS_RANGE_MIN, (float)OMX_PLOSIVE_POP_RECOVERY_MS_RANGE_MAX,
                         (float)OMX_PLOSIVE_POP_RECOVERY_MS_RANGE_DEFAULT);
  s->pop_hz = omx_clamp_or(pop_hz, (float)OMX_PLOSIVE_POP_HZ_RANGE_MIN, (float)OMX_PLOSIVE_POP_HZ_RANGE_MAX,
                         (float)OMX_PLOSIVE_POP_HZ_RANGE_DEFAULT);
  s->a = omx_allpass1_coef(s->pop_hz, s->sr);
  const double half = pow(2.0, 0.5 * (double)s->band_oct);
  const double lo = (double)s->band_hz / half, hi = fmin((double)s->band_hz * half, OMX_PLOSIVE_INSTANCE_EDGE * (double)s->sr);
  omx_eq_design_f(OMX_EQ_BANDPASS, sqrt(lo * hi), omx_eq_octaves_q(log2(hi / lo)), 0.0, (double)s->sr, s->bp);
  s->fe = (struct omx_env_params){omx_pole_from_time_ms(OMX_TRANSIENT_FAST_ATTACK_MS, s->sr),
                                  omx_pole_from_time_ms(OMX_TRANSIENT_FAST_RELEASE_MS, s->sr), OMX_DETECT_PEAK};
  omx_plosive_poles(&s->fe, s->pop_rec, s->sr, &s->pp);
  omx_plosive_poles(&s->fe, s->cons_rec, s->sr, &s->cp);
  s->enabled = bypass ? 0 : 1;
  OMX_POST(s->a > -1.0f && s->a < 1.0f && hi > lo, "designs-inside-their-laws");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the pop half then the consonant half, in to out. `in_*` and `out_*`
 * may alias. Not ready or bypassed is the identity and moves no state.
 */
#define OMX_CONTRACT_STAGE "plosive/instance-run"
static inline void omx_plosive_instance_run(OmxPlosiveInstance *s, const float *in_l, const float *in_r, float *out_l,
                                            float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || !s->enabled) return;
  OMX_PRE(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-in");
  float pmin = 1.0f, cmin = 1.0f;
  for (uint32_t i = 0; i < n; i++) {
    const float xl = out_l[i], xr = out_r[i];
    /* the pop half: its detector reads this sample's low band, its gain applies to it */
    const float lpl = omx_plosive_pop_band(&s->ap[0], s->a, xl), lpr = omx_plosive_pop_band(&s->ap[1], s->a, xr);
    const float dp = 0.5f * (fabsf(lpl) + fabsf(lpr));
    const float fp = omx_env_step(&s->pd.fast, &s->fe, dp, s->pp.fast_attack, s->pp.fast_release);
    const float gp = omx_plosive_gain(s->pop_db, omx_envdiff_step(&s->pd.slow, &s->fe, dp, fp, &s->pp, OMX_TRANSIENT_FLOOR_LIN), s->pop_s);
    const float x1l = omx_plosive_pop_apply(xl, lpl, gp), x1r = omx_plosive_pop_apply(xr, lpr, gp);
    /* the consonant half, on what the pop half left */
    const float bl = omx_biquad(x1l, s->bp, s->bs[0]), br = omx_biquad(x1r, s->bp, s->bs[1]);
    const float dc = 0.5f * (fabsf(bl) + fabsf(br));
    const float fc = omx_env_step(&s->cd.fast, &s->fe, dc, s->cp.fast_attack, s->cp.fast_release);
    const float gc = omx_plosive_gain(s->cons_db, omx_envdiff_step(&s->cd.slow, &s->fe, dc, fc, &s->cp, OMX_TRANSIENT_FLOOR_LIN), s->cons_s);
    out_l[i] = omx_plosive_cons_apply(x1l, bl, gc);
    out_r[i] = omx_plosive_cons_apply(x1r, br, gc);
    for (int k = 0; k < 4; k++) s->bs[0][k] = omx_flush(s->bs[0][k]), s->bs[1][k] = omx_flush(s->bs[1][k]);
    for (int k = 0; k < OMX_DYN_ENV_STAGES; k++) {
      s->pd.fast.stage[k] = omx_flush(s->pd.fast.stage[k]);
      s->pd.slow.stage[k] = omx_flush(s->pd.slow.stage[k]);
      s->cd.fast.stage[k] = omx_flush(s->cd.fast.stage[k]);
      s->cd.slow.stage[k] = omx_flush(s->cd.slow.stage[k]);
    }
    pmin = gp < pmin ? gp : pmin;
    cmin = gc < cmin ? gc : cmin;
  }
  s->pop_min = pmin;
  s->cons_min = cmin;
  OMX_POST(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/** The consonantReductionDb readout: the deepest consonant cut of the last block, dB, at most 0. */
static inline float omx_plosive_instance_consonant_reduction_db(const OmxPlosiveInstance *s) {
  return s && s->ready && s->cons_min < 1.0f ? omx_lin_to_db(s->cons_min) : 0.0f;
}

/** The popReductionDb readout: the deepest pop cut of the last block, dB, at most 0. */
static inline float omx_plosive_instance_pop_reduction_db(const OmxPlosiveInstance *s) {
  return s && s->ready && s->pop_min < 1.0f ? omx_lin_to_db(s->pop_min) : 0.0f;
}

/** The frames of latency the instance introduces: none. */
static inline uint32_t omx_plosive_instance_latency(const OmxPlosiveInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_PLOSIVE_INSTANCE_H */
