/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_dyneq_instance.h — the dynamic EQ as a plugin instance: six EQ bands, each with its own
 * dynamics, and no format in it. It mints no DSP: every band's static section is the EQ face's own
 * (fx/omx_eq_instance.h: its design, its clamps, its parking, its bank and its state), and every
 * band's dynamics are the band-dynamics word (omx_band_dyn.h), the word the de-esser runs.
 *
 *   1. RESOLVE. {@link omx_dyneq_instance_resolve} takes the dyneq kernel's contract controls in
 *      their declared order (data/kernels/dyneq.json), one array per band control and the mix. The
 *      static half (switch, type, centre, gain, Q) goes to an EQ face whose first six bands are these
 *      and whose other bands and pass filters are off, so its bank is exactly the six bands that are
 *      on, in order. The dynamic half resolves to one band-dynamics atom per band: above the
 *      threshold for `down`, below it for `up`, the signed range, the band knee BAND_DYN_KNEE_DB, peak
 *      detection, no make-up; and `B`, designed by omx_band_dyn_design() at the band's clamped centre
 *      and Q only when the shape, centre or Q moved. Every travel is clamped into its declared range,
 *      a non-finite word reading as the declared default; a choice outside its ids reads as its
 *      default.
 *   2. RUN. In chunks of OMX_DYNEQ_INSTANCE_CHUNK frames, band by band: the band's static section
 *      over both legs (omx_biquad_cascade() on the EQ face's bank entry and state), then its dynamics
 *      over both legs, one detector for the two (omx_band_dyn_process()). The detector hears the band
 *      after its static section. With every band's dynamics off the output is the EQ face's at the
 *      same bands, bit for bit: the cascade runs the same sections in the same order on the same
 *      state, and a band whose dynamics are off touches no sample.
 *   3. LISTEN. While a band listens, the output is its `B` over the signal reaching the band, through
 *      a history of its own; with several bands listening, their sum. Listen ignores the mix.
 *   4. MIX. Below 100 % the output is the convex blend of the input and the processed signal
 *      (omx_wetdry_mix()); at 100 % it is the processed signal itself.
 *
 * READOUTS. {@link omx_dyneq_instance_band_gain_db} is the dyneq kernel's band<n>GainDb: the band's
 * gain computer re-run on the level its detector holds after the last block, signed.
 *
 * Bypass is the identity and keeps every state word. LATENCY IS ZERO. Nothing is allocated; the
 * whole state is inline in {@link OmxDyneqInstance}.
 */
#ifndef OMX_DYNEQ_INSTANCE_H
#define OMX_DYNEQ_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_eq_instance.h>
#include <omxdsp/omx_band_dyn.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_wetdry.h>

#define OMX_DYNEQ_INSTANCE_CHANNELS 2

/** The bands, the dyneq kernel's DYNEQ_BANDS. */
#define OMX_DYNEQ_INSTANCE_BANDS OMX_DYNEQ_BANDS

/** Frames per pass through the scratch; nothing observable depends on it. */
#define OMX_DYNEQ_INSTANCE_CHUNK 256u

_Static_assert(OMX_DYNEQ_INSTANCE_BANDS <= OMX_EQ_INSTANCE_BANDS, "the EQ face carries every band of the dynamic EQ");
_Static_assert((int)OMX_DYNEQ_BAND_TYPES_BELL == (int)OMX_EQ_BAND_TYPES_BELL &&
                   (int)OMX_DYNEQ_BAND_TYPES_LOW_SHELF == (int)OMX_EQ_BAND_TYPES_LOW_SHELF &&
                   (int)OMX_DYNEQ_BAND_TYPES_HIGH_SHELF == (int)OMX_EQ_BAND_TYPES_HIGH_SHELF,
               "a dynamic band's type is the EQ band type of the same index");
_Static_assert((int)OMX_DYNEQ_BAND_ONS_DEFAULT == (int)OMX_EQ_BAND_ONS_DEFAULT &&
                   (int)OMX_DYNEQ_BAND_ONS_ON == (int)OMX_EQ_BAND_ONS_ON,
               "a dynamic band switches and comes up as an EQ band does");

/** One instance: the EQ face that runs the static sections, one band-dynamics atom and state per band,
 * the listen histories and the scratch. */
typedef struct {
  float sr;
  OmxEqInstance eq;
  struct omx_band_dyn band[OMX_DYNEQ_INSTANCE_BANDS];
  struct omx_band_dyn_state st[OMX_DYNEQ_INSTANCE_BANDS];
  /** Band i's entry in the EQ face's bank while it is on. */
  uint32_t entry[OMX_DYNEQ_INSTANCE_BANDS];
  int on[OMX_DYNEQ_INSTANCE_BANDS];
  int listen[OMX_DYNEQ_INSTANCE_BANDS];
  /** The shape, centre and Q `B` was last designed from; `designed` 0 until the first. */
  int kind[OMX_DYNEQ_INSTANCE_BANDS];
  float freq[OMX_DYNEQ_INSTANCE_BANDS], q[OMX_DYNEQ_INSTANCE_BANDS];
  int designed[OMX_DYNEQ_INSTANCE_BANDS];
  /** Each listening band's own history of `B`, per leg. */
  float listen_l[OMX_DYNEQ_INSTANCE_BANDS][4], listen_r[OMX_DYNEQ_INSTANCE_BANDS][4];
  float mix;
  int enabled;
  int ready;
  float dry_l[OMX_DYNEQ_INSTANCE_CHUNK], dry_r[OMX_DYNEQ_INSTANCE_CHUNK];
  float sum_l[OMX_DYNEQ_INSTANCE_CHUNK], sum_r[OMX_DYNEQ_INSTANCE_CHUNK];
  float tap_l[OMX_DYNEQ_INSTANCE_CHUNK], tap_r[OMX_DYNEQ_INSTANCE_CHUNK];
} OmxDyneqInstance;

/** A choice word: inside [0, count), else the declared default. */
static inline int omx_dyneq_instance_choice(int v, int count, int dflt) { return v >= 0 && v < count ? v : dflt; }

/** The EQ design kind of a dynamic band's type. */
static inline enum omx_eq_kind omx_dyneq_instance_kind(int type) {
  return type == OMX_DYNEQ_BAND_TYPES_LOW_SHELF ? OMX_EQ_LOWSHELF
         : type == OMX_DYNEQ_BAND_TYPES_HIGH_SHELF ? OMX_EQ_HIGHSHELF
                                                   : OMX_EQ_PEAKING;
}

/**
 * Bind an instance to its rate. Returns 1 when usable, 0 when not (a refused init leaves `ready`
 * clear and run() is the identity): the rate must be one of the oracle set. Every band comes up off,
 * so a fresh instance is a wire. Allocates nothing.
 */
static inline int omx_dyneq_instance_init(OmxDyneqInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr) || !omx_eq_instance_init(&s->eq, sr)) return 0;
  for (uint32_t i = 0; i < OMX_DYNEQ_INSTANCE_BANDS; i++) omx_band_dyn_state_init(&s->st[i]);
  s->sr = sr;
  s->mix = 1.0f;
  s->enabled = 1;
  s->ready = 1;
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the dyneq kernel's contract controls in their declared order: per band (arrays of
 * OMX_DYNEQ_INSTANCE_BANDS) the switch (EQ_BAND_ONS index), the type (bell, lowShelf, highShelf), the
 * centre (Hz), the static gain (dB), the Q, the dynMode (off, down, up), the threshold (dB), the
 * ratio, the signed range (dB), the attack and the release (ms) and the listen switch; then the
 * mix (percent).
 */
