// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_clipper_instance.h
 * @brief A peak clipper with a ceiling: a drive, a ceiling and a softness, per sample, no memory.
 *
 * `u = drive·x`. Below the knee `a = c·(1 − span·softness)` a sample is `u`; above it
 * `y = a + (c − a)·d / (d + c − a)`, `d = |u| − a`, signed as `u`, a hyperbola that leaves the
 * straight line at slope 1 and approaches the ceiling `c` from below. At a softness of 0 `a` is `c`,
 * the hard clip. Each leg is its own: a clipper has no link and no memory, so it adds no latency and
 * runs on a strip's own thread. At the come-up values a signal under full scale is itself, bit for bit.
 *
 * It clips at the base rate: the harmonics it makes above Nyquist fold back. omx_limiter is the word
 * for inaudible peak control; this one is for the sound of clipping, and for a ceiling with no look-ahead.
 *
 * Console use: on a drum or a bus strip, the last insert before MAIN, a ceiling that costs no latency.
 * Nothing is allocated; every word of state is in {@link OmxClipperInstance}.
 */
#ifndef OMX_CLIPPER_INSTANCE_H
#define OMX_CLIPPER_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_instance_io.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_CLIPPER_INSTANCE_CHANNELS 2

/**
 * @brief One sample through the clip.
 * @param u The sample after the drive.
 * @param c The ceiling, linear, positive.
 * @param a The knee, linear, in (0, c].
 * @return `u` under the knee, else the bend, signed as `u`, never past `c`.
 * @note One divide above the knee. Thread-safe: pure.
 * @thread audio
 */
static inline float omx_clipper_sample(float u, float c, float a) {
  const float m = fabsf(u);
  if (m <= a) return u;
  const float room = c - a, d = m - a;
  const float y = room > 0.0f ? a + room * d / (d + room) : c;
  return u < 0.0f ? -y : y;
}

typedef struct {
  float sr;
  float drive_lin;   /**< the gain before the clip */
  float ceiling_lin; /**< the ceiling */
  float knee_lin;    /**< where the bend starts */
  float gr_db;       /**< the last block's reduction, dB, negative or 0 */
  int enabled;
  int ready;
} OmxClipperInstance;

static inline void omx_clipper_instance_resolve(OmxClipperInstance *s, int bypass, float drive_db, float ceiling_db,
                                                float softness_pct);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. It comes up at its declared defaults. Allocates nothing.
 * @thread control */
static inline int omx_clipper_instance_init(OmxClipperInstance *s, float sr) {
  if (!omx_instance_bind(s, sizeof(*s), sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  omx_clipper_instance_resolve(s, 0, (float)OMX_CLIPPER_DRIVE_DB_RANGE_DEFAULT, (float)OMX_CLIPPER_CEILING_DB_RANGE_DEFAULT,
                               (float)OMX_CLIPPER_SOFTNESS_PCT_RANGE_DEFAULT);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the clipper kernel's contract controls in their declared order, each clamped into its
 * travel, a non-finite one reading as its default.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "clipper/instance-resolve"
static inline void omx_clipper_instance_resolve(OmxClipperInstance *s, int bypass, float drive_db, float ceiling_db,
                                                float softness_pct) {
  if (!s || !s->ready) return;
  const float dr = omx_clamp_or(drive_db, (float)OMX_CLIPPER_DRIVE_DB_RANGE_MIN, (float)OMX_CLIPPER_DRIVE_DB_RANGE_MAX,
                                (float)OMX_CLIPPER_DRIVE_DB_RANGE_DEFAULT);
  const float ce = omx_clamp_or(ceiling_db, (float)OMX_CLIPPER_CEILING_DB_RANGE_MIN, (float)OMX_CLIPPER_CEILING_DB_RANGE_MAX,
                                (float)OMX_CLIPPER_CEILING_DB_RANGE_DEFAULT);
  const float so = omx_clamp_or(softness_pct, (float)OMX_CLIPPER_SOFTNESS_PCT_RANGE_MIN,
                                (float)OMX_CLIPPER_SOFTNESS_PCT_RANGE_MAX, (float)OMX_CLIPPER_SOFTNESS_PCT_RANGE_DEFAULT);
  s->drive_lin = omx_db_to_lin(dr);
  s->ceiling_lin = omx_db_to_lin(ce);
  s->knee_lin = s->ceiling_lin * (1.0f - (float)OMX_CLIPPER_KNEE_SPAN * so * 0.01f);
  s->enabled = !bypass;
  OMX_POST(s->knee_lin > 0.0f && s->knee_lin <= s->ceiling_lin, "knee-under-the-ceiling");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the clip over both legs, in to out. `in_*` and `out_*` may alias.
 * Not ready or bypassed is the identity.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "clipper/instance-run"
static inline void omx_clipper_instance_run(OmxClipperInstance *s, const float *in_l, const float *in_r, float *out_l,
                                            float *out_r, uint32_t n) {
  if (!omx_instance_copy_through(in_l, in_r, out_l, out_r, n)) return;
  if (!s || !s->ready || !s->enabled) {
    if (s) s->gr_db = 0.0f;
    return;
  }
  OMX_PRE(omx_block_pair_finite(out_l, out_r, n), "finite-in");
  const float g = s->drive_lin, c = s->ceiling_lin, a = s->knee_lin;
  float over = 0.0f, kept = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    const float ul = g * out_l[i], ur = g * out_r[i];
    const float yl = omx_clipper_sample(ul, c, a), yr = omx_clipper_sample(ur, c, a);
    const float ml = fabsf(ul), mr = fabsf(ur);
    if (ml > over) { over = ml; kept = fabsf(yl); }
    if (mr > over) { over = mr; kept = fabsf(yr); }
    out_l[i] = yl;
    out_r[i] = yr;
  }
  s->gr_db = over > 0.0f && kept < over ? omx_lin_to_db(kept / over) : 0.0f;
  OMX_POST(omx_block_pair_finite(out_l, out_r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/** The gainReductionDb readout: the last block's reduction, dB, 0 or negative.
 * @thread audio */
static inline float omx_clipper_instance_gain_reduction_db(const OmxClipperInstance *s) { return s ? s->gr_db : 0.0f; }

/** The frames of latency the instance introduces: none.
 * @thread audio */
static inline uint32_t omx_clipper_instance_latency(const OmxClipperInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_CLIPPER_INSTANCE_H */
