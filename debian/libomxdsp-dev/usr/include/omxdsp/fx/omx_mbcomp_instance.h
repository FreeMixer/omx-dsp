/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_mbcomp_instance.h — the multiband compressor as a plugin instance: the shell's core, with no
 * format in it. The core is fx/omx_mbcomp.h's `omx_mbcomp_process`, the LR4 tree with one RMS
 * compressor per band, and this file adds no DSP to it. It adds what a host's port model needs:
 *
 *   1. RESOLVE. {@link omx_mbcomp_instance_resolve} takes the mbcomp kernel's contract controls in
 *      their declared order (data/kernels/mbcomp.json): the band count, the corners, per band the
 *      threshold, ratio, attack, release, make-up, solo and bypass, then the mix and the output gain.
 *      Every travel is clamped into its declared range, a non-finite word reading as its default (a
 *      corner as the come-up corner of the band count); a choice outside its ids reads as its default.
 *      The corners are made strictly rising (one at or below the one before it moves just above it)
 *      and the tree is redesigned only when the band count or a corner in use moved.
 *   2. THE BANDS. Each band is a feed-forward RMS compressor with a hard knee; a bypassed band is the
 *      kernel's unity band. The mix is each band's own dry share (omx_dyn.dry), so the dry signal is
 *      the tree at rest and the blend is in phase at every frequency. Solo leaves the bands that are
 *      not soloed out of the sum (omx_mbcomp.muted) while any band in use is soloed; a solo past the
 *      band count is ignored.
 *   3. IN -> OUT. The kernel works in place; run() copies in to out where they differ (they may alias),
 *      runs the kernel on out, and applies the output gain when it is not 0 dB.
 *
 * READOUTS. {@link omx_mbcomp_instance_band_gr_db} is band<n>GainReductionDb: omx_mbcomp_band_gr_db().
 *
 * LATENCY IS PUBLISHED: OMX_OVS_LATENCY_4X while the stage runs at 4x (omx_mbcomp_factor()), else 0.
 * Bypass is the identity and keeps every state word; a change of the stage's factor re-arms the band
 * compressors at the new one. Nothing is allocated.
 */
#ifndef OMX_MBCOMP_INSTANCE_H
#define OMX_MBCOMP_INSTANCE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_mbcomp.h>
#include <omxdsp/omx_param.h>

#define OMX_MBCOMP_INSTANCE_CHANNELS 2

/** One instance: the atom, the stage's state, the corners the tree was designed at and the output gain. */
typedef struct {
  float sr;
  struct omx_mbcomp atom;
  struct omx_mbcomp_state st;
  float hz[OMX_MBC_MAX_XOVERS]; /**< The corners in use as the tree was last designed. */
  int designed;
  uint32_t factor; /**< The factor the band compressors were armed at. */
  float out_lin;   /**< The output gain, linear; exactly 1 at 0 dB. */
  int ready;
} OmxMbcompInstance;

/**
 * Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. The stage comes up at its declared defaults (three bands at the come-up corners, every band at
 * its come-up compressor), engaged. Allocates nothing.
 */
static inline void omx_mbcomp_instance_resolve(OmxMbcompInstance *s, int bypass, float bands,
                                               const float xover_hz[OMX_MBC_MAX_XOVERS],
                                               const float threshold_db[OMX_MBC_MAX_BANDS],
                                               const float ratio[OMX_MBC_MAX_BANDS],
                                               const float attack_ms[OMX_MBC_MAX_BANDS],
                                               const float release_ms[OMX_MBC_MAX_BANDS],
                                               const float makeup_db[OMX_MBC_MAX_BANDS], const int solo[OMX_MBC_MAX_BANDS],
                                               const int band_bypass[OMX_MBC_MAX_BANDS], float mix_pct, float output_db);