#define OMX_CONTRACT_STAGE "dyneq/instance-resolve"
static inline void omx_dyneq_instance_resolve(OmxDyneqInstance *s, int bypass, const int band_on[OMX_DYNEQ_INSTANCE_BANDS],
                                              const int band_type[OMX_DYNEQ_INSTANCE_BANDS],
                                              const float freq[OMX_DYNEQ_INSTANCE_BANDS],
                                              const float gain[OMX_DYNEQ_INSTANCE_BANDS],
                                              const float q[OMX_DYNEQ_INSTANCE_BANDS],
                                              const int dyn_mode[OMX_DYNEQ_INSTANCE_BANDS],
                                              const float threshold[OMX_DYNEQ_INSTANCE_BANDS],
                                              const float ratio[OMX_DYNEQ_INSTANCE_BANDS],
                                              const float range[OMX_DYNEQ_INSTANCE_BANDS],
                                              const float attack[OMX_DYNEQ_INSTANCE_BANDS],
                                              const float release[OMX_DYNEQ_INSTANCE_BANDS],
                                              const int listen[OMX_DYNEQ_INSTANCE_BANDS], float mix_pct) {
  if (!s || !s->ready || !band_on || !band_type || !freq || !gain || !q || !dyn_mode || !threshold || !ratio || !range ||
      !attack || !release || !listen)
    return;
  int eq_type[OMX_EQ_INSTANCE_BANDS], eq_on[OMX_EQ_INSTANCE_BANDS], modes[OMX_DYNEQ_INSTANCE_BANDS];
  float eq_freq[OMX_EQ_INSTANCE_BANDS], eq_gain[OMX_EQ_INSTANCE_BANDS], eq_q[OMX_EQ_INSTANCE_BANDS];
  for (uint32_t i = OMX_DYNEQ_INSTANCE_BANDS; i < (uint32_t)OMX_EQ_INSTANCE_BANDS; i++) {
    eq_on[i] = 0;
    eq_type[i] = (int)OMX_EQ_BAND_TYPES_DEFAULT;
    eq_freq[i] = (float)OMX_EQ_FREQ_RANGE_MIN;
    eq_gain[i] = 0.0f;
    eq_q[i] = OMX_EQ_LV2_BAND_Q_DEFAULT;
  }
  float centres[OMX_DYNEQ_INSTANCE_BANDS];
  const int spread = omx_eq_default_centres(OMX_DYNEQ_INSTANCE_BANDS, centres) == OMX_DYNEQ_INSTANCE_BANDS;
  for (uint32_t i = 0; i < OMX_DYNEQ_INSTANCE_BANDS; i++) {
    const int sw = omx_dyneq_instance_choice(band_on[i], (int)OMX_DYNEQ_BAND_ONS_COUNT, (int)OMX_DYNEQ_BAND_ONS_DEFAULT);
    const int type = omx_dyneq_instance_choice(band_type[i], (int)OMX_DYNEQ_BAND_TYPES_COUNT, (int)OMX_DYNEQ_BAND_TYPES_DEFAULT);
    /* A non-finite centre is the band's come-up centre in a bank of six (omx_eq_default_centres). */
    const float fc = freq[i] - freq[i] == 0.0f
                         ? omx_clampf(freq[i], (float)OMX_DYNEQ_FREQ_RANGE_MIN, (float)OMX_DYNEQ_FREQ_RANGE_MAX)
                     : spread ? centres[i]
                              : (float)OMX_DYNEQ_FREQ_RANGE_MIN;
    const float g = omx_clamp_or(gain[i], (float)OMX_DYNEQ_GAIN_RANGE_MIN, (float)OMX_DYNEQ_GAIN_RANGE_MAX,
                                 (float)OMX_DYNEQ_GAIN_RANGE_DEFAULT);
    const float bq = omx_clamp_or(q[i], (float)OMX_DYNEQ_Q_RANGE_MIN, (float)OMX_DYNEQ_Q_RANGE_MAX,
                                  (float)OMX_DYNEQ_Q_RANGE_DEFAULT);
    const int dyn_mode_i = omx_dyneq_instance_choice(dyn_mode[i], (int)OMX_DYNEQ_DYN_MODES_COUNT,
                                                     (int)OMX_DYNEQ_DYN_MODES_DEFAULT);
    const int lis = omx_dyneq_instance_choice(listen[i], (int)OMX_DYNEQ_LISTENS_COUNT, (int)OMX_DYNEQ_LISTENS_DEFAULT);
    const float thr = omx_clamp_or(threshold[i], (float)OMX_DYNEQ_THRESHOLD_RANGE_MIN, (float)OMX_DYNEQ_THRESHOLD_RANGE_MAX,
                                   (float)OMX_DYNEQ_THRESHOLD_RANGE_DEFAULT);
    const float rat = omx_clamp_or(ratio[i], (float)OMX_DYNEQ_RATIO_RANGE_MIN, (float)OMX_DYNEQ_RATIO_RANGE_MAX,
                                   (float)OMX_DYNEQ_RATIO_RANGE_DEFAULT);
    const float rng = omx_clamp_or(range[i], (float)OMX_DYNEQ_RANGE_RANGE_MIN, (float)OMX_DYNEQ_RANGE_RANGE_MAX,
                                   (float)OMX_DYNEQ_RANGE_RANGE_DEFAULT);
    const float att = omx_clamp_or(attack[i], (float)OMX_DYNEQ_ATTACK_RANGE_MIN, (float)OMX_DYNEQ_ATTACK_RANGE_MAX,
                                   (float)OMX_DYNEQ_ATTACK_RANGE_DEFAULT);
    const float rel = omx_clamp_or(release[i], (float)OMX_DYNEQ_RELEASE_RANGE_MIN, (float)OMX_DYNEQ_RELEASE_RANGE_MAX,
                                   (float)OMX_DYNEQ_RELEASE_RANGE_DEFAULT);
    eq_on[i] = sw;
    eq_type[i] = type;
    eq_freq[i] = fc;
    eq_gain[i] = g;
    eq_q[i] = bq;
    modes[i] = dyn_mode_i;
    if (lis && !s->listen[i]) {
      memset(s->listen_l[i], 0, sizeof s->listen_l[i]);
      memset(s->listen_r[i], 0, sizeof s->listen_r[i]);
    }
    s->listen[i] = lis;
    struct omx_band_dyn *b = &s->band[i];
    b->dyn.enabled = 1;
    b->dyn.gc.mode = dyn_mode_i == OMX_DYNEQ_DYN_MODES_UP ? OMX_DYN_BELOW : OMX_DYN_ABOVE;
    b->dyn.gc.thresh_db = thr;
    b->dyn.gc.ratio = rat;
    b->dyn.gc.knee_db = (float)OMX_BAND_DYN_KNEE_DB;
    b->dyn.gc.range_db = rng;
    b->dyn.gc.makeup_lin = 1.0f;
    b->dyn.detect = OMX_DETECT_PEAK;
    b->dyn.ovs_mode = OMX_DYN_OVS_OFF;
    b->dyn.attack_ms = att;
    b->dyn.attack_coeff = omx_pole_from_time_ms(att, s->sr);
    b->dyn.release_coeff = omx_pole_from_time_ms(rel, s->sr);
  }
  omx_eq_instance_resolve(&s->eq, 0, 0, OMX_EQ_LV2_HPF_FREQ_DEFAULT, (int)OMX_FILTER_SLOPES_DEFAULT, 0,
                          OMX_EQ_LV2_LPF_FREQ_DEFAULT, (int)OMX_FILTER_SLOPES_DEFAULT, eq_type, eq_freq, eq_gain, eq_q,
                          eq_on);
  omx_eq_lv2_publish(&s->eq.leg[0]);
  omx_eq_lv2_publish(&s->eq.leg[1]);
  uint32_t k = 0;
  for (uint32_t i = 0; i < OMX_DYNEQ_INSTANCE_BANDS; i++) {
    /* The band as the EQ face holds it (its clamps applied), the band B is designed from. */
    const struct omx_eq_lv2_band *eb = &s->eq.leg[0].band[i];
    s->on[i] = eq_on[i];
    s->entry[i] = k;
    k += (uint32_t)s->on[i];
    struct omx_band_dyn *b = &s->band[i];
    b->enabled = s->on[i] && modes[i] != OMX_DYNEQ_DYN_MODES_OFF;
    const int kind = (int)omx_dyneq_instance_kind(eb->type);
    if (!s->designed[i] || kind != s->kind[i] || eb->freq_hz != s->freq[i] || eb->q != s->q[i]) {
      omx_band_dyn_design((enum omx_eq_kind)kind, (double)eb->freq_hz, (double)eb->q, (double)s->sr, b->b_c);
      s->kind[i] = kind;
      s->freq[i] = eb->freq_hz;
      s->q[i] = eb->q;
      s->designed[i] = 1;
    }
  }
  const float mix_share = omx_clamp_or(mix_pct, (float)OMX_DYNEQ_MIX_PCT_RANGE_MIN, (float)OMX_DYNEQ_MIX_PCT_RANGE_MAX,
                                       (float)OMX_DYNEQ_MIX_PCT_RANGE_DEFAULT) /
                          100.0f;
  s->mix = mix_share;
  s->enabled = bypass ? 0 : 1;
  OMX_POST(k == s->eq.leg[0].count && k == s->eq.leg[1].count, "one-bank-entry-per-band-that-is-on");
  OMX_POST(s->mix >= 0.0f && s->mix <= 1.0f, "mix-in-unit-range");
}
#undef OMX_CONTRACT_STAGE

