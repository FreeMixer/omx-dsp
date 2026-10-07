// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_fbs_detect.c — the FBS detector (<omxdsp/analysis/omx_fbs_detect.h>), moved unchanged from
 * openmixer packages/pipewire-native/src/fbs_detect.c (omx-dsp#15).
 *
 * Every reading is in double and in the order core's TS detector computed it, because the
 * identity record holds this file to that detector's decisions bit for bit.
 */
#include <omxdsp/analysis/omx_fbs_detect.h>

#include <math.h>
#include <omxdsp/omx_units.h>
#include <string.h>

static double to_db(double mag) { return omx_lin_to_db_d(mag, OMX_FBS_FLOOR_LIN_DOUBLE); }

/* Round half up, as the TS `Math.round` the recorded depths were computed with. */
static double round_half_up(double x) {
  double r = floor(x);
  if (x - r >= 0.5) r += 1.0;
  if (r == 0.0 && x < 0.0) r = -0.0;
  return r;
}

OmxFbsThresholds omx_fbs_thresholds_default(void) {
  OmxFbsThresholds t;
  memset(&t, 0, sizeof(t));
  t.papr_db = OMX_DEFAULT_THRESHOLDS_PAPR_DB;
  t.pnpr_db = OMX_DEFAULT_THRESHOLDS_PNPR_DB;
  t.phpr_db = OMX_DEFAULT_THRESHOLDS_PHPR_DB;
  t.persist_frames = OMX_DEFAULT_THRESHOLDS_PERSIST_FRAMES;
  t.growth_confirm_frames = OMX_DEFAULT_THRESHOLDS_GROWTH_CONFIRM_FRAMES;
  t.sub_clean_frames = OMX_DEFAULT_THRESHOLDS_SUB_CLEAN_FRAMES;
  t.growth_min_total_rise_db = OMX_DEFAULT_THRESHOLDS_GROWTH_MIN_TOTAL_RISE_DB;
  t.growth_max_single_frame_share = OMX_DEFAULT_THRESHOLDS_GROWTH_MAX_SINGLE_FRAME_SHARE_DOUBLE;
  t.growth_min_db_per_frame = OMX_DEFAULT_THRESHOLDS_GROWTH_MIN_DB_PER_FRAME_DOUBLE;
  t.growth_max_dip_db = OMX_DEFAULT_THRESHOLDS_GROWTH_MAX_DIP_DB_DOUBLE;
  t.pshr_min_db = OMX_DEFAULT_THRESHOLDS_PSHR_MIN_DB;
  return t;
}

static double spectrum_sum(const double *m, uint32_t n) {
  double sum = 0;
  for (uint32_t i = 0; i < n; i++) sum += m[i];
  return sum;
}

static double average_excluding(const double *m, uint32_t n, uint32_t bin, uint32_t guard, double total) {
  const uint32_t lo = bin > guard ? bin - guard : 0;
  const uint32_t hi = bin + guard < n - 1 ? bin + guard : n - 1;
  double band = 0;
  uint32_t in_band = 0;
  for (uint32_t i = lo; i <= hi; i++) {
    band += m[i];
    in_band += 1;
  }
  const double rest = (double)n - (double)in_band;
  return rest > 0 ? (total - band) / rest : 0;
}

static double papr(const double *m, uint32_t n, uint32_t bin, double total) {
  return to_db(m[bin]) - to_db(average_excluding(m, n, bin, OMX_FBS_PAPR_GUARD_BINS, total));
}

static double pnpr(const double *m, uint32_t n, uint32_t bin) {
  double sum = 0;
  uint32_t count = 0;
  for (int32_t d = -OMX_FBS_PNPR_RADIUS_BINS; d <= OMX_FBS_PNPR_RADIUS_BINS; d++) {
    if (d >= -OMX_FBS_PNPR_GUARD_BINS && d <= OMX_FBS_PNPR_GUARD_BINS) continue;
    const int64_t i = (int64_t)bin + d;
    if (i < 0 || i >= (int64_t)n) continue;
    sum += m[i];
    count += 1;
  }
  return count > 0 ? to_db(m[bin]) - to_db(sum / (double)count) : INFINITY;
}

static double phpr(const double *m, uint32_t n, uint32_t bin) {
  double strongest = 0;
  for (uint32_t h = 2; h <= OMX_FBS_PHPR_HARMONICS + 1; h++) {
    const uint64_t i = (uint64_t)bin * h;
    if (i >= n) break;
    if (m[i] > strongest) strongest = m[i];
  }
  return to_db(m[bin]) - to_db(strongest);
}

double omx_fbs_pshr(const double *mags, uint32_t nbins, uint32_t bin) {
  double strongest = 0;
  for (uint32_t k = 2; k <= 3; k++) {
    const double center = (double)bin / (double)k;
    double lo = floor(center - OMX_SUBHARMONIC_RADIUS_BINS);
    if (lo < 1) lo = 1;
    double hi = ceil(center + OMX_SUBHARMONIC_RADIUS_BINS);
    const double self = (double)bin - OMX_FBS_PSHR_SELF_SKIP_BINS;
    if (self < hi) hi = self;
    for (double i = lo; i <= hi; i += 1) {
      const double v = (uint32_t)i < nbins ? mags[(uint32_t)i] : 0;
      if (v > strongest) strongest = v;
    }
  }
  if (strongest == 0) return INFINITY;
  return to_db(mags[bin]) - to_db(strongest);
}

static int is_candidate(const double *m, uint32_t n, uint32_t bin, const OmxFbsThresholds *t, double total) {
  return pnpr(m, n, bin) >= t->pnpr_db && phpr(m, n, bin) >= t->phpr_db && papr(m, n, bin, total) >= t->papr_db;
}

static double growth_slope(const double *y, uint32_t n) {
  if (n < 2) return 0;
  double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
  for (uint32_t i = 0; i < n; i++) {
    const double x = (double)i;
    sum_x += x;
    sum_y += y[i];
    sum_xy += x * y[i];
    sum_xx += x * x;
  }
  const double denom = (double)n * sum_xx - sum_x * sum_x;
  return denom == 0 ? 0 : ((double)n * sum_xy - sum_x * sum_y) / denom;
}

int omx_fbs_is_growing(const double *levels_db, uint32_t n, const OmxFbsThresholds *t) {
  if (n < 2) return 0;
  const double rise = levels_db[n - 1] - levels_db[0];
  if (rise < t->growth_min_total_rise_db) return 0;
  double max_hop = -INFINITY;
  for (uint32_t i = 1; i < n; i++) {
    const double hop = levels_db[i] - levels_db[i - 1];
    if (hop < -t->growth_max_dip_db) return 0;
    if (hop > max_hop) max_hop = hop;
  }
  if (max_hop > t->growth_max_single_frame_share * rise) return 0;
  return growth_slope(levels_db, n) >= t->growth_min_db_per_frame;
}

static int is_saturated_plateau(const double *levels, uint32_t n, double floor_db, double max_dip, uint32_t min_frames) {
  if (n < min_frames) return 0;
  const double *w = levels + (n - min_frames);
  for (uint32_t i = 0; i < min_frames; i++) {
    if (w[i] < floor_db) return 0;
    if (i > 0 && w[i] - w[i - 1] < -max_dip) return 0;
  }
  return 1;
}

static void push_capped(double *buf, uint32_t *len, uint32_t cap, double v) {
  if (*len == cap) {
    memmove(buf, buf + 1, (size_t)(cap - 1) * sizeof(double));
    buf[cap - 1] = v;
  } else {
    buf[(*len)++] = v;
  }
}

static OmxFbsCandidate candidate_of(const OmxFbsDetector *d, const double *m, uint32_t n, uint32_t bin,
                                    uint32_t persistence, double total) {
  OmxFbsCandidate c;
  c.bin = bin;
  c.freq_hz = ((double)bin * d->sample_rate) / (double)d->fft_size;
  const double r = round_half_up(papr(m, n, bin, total));
  c.depth_db = -(r < OMX_FBS_MAX_NOTCH_DEPTH_DB ? r : (double)OMX_FBS_MAX_NOTCH_DEPTH_DB);
  c.q = OMX_FBS_DEFAULT_NOTCH_Q;
  c.persistence = persistence;
  c.level_db = to_db(m[bin]);
  return c;
}

void omx_fbs_init(OmxFbsDetector *d) {
  d->n_free = d->n_slots;
  d->frame = 0;
  d->seq = 0;
  for (uint32_t i = 0; i < d->nbins_cap; i++) d->bin_slot[i] = -1;
  for (uint32_t i = 0; i < d->n_slots; i++) {
    d->slots[i].bin = -1;
    d->free_stack[i] = d->n_slots - 1 - i;
  }
}

void omx_fbs_push(OmxFbsDetector *d, const double *m, uint32_t n, OmxFbsResult *out) {
  const OmxFbsThresholds *t = &d->t;
  if (n > d->nbins_cap) n = d->nbins_cap;
  d->frame += 1;
  out->n_confirmed = 0;
  out->n_snapshot = 0;
  out->n_active = 0;
  out->untracked = 0;
  const double total = spectrum_sum(m, n);

  for (uint32_t bin = 1; bin < n; bin++) {
    if (!is_candidate(m, n, bin, t, total)) continue;
    int32_t si = d->bin_slot[bin];
    if (si < 0) {
      if (d->n_free == 0) {
        out->untracked += 1;
        continue;
      }
      si = (int32_t)d->free_stack[--d->n_free];
      OmxFbsSlot *fresh = &d->slots[si];
      memset(fresh, 0, sizeof(*fresh));
      fresh->bin = (int32_t)bin;
      d->bin_slot[bin] = si;
    }
    OmxFbsSlot *s = &d->slots[si];
    s->seen_frame = d->frame;
    out->active[out->n_active++] = bin;
    s->streak += 1;

    const double level = to_db(m[bin]);
    push_capped(s->hist, &s->n_hist, OMX_GROWTH_WINDOW_FRAMES, level);
    push_capped(s->deep, &s->n_deep, OMX_DEEP_PLATEAU_FRAMES, level);

    s->growing_run = omx_fbs_is_growing(s->hist, s->n_hist, t) ? s->growing_run + 1 : 0;
    s->sub_clean_run = omx_fbs_pshr(m, n, bin) >= t->pshr_min_db ? s->sub_clean_run + 1 : 0;

    const int by_ramp = s->streak >= t->persist_frames && s->growing_run >= t->growth_confirm_frames &&
                        s->sub_clean_run >= t->sub_clean_frames;
    const int by_plateau = t->plateau_on &&
                           is_saturated_plateau(s->hist, s->n_hist, t->plateau_floor_db, t->growth_max_dip_db,
                                                t->persist_frames) &&
                           s->sub_clean_run >= t->sub_clean_frames;
    int by_trend = 0;
    if (t->trend_on && s->n_deep >= OMX_DEEP_PLATEAU_FRAMES &&
        s->deep[s->n_deep - 1] - s->deep[0] >= t->trend_min_rise_db) {
      by_trend = 1;
      for (uint32_t i = 1; i < s->n_deep; i++) {
        if (s->deep[i] - s->deep[i - 1] < -t->growth_max_dip_db) {
          by_trend = 0;
          break;
        }
      }
      by_trend = by_trend && s->sub_clean_run >= OMX_DEEP_PLATEAU_FRAMES;
    }
    const int by_deep = t->deep_plateau_on &&
                        is_saturated_plateau(s->deep, s->n_deep, t->deep_plateau_floor_db, t->growth_max_dip_db / 2,
                                             OMX_DEEP_PLATEAU_FRAMES) &&
                        s->sub_clean_run >= OMX_DEEP_PLATEAU_FRAMES;

    if (!s->confirmed && (by_ramp || by_plateau || by_deep || by_trend)) {
      s->confirmed = 1;
      s->confirm_seq = ++d->seq;
      out->confirmed[out->n_confirmed++] = candidate_of(d, m, n, bin, s->streak, total);
    }
  }

  for (uint32_t i = 0; i < d->n_slots; i++) {
    OmxFbsSlot *s = &d->slots[i];
    if (s->bin < 0 || s->seen_frame == d->frame) continue;
    d->bin_slot[s->bin] = -1;
    s->bin = -1;
    d->free_stack[d->n_free++] = i;
  }

  for (uint32_t i = 0; i < d->n_slots; i++) {
    const OmxFbsSlot *s = &d->slots[i];
    if (s->bin < 0 || !s->confirmed) continue;
    const OmxFbsCandidate c = candidate_of(d, m, n, (uint32_t)s->bin, s->streak, total);
    uint32_t k = out->n_snapshot++;
    while (k > 0 && d->slots[d->bin_slot[out->snapshot[k - 1].bin]].confirm_seq > s->confirm_seq) {
      out->snapshot[k] = out->snapshot[k - 1];
      k--;
    }
    out->snapshot[k] = c;
  }
}