static inline int omx_mbcomp_instance_init(OmxMbcompInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  float c_hz[OMX_MBC_MAX_XOVERS], c_thr[OMX_MBC_MAX_BANDS], c_rat[OMX_MBC_MAX_BANDS], c_att[OMX_MBC_MAX_BANDS],
      c_rel[OMX_MBC_MAX_BANDS], c_mk[OMX_MBC_MAX_BANDS];
  int c_so[OMX_MBC_MAX_BANDS], c_by[OMX_MBC_MAX_BANDS];
  for (uint32_t k = 0; k < OMX_MBC_MAX_XOVERS; k++) c_hz[k] = NAN;
  for (uint32_t k = 0; k < OMX_MBC_MAX_BANDS; k++) {
    c_thr[k] = (float)OMX_MBCOMP_THRESHOLD_DB_RANGE_DEFAULT;
    c_rat[k] = (float)OMX_MBCOMP_RATIO_RANGE_DEFAULT;
    c_att[k] = (float)OMX_MBCOMP_ATTACK_MS_RANGE_DEFAULT;
    c_rel[k] = (float)OMX_MBCOMP_RELEASE_MS_RANGE_DEFAULT;
    c_mk[k] = (float)OMX_MBCOMP_MAKEUP_DB_RANGE_DEFAULT;
    c_so[k] = (int)OMX_MBCOMP_SOLOS_DEFAULT;
    c_by[k] = (int)OMX_MBCOMP_BAND_BYPASSES_DEFAULT;
  }
  omx_mbcomp_instance_resolve(s, 0, (float)OMX_MBCOMP_BANDS_RANGE_DEFAULT, c_hz, c_thr, c_rat, c_att, c_rel, c_mk, c_so, c_by,
                              (float)OMX_MBCOMP_MIX_PCT_RANGE_DEFAULT, (float)OMX_MBCOMP_OUTPUT_DB_RANGE_DEFAULT);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the mbcomp kernel's contract controls in their declared order: the band count, the
 * corners (Hz, OMX_MBC_MAX_XOVERS of them, the first `bands - 1` in use), per band (OMX_MBC_MAX_BANDS
 * of each) the threshold (dB), ratio, attack and release (ms), make-up (dB), solo and bypass
 * (MBCOMP_SOLOS, MBCOMP_BAND_BYPASSES indices), then the mix (percent) and the output gain (dB).
 */
#define OMX_CONTRACT_STAGE "mbcomp/instance-resolve"
static inline void omx_mbcomp_instance_resolve(OmxMbcompInstance *s, int bypass, float bands,
                                               const float xover_hz[OMX_MBC_MAX_XOVERS],
                                               const float threshold_db[OMX_MBC_MAX_BANDS],
                                               const float ratio[OMX_MBC_MAX_BANDS],
                                               const float attack_ms[OMX_MBC_MAX_BANDS],
                                               const float release_ms[OMX_MBC_MAX_BANDS],
                                               const float makeup_db[OMX_MBC_MAX_BANDS], const int solo[OMX_MBC_MAX_BANDS],
                                               const int band_bypass[OMX_MBC_MAX_BANDS], float mix_pct, float output_db) {
  if (!s || !s->ready || !xover_hz || !threshold_db || !ratio || !attack_ms || !release_ms || !makeup_db || !solo ||
      !band_bypass)
    return;
  struct omx_mbcomp *m = &s->atom;
  const float nbf = omx_clamp_or(bands, (float)OMX_MBCOMP_BANDS_RANGE_MIN, (float)OMX_MBCOMP_BANDS_RANGE_MAX,
                                 (float)OMX_MBCOMP_BANDS_RANGE_DEFAULT);
  const uint32_t nb = (uint32_t)(nbf + 0.5f);
  const float *come_up = omx_mbcomp_come_up_hz(nb);
  float hz[OMX_MBC_MAX_XOVERS];
  int moved = !s->designed || nb != m->bands;
  for (uint32_t k = 0; k + 1u < nb; k++) {
    const float corner = xover_hz[k] - xover_hz[k] == 0.0f
                             ? omx_clampf(xover_hz[k], (float)OMX_MBCOMP_XOVER_HZ_RANGE_MIN, (float)OMX_MBCOMP_XOVER_HZ_RANGE_MAX)
                             : come_up[k];
    hz[k] = k > 0u && !(corner > hz[k - 1u]) ? nextafterf(hz[k - 1u], INFINITY) : corner;
    moved |= hz[k] != s->hz[k];
  }
  if (moved) {
    omx_mbcomp_design(m, nb, hz, (double)s->sr);
    for (uint32_t k = 0; k + 1u < nb; k++) s->hz[k] = hz[k];
    s->designed = 1;
  }
  const float mix_share = omx_clamp_or(mix_pct, (float)OMX_MBCOMP_MIX_PCT_RANGE_MIN, (float)OMX_MBCOMP_MIX_PCT_RANGE_MAX,
                                       (float)OMX_MBCOMP_MIX_PCT_RANGE_DEFAULT);
  uint32_t soloed = 0u;
  for (uint32_t k = 0; k < OMX_MBC_MAX_BANDS; k++) {
    const float thr = omx_clamp_or(threshold_db[k], (float)OMX_MBCOMP_THRESHOLD_DB_RANGE_MIN,
                                   (float)OMX_MBCOMP_THRESHOLD_DB_RANGE_MAX, (float)OMX_MBCOMP_THRESHOLD_DB_RANGE_DEFAULT);
    const float rat = omx_clamp_or(ratio[k], (float)OMX_MBCOMP_RATIO_RANGE_MIN, (float)OMX_MBCOMP_RATIO_RANGE_MAX,
                                   (float)OMX_MBCOMP_RATIO_RANGE_DEFAULT);
    const float att = omx_clamp_or(attack_ms[k], (float)OMX_MBCOMP_ATTACK_MS_RANGE_MIN, (float)OMX_MBCOMP_ATTACK_MS_RANGE_MAX,
                                   (float)OMX_MBCOMP_ATTACK_MS_RANGE_DEFAULT);
    const float rel = omx_clamp_or(release_ms[k], (float)OMX_MBCOMP_RELEASE_MS_RANGE_MIN,
                                   (float)OMX_MBCOMP_RELEASE_MS_RANGE_MAX, (float)OMX_MBCOMP_RELEASE_MS_RANGE_DEFAULT);
    const float mk = omx_clamp_or(makeup_db[k], (float)OMX_MBCOMP_MAKEUP_DB_RANGE_MIN, (float)OMX_MBCOMP_MAKEUP_DB_RANGE_MAX,
                                  (float)OMX_MBCOMP_MAKEUP_DB_RANGE_DEFAULT);
    const int so = solo[k] == OMX_MBCOMP_SOLOS_ON ? 1 : 0;
    const int band_off = band_bypass[k] == OMX_MBCOMP_BAND_BYPASSES_ON ? 1 : 0;
    struct omx_dyn *d = &m->band[k];
    memset(d, 0, sizeof *d);
    d->enabled = !band_off;
    d->gc.mode = OMX_DYN_ABOVE;
    d->gc.thresh_db = thr;
    d->gc.ratio = rat;
    d->gc.makeup_lin = omx_db_to_lin(mk);
    d->detect = OMX_DETECT_RMS;
    d->attack_ms = att;
    d->attack_coeff = omx_pole_from_time_ms(att, s->sr);
    d->release_coeff = omx_pole_from_time_ms(rel, s->sr);
    d->dry = omx_dyn_dry_share(mix_share);
    soloed |= (uint32_t)so << k;
  }
  /* Only the bands in use count: a solo left on past the band count mutes nothing. */
  soloed &= (1u << nb) - 1u;
  m->muted = soloed ? ~soloed & ((1u << nb) - 1u) : 0u;
  m->ovs_mode = OMX_DYN_OVS_AUTO;
  const float out_db = omx_clamp_or(output_db, (float)OMX_MBCOMP_OUTPUT_DB_RANGE_MIN, (float)OMX_MBCOMP_OUTPUT_DB_RANGE_MAX,
                                    (float)OMX_MBCOMP_OUTPUT_DB_RANGE_DEFAULT);
  s->out_lin = out_db == 0.0f ? 1.0f : omx_db_to_lin(out_db);
  m->enabled = bypass ? 0 : 1;
  const uint32_t f = omx_mbcomp_factor(m);
  if (f != s->factor) {
    for (uint32_t k = 0; k < OMX_MBC_MAX_BANDS; k++) omx_dyn_state_init(&s->st.dyn[k], f);
    s->factor = f;
  }
  OMX_POST(m->bands >= OMX_MBC_MIN_BANDS && m->bands <= OMX_MBC_MAX_BANDS, "band-count-declared");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the stage in place on out,
 * apply the output gain. `in_*` and `out_*` may alias. Not ready, or bypassed, is the identity.
 */
#define OMX_CONTRACT_STAGE "mbcomp/instance-run"
static inline void omx_mbcomp_instance_run(OmxMbcompInstance *s, const float *in_l, const float *in_r, float *out_l,
                                           float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || !s->atom.enabled) return;
  omx_mbcomp_process(out_l, out_r, n, &s->atom, &s->st);
  if (s->out_lin != 1.0f)
    for (uint32_t i = 0; i < n; i++) {
      out_l[i] *= s->out_lin;
      out_r[i] *= s->out_lin;
    }
}
#undef OMX_CONTRACT_STAGE

/** Band `k`'s readout, band<k+1>GainReductionDb, dB, at most 0: 0 past the band count, for a bypassed
 * band and for a refused instance. */
static inline float omx_mbcomp_instance_band_gr_db(const OmxMbcompInstance *s, uint32_t k) {
  if (!s || !s->ready || k >= s->atom.bands) return 0.0f;
  return omx_mbcomp_band_gr_db(&s->atom, &s->st, k);
}

/** The frames of latency the instance reports: OMX_OVS_LATENCY_4X while the stage runs at 4x, else 0. */
static inline uint32_t omx_mbcomp_instance_latency(const OmxMbcompInstance *s) {
  return s && s->ready && omx_mbcomp_factor(&s->atom) == 4u ? (uint32_t)OMX_OVS_LATENCY_4X : 0u;
}

#endif /* OMX_MBCOMP_INSTANCE_H */
