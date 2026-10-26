/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_tone_instance.h — the tone face as a plugin instance: a tilt about one pivot and a DJ filter,
 * one knob each, for a monitor mix or an FX return made brighter or darker and a playback swept. It
 * mints no DSP: the tilt is omx_tilt, one first-order complementary shelf pair, and the filter is the
 * EQ's own low-pass or high-pass section (omx_eq_design), both run in double (omx_biquad_d).
 *
 *   1. RESOLVE. {@link omx_tone_instance_resolve} takes the tone kernel's contract controls in their
 *      declared order (data/kernels/tone.json), each clamped into its travel, a non-finite word
 *      reading as its default.
 *      - `tiltDb` is the tilt's SPAN (the ruling R3b of omx-dsp#188): `a = 10^(tiltDb/40)`
 *        (omx_tilt_a_of_span), so the tilt is `-tiltDb/2` at DC, `+tiltDb/2` at Nyquist and 0 dB at
 *        `pivotHz`. +6 dB is a +-3 dB pair, about 2 dB per octave at the pivot. One section, never two.
 *      - `filter` below 0 is a low-pass whose corner falls from TONE_FILTER_HIGH_HZ at -0 to
 *        TONE_FILTER_LOW_HZ at -100; above 0 a high-pass whose corner rises from TONE_FILTER_LOW_HZ
 *        to TONE_FILTER_HIGH_HZ; on a log map, {@link omx_tone_filter_hz}. 0 is open. `resonance`
 *        is the section's Q: the magnitude at the corner, as the EQ's sections are designed.
 *        A corner past 0.999 of Nyquist is held there by omx_eq_design.
 *   2. RUN. Per leg, the tilt then the filter. A section at its rest value (a span of 0, a filter
 *      of 0) is not run and moves no state; with both at rest the face is the input, bit for bit. A
 *      section that comes back into use starts from silence, and so does the filter on every change
 *      of side: a low-pass's history is never read by the high-pass, nor the other way.
 *
 * LATENCY IS ZERO. Nothing is allocated; every word of state is in {@link OmxToneInstance}. The
 * sections are designed in resolve(), one tan for the tilt and the EQ design for the filter.
 */
#ifndef OMX_TONE_INSTANCE_H
#define OMX_TONE_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_tilt.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_TONE_INSTANCE_CHANNELS 2

/** The DJ filter's ends, Hz: TONE_FILTER_LOW_HZ and TONE_FILTER_HIGH_HZ. */
#define OMX_TONE_INSTANCE_LOW_HZ ((double)OMX_TONE_FILTER_LOW_HZ)
#define OMX_TONE_INSTANCE_HIGH_HZ ((double)OMX_TONE_FILTER_HIGH_HZ)

typedef struct {
  float sr;
  struct omx_tilt tilt;
  double fc[5];       /**< the filter's section, `{b0, b1, b2, a1, a2}` */
  double ts[2][4];    /**< the tilt's history per leg */
  double fs[2][4];    /**< the filter's history per leg */
  float span_db;      /**< the tilt's span as resolved, dB */
  float pivot_hz;     /**< the tilt's pivot as resolved, Hz */
  float filter;       /**< the DJ filter as resolved, -100 to +100 */
  float q;            /**< the filter's resonance as resolved */
  int tilt_on;        /**< 1 while the tilt is not flat */
  int filter_on;      /**< 1 while the filter is not open */
  int fside;          /**< the side the filter's history belongs to: -1 the low-pass, +1 the high-pass */
  int enabled;
  int ready;
} OmxToneInstance;

/**
 * The DJ filter's corner, Hz, for a `filter` word in [-100, 100]: below 0 the low-pass,
 * `TONE_FILTER_HIGH_HZ x (TONE_FILTER_LOW_HZ / TONE_FILTER_HIGH_HZ)^(-filter/100)`; above 0 the
 * high-pass, `TONE_FILTER_LOW_HZ x (TONE_FILTER_HIGH_HZ / TONE_FILTER_LOW_HZ)^(filter/100)`. Its
 * logarithm is linear in the word on each side. 0 returns 0 (open). RT-safe: one exp. Pure.
 */
static inline double omx_tone_filter_hz(double filter) {
  if (filter == 0.0) return 0.0;
  const double u = fabs(filter) / 100.0, span = log(OMX_TONE_INSTANCE_HIGH_HZ / OMX_TONE_INSTANCE_LOW_HZ);
  return filter < 0.0 ? OMX_TONE_INSTANCE_HIGH_HZ * exp(-u * span) : OMX_TONE_INSTANCE_LOW_HZ * exp(u * span);
}

static inline void omx_tone_instance_resolve(OmxToneInstance *s, int bypass, float tilt_db, float pivot_hz, float filter,
                                             float resonance);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. The face comes up at its declared defaults, flat and open. Allocates nothing. */