/** One chunk, in place over `l` and `r`: band by band, the static section and the dynamics; the
 * listened bands' sum, or the mix. */
#define OMX_CONTRACT_STAGE "dyneq/instance-chunk"
static inline void omx_dyneq_instance_chunk(OmxDyneqInstance *s, float *l, float *r, uint32_t m) {
  struct omx_eq_lv2 *el = &s->eq.leg[0], *er = &s->eq.leg[1];
  int listening = 0;
  for (uint32_t i = 0; i < OMX_DYNEQ_INSTANCE_BANDS; i++) listening |= s->on[i] && s->listen[i];
  if (s->mix < 1.0f && !listening) {
    memcpy(s->dry_l, l, m * sizeof(float));
    memcpy(s->dry_r, r, m * sizeof(float));
  }
  if (listening) {
    memset(s->sum_l, 0, m * sizeof(float));
    memset(s->sum_r, 0, m * sizeof(float));
  }
  for (uint32_t i = 0; i < OMX_DYNEQ_INSTANCE_BANDS; i++) {
    if (!s->on[i]) continue;
    const uint32_t k = s->entry[i];
    omx_biquad_cascade(l, m, 1u, (const float(*)[5]) & el->coeffs[k], &el->enabled[k], &el->state[k]);
    omx_biquad_cascade(r, m, 1u, (const float(*)[5]) & er->coeffs[k], &er->enabled[k], &er->state[k]);
    if (s->listen[i]) {
      for (uint32_t j = 0; j < m; j++) {
        s->tap_l[j] = omx_biquad(l[j], s->band[i].b_c, s->listen_l[i]);
        s->tap_r[j] = omx_biquad(r[j], s->band[i].b_c, s->listen_r[i]);
      }
      for (uint32_t j = 0; j < m; j++) {
        s->sum_l[j] += s->tap_l[j];
        s->sum_r[j] += s->tap_r[j];
      }
    }
    omx_band_dyn_process(l, r, m, &s->band[i], &s->st[i]);
  }
  if (listening) {
    memcpy(l, s->sum_l, m * sizeof(float));
    memcpy(r, s->sum_r, m * sizeof(float));
  } else if (s->mix < 1.0f) {
    const float dry = 1.0f - s->mix;
    for (uint32_t j = 0; j < m; j++) omx_wetdry_mix(&l[j], &r[j], s->dry_l[j], s->dry_r[j], l[j], r[j], dry, s->mix);
  }
  OMX_POST(omx_block_finite(l, m) && omx_block_finite(r, m), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the bands in place on out.
 * `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity and moves no state word.
 */
#define OMX_CONTRACT_STAGE "dyneq/instance-run"
static inline void omx_dyneq_instance_run(OmxDyneqInstance *s, const float *in_l, const float *in_r, float *out_l,
                                          float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || !s->enabled) return;
  OMX_PRE(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-in");
  for (uint32_t o = 0; o < n; o += OMX_DYNEQ_INSTANCE_CHUNK) {
    const uint32_t m = n - o < OMX_DYNEQ_INSTANCE_CHUNK ? n - o : OMX_DYNEQ_INSTANCE_CHUNK;
    omx_dyneq_instance_chunk(s, out_l + o, out_r + o, m);
  }
}
#undef OMX_CONTRACT_STAGE

/**
 * Band `i`'s readout, band<i+1>GainDb: the gain its dynamics apply now, dB, signed (a cut negative, a
 * boost positive), inside the range travel. 0 for a band that is off, a band whose dynamics are off,
 * and a refused instance.
 */
static inline float omx_dyneq_instance_band_gain_db(const OmxDyneqInstance *s, uint32_t i) {
  if (!s || !s->ready || i >= OMX_DYNEQ_INSTANCE_BANDS || !s->band[i].enabled) return 0.0f;
  const struct omx_band_dyn_block bk = omx_band_dyn_begin(&s->band[i].dyn);
  return omx_band_dyn_offset_db(&s->band[i].dyn.gc, omx_env_level(&s->st[i].env, &bk.ep));
}

/** The frames of latency the instance introduces: none. */
static inline uint32_t omx_dyneq_instance_latency(const OmxDyneqInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_DYNEQ_INSTANCE_H */