static inline int omx_tone_instance_init(OmxToneInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  omx_tone_instance_resolve(s, 0, (float)OMX_TONE_TILT_DB_RANGE_DEFAULT, (float)OMX_TONE_PIVOT_HZ_RANGE_DEFAULT,
                            (float)OMX_TONE_FILTER_RANGE_DEFAULT, (float)OMX_TONE_RESONANCE_RANGE_DEFAULT);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the tone kernel's contract controls in their declared order: the tilt's span (dB),
 * its pivot (Hz), the DJ filter (-100 to +100) and its resonance.
 */
#define OMX_CONTRACT_STAGE "tone/instance-resolve"
static inline void omx_tone_instance_resolve(OmxToneInstance *s, int bypass, float tilt_db, float pivot_hz, float filter,
                                             float resonance) {
  if (!s || !s->ready) return;
  s->span_db = omx_clamp_or(tilt_db, (float)OMX_TONE_TILT_DB_RANGE_MIN, (float)OMX_TONE_TILT_DB_RANGE_MAX,
                            (float)OMX_TONE_TILT_DB_RANGE_DEFAULT);
  s->pivot_hz = omx_clamp_or(pivot_hz, (float)OMX_TONE_PIVOT_HZ_RANGE_MIN, (float)OMX_TONE_PIVOT_HZ_RANGE_MAX,
                             (float)OMX_TONE_PIVOT_HZ_RANGE_DEFAULT);
  s->filter = omx_clamp_or(filter, (float)OMX_TONE_FILTER_RANGE_MIN, (float)OMX_TONE_FILTER_RANGE_MAX,
                           (float)OMX_TONE_FILTER_RANGE_DEFAULT);
  s->q = omx_clamp_or(resonance, (float)OMX_TONE_RESONANCE_RANGE_MIN, (float)OMX_TONE_RESONANCE_RANGE_MAX,
                      (float)OMX_TONE_RESONANCE_RANGE_DEFAULT);
  const int tilt_on = s->span_db != 0.0f, filter_on = s->filter != 0.0f;
  const int side_changed = filter_on && s->filter_on && (s->filter < 0.0f) != (s->fside < 0);
  if (tilt_on) {
    omx_tilt_design(&s->tilt, omx_tilt_a_of_span(s->span_db), s->pivot_hz, s->sr);
    if (!s->tilt_on) memset(s->ts, 0, sizeof(s->ts));
  }
  if (filter_on) {
    omx_eq_design(s->filter < 0.0f ? OMX_EQ_LOWPASS : OMX_EQ_HIGHPASS, omx_tone_filter_hz(s->filter), s->q, 0.0, s->sr,
                  s->fc);
    if (!s->filter_on || side_changed) memset(s->fs, 0, sizeof(s->fs));
    s->fside = s->filter < 0.0f ? -1 : 1;
  }
  s->tilt_on = tilt_on;
  s->filter_on = filter_on;
  s->enabled = bypass ? 0 : 1;
}
#undef OMX_CONTRACT_STAGE

/**
 * The face's magnitude at `freq_hz` as resolved, below Nyquist: the tilt's closed form
 * (omx_tilt_magnitude) times the filter section's `|H(e^jw)|`. What a host draws; 1 when flat and
 * open. RT-safe: a few trig calls. Thread-safe: reads `s`.
 */
static inline double omx_tone_instance_magnitude(const OmxToneInstance *s, double freq_hz) {
  if (!s || !s->ready) return 1.0;
  double m = s->tilt_on ? omx_tilt_magnitude(s->tilt.a, s->pivot_hz, freq_hz, s->sr) : 1.0;
  if (s->filter_on) {
    const double w = 2.0 * M_PI * freq_hz / s->sr, c1 = cos(w), s1 = sin(w), c2 = cos(2.0 * w), s2 = sin(2.0 * w);
    const double nr = s->fc[0] + s->fc[1] * c1 + s->fc[2] * c2, ni = -(s->fc[1] * s1 + s->fc[2] * s2);
    const double dr = 1.0 + s->fc[3] * c1 + s->fc[4] * c2, di = -(s->fc[3] * s1 + s->fc[4] * s2);
    m *= sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
  }
  return m;
}

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: each leg through the tilt then the filter, in to out. `in_*` and
 * `out_*` may alias. Not ready, bypassed, or flat and open is the identity and moves no state.
 */
#define OMX_CONTRACT_STAGE "tone/instance-run"
static inline void omx_tone_instance_run(OmxToneInstance *s, const float *in_l, const float *in_r, float *out_l,
                                         float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || !s->enabled || !(s->tilt_on || s->filter_on)) return;
  OMX_PRE(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-in");
  float *legs[2] = {out_l, out_r};
  for (int c = 0; c < 2; c++) {
    float *x = legs[c];
    if (s->tilt_on)
      for (uint32_t i = 0; i < n; i++) x[i] = omx_tilt_tick(&s->tilt, x[i], s->ts[c]);
    if (s->filter_on)
      for (uint32_t i = 0; i < n; i++) x[i] = omx_biquad_d(x[i], s->fc, s->fs[c]);
  }
  OMX_POST(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces: none. */
static inline uint32_t omx_tone_instance_latency(const OmxToneInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_TONE_INSTANCE_H */
