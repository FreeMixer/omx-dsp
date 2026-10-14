// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_room.c — room analysis (<omxdsp/analysis/omx_room.h>), moved from OpenMixer core's
 * TypeScript (room-transfer.ts, room-impulse.ts, room-rt60.ts, room-modes.ts and room-mode-eq.ts).
 *
 * Every reading is in double and in the order core's TypeScript computed it, so the parity table
 * (test/analysis/room_ts.test.c) holds this file to core's answers within the stated tolerances.
 * Where core sorted with Array.prototype.sort (a stable sort), the sorts here are stable too.
 */
#include <omxdsp/analysis/omx_room.h>

#include <math.h>
#include <string.h>

#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_fft.h>

/* Round half up, as the TS `Math.round` core computed with. */
static double round_half_up(double x) {
  double r = floor(x);
  if (x - r >= 0.5) r += 1.0;
  return r;
}

/* 20·log10(x) floored, as core's `linearToDbFloored`; `linearGainToDb` reads −∞ at or under 0. */
static double lin_to_db_floored(double x, double floor_db) {
  const double db = x <= 0.0 ? -INFINITY : 20.0 * log10(x);
  return floor_db > db ? floor_db : db;
}

/* Ascending in place, ties in either order: a heap sort, O(n log n), no allocation. */
static void sort_ascending(double *v, uint32_t n) {
  if (n < 2u) return;
  for (uint32_t start = n / 2u; start-- > 0u;) {
    uint32_t root = start;
    for (;;) {
      uint32_t child = 2u * root + 1u;
      if (child >= n) break;
      if (child + 1u < n && v[child] < v[child + 1u]) child++;
      if (!(v[root] < v[child])) break;
      const double t = v[root]; v[root] = v[child]; v[child] = t;
      root = child;
    }
  }
  for (uint32_t end = n - 1u; end > 0u; end--) {
    const double t = v[0]; v[0] = v[end]; v[end] = t;
    uint32_t root = 0;
    for (;;) {
      uint32_t child = 2u * root + 1u;
      if (child >= end) break;
      if (child + 1u < end && v[child] < v[child + 1u]) child++;
      if (!(v[root] < v[child])) break;
      const double s = v[root]; v[root] = v[child]; v[child] = s;
      root = child;
    }
  }
}

#define OMX_CONTRACT_STAGE "analysis/room-transfer"

enum omx_analysis_refusal omx_room_transfer(const OmxXspec *a, uint32_t min_frames, double low_hz, double high_hz,
                                            OmxRoomAccum *out) {
  out->frames = a->frames;
  out->sample_rate = a->sample_rate;
  out->fft_size = a->fft_size;
  out->k_lo = 0;
  out->k_hi = 0;
  if (a->frames < min_frames) return OMX_ANALYSIS_TOO_FEW_FRAMES;
  if (a->mismatch) return OMX_ANALYSIS_SHAPE_MISMATCH;
  uint32_t k_lo, k_hi;
  if (!omx_xspec_band(a->fft_size, a->sample_rate, low_hz, high_hz, &k_lo, &k_hi)) return OMX_ANALYSIS_EMPTY_BAND;
  out->k_lo = k_lo;
  out->k_hi = k_hi;
  memset(out->re, 0, a->bins * sizeof(double));
  memset(out->im, 0, a->bins * sizeof(double));
  memset(out->coherence, 0, a->bins * sizeof(double));
  int any = 0;
  for (uint32_t k = k_lo; k <= k_hi; k++) {
    const double den = a->pxx[k] * a->pyy[k];
    if (!(a->pyy[k] > 0.0) || !(den > 0.0)) continue;
    /* H1(k) = Sxy(k) / Pyy(k): the reference's own power deconvolved out. */
    out->re[k] = a->xr[k] / a->pyy[k];
    out->im[k] = a->xi[k] / a->pyy[k];
    const double cross2 = a->xr[k] * a->xr[k] + a->xi[k] * a->xi[k];
    double c = cross2 / den;
    c = 0.0 > c ? 0.0 : c;
    out->coherence[k] = 1.0 < c ? 1.0 : c;
    any = 1;
  }
  return any ? OMX_ANALYSIS_OK : OMX_ANALYSIS_NO_SHARED_ENERGY;
}

uint32_t omx_room_ir_fft_size(uint32_t n) {
  uint32_t size = 2u;
  while (size < n) size <<= 1;
  return size;
}

enum omx_analysis_refusal omx_room_transfer_from_ir(const double *ir, uint32_t n, double sample_rate, double low_hz,
                                                    double high_hz, double *scratch_re, double *scratch_im,
                                                    OmxRoomAccum *out) {
  const uint32_t size = omx_room_ir_fft_size(n);
  double *re = scratch_re, *im = scratch_im;
  memset(re, 0, size * sizeof(double));
  memset(im, 0, size * sizeof(double));
  for (uint32_t i = 0; i < n; i++) re[i] = ir[i];
  omx_fft_radix2_d(re, im, size, 0);

  const uint32_t bins = size >> 1;
  out->sample_rate = sample_rate;
  out->fft_size = size;
  out->frames = 0;
  uint32_t k_lo, k_hi;
  if (!omx_xspec_band(size, sample_rate, low_hz, high_hz, &k_lo, &k_hi)) {
    out->k_lo = 0;
    out->k_hi = 0;
    return OMX_ANALYSIS_EMPTY_BAND;
  }
  out->k_lo = k_lo;
  out->k_hi = k_hi;
  memset(out->re, 0, bins * sizeof(double));
  memset(out->im, 0, bins * sizeof(double));
  memset(out->coherence, 0, bins * sizeof(double));
  int any = 0;
  for (uint32_t k = k_lo; k <= k_hi; k++) {
    out->re[k] = re[k];
    out->im[k] = im[k];
    out->coherence[k] = 1.0;
    if (re[k] != 0.0 || im[k] != 0.0) any = 1;
  }
  out->frames = 1;
  return any ? OMX_ANALYSIS_OK : OMX_ANALYSIS_NO_SHARED_ENERGY;
}

void omx_room_unwrap(double *rad, uint32_t n) {
  if (n == 0u) return;
  double prev_raw = rad[0];
  for (uint32_t i = 1; i < n; i++) {
    const double raw = rad[i];
    double d = raw - prev_raw;
    while (d > M_PI) d -= 2.0 * M_PI;
    while (d < -M_PI) d += 2.0 * M_PI;
    rad[i] = rad[i - 1] + d;
    prev_raw = raw;
  }
}

void omx_room_curve(const OmxRoomAccum *acc, double floor_db, double *hz, double *magnitude_db, double *phase_deg,
                    double *coherence) {
  const uint32_t m = acc->k_hi - acc->k_lo + 1u;
  for (uint32_t i = 0; i < m; i++) {
    const uint32_t k = acc->k_lo + i;
    phase_deg[i] = atan2(acc->im[k], acc->re[k]);
  }
  omx_room_unwrap(phase_deg, m);
  for (uint32_t i = 0; i < m; i++) {
    const uint32_t k = acc->k_lo + i;
    hz[i] = ((double)k * acc->sample_rate) / (double)acc->fft_size;
    magnitude_db[i] = lin_to_db_floored(hypot(acc->re[k], acc->im[k]), floor_db);
    phase_deg[i] = (phase_deg[i] * 180.0) / M_PI;
    coherence[i] = acc->coherence[k];
  }
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "analysis/room-impulse"

/* The main lobe of the analytic envelope under the Hann taper: twice a rectangular band
 * selection's `n / (k_hi − k_lo + 1)`. No two accepted arrivals sit closer. */
static uint32_t impulse_lobe(const OmxRoomAccum *acc) {
  const double band_bins = acc->k_hi >= acc->k_lo ? (double)(acc->k_hi - acc->k_lo + 1u) : 1.0;
  const double lobe = round_half_up((2.0 * (double)acc->fft_size) / band_bins);
  return lobe > 1.0 ? (uint32_t)lobe : 1u;
}

static double impulse_horizon(const OmxRoomAccum *acc, double window_ms) {
  const double sample_ms = 1000.0 / acc->sample_rate;
  return round_half_up(window_ms / sample_ms);
}

uint32_t omx_room_impulse_capacity(const OmxRoomAccum *acc, double window_ms) {
  double horizon = impulse_horizon(acc, window_ms);
  if (horizon > (double)acc->fft_size) horizon = (double)acc->fft_size;
  if (!(horizon >= 0.0)) horizon = 0.0;
  return (uint32_t)horizon / impulse_lobe(acc) + 1u;
}

enum omx_analysis_refusal omx_room_impulse(const OmxRoomAccum *acc, double window_ms, double floor_db,
                                           double *scratch_re, double *scratch_im, uint32_t *direct,
                                           OmxRoomReflection *out, uint32_t cap, uint32_t *n_out) {
  OMX_PRE(cap >= omx_room_impulse_capacity(acc, window_ms), "capacity-covers-the-horizon");
  const uint32_t n = acc->fft_size;
  double *re = scratch_re, *im = scratch_im;
  *n_out = 0;
  *direct = 0;
  memset(re, 0, n * sizeof(double));
  memset(im, 0, n * sizeof(double));
  /* A Hann taper across the band, so the band selection's edges do not ring as false arrivals. */
  const uint32_t span = acc->k_hi - acc->k_lo;
  for (uint32_t k = acc->k_lo; k <= acc->k_hi; k++) {
    const double taper = span > 0u ? 0.5 * (1.0 - cos((2.0 * M_PI * (double)(k - acc->k_lo)) / (double)span)) : 1.0;
    re[k] = acc->re[k] * taper;
    im[k] = acc->im[k] * taper;
  }
  omx_fft_radix2_d(re, im, n, 1);

  /* The analytic envelope, into `re`; the global peak is the direct arrival. */
  double peak = 0.0;
  uint32_t peak_idx = 0;
  for (uint32_t i = 0; i < n; i++) {
    const double v = hypot(re[i], im[i]);
    re[i] = v;
    if (v > peak) {
      peak = v;
      peak_idx = i;
    }
  }
  if (!(peak > 0.0)) return OMX_ANALYSIS_NO_SHARED_ENERGY;
  *direct = peak_idx;
  const double *env = re;
  const double floor = peak * pow(10.0, floor_db / 20.0);
  const double sample_ms = 1000.0 / acc->sample_rate;
  double horizon = impulse_horizon(acc, window_ms);
  if (!(horizon >= 0.0)) horizon = 0.0; /* a negative or NaN horizon names no sample */
  const double room = (double)(n - 1u - peak_idx);
  if (room < horizon) horizon = room;
  const uint32_t lobe = impulse_lobe(acc);
  const uint32_t first = peak_idx + lobe;
  const uint32_t last = peak_idx + (uint32_t)horizon;

  /* Candidates: every sample past the direct lobe, inside the horizon, at or above the floor;
   * `im` marks them. Greedy non-maximum suppression: the highest remaining candidate (the earliest
   * of equals, as core's stable sort leaves them) is accepted and suppresses every candidate
   * within one lobe of it, until none remain. */
  for (uint32_t i = first; i <= last && i < n; i++) im[i] = env[i] >= floor ? 1.0 : 0.0;
  for (;;) {
    double best = -1.0;
    uint32_t best_i = 0;
    int found = 0;
    for (uint32_t i = first; i <= last && i < n; i++) {
      if (im[i] != 0.0 && env[i] > best) {
        best = env[i];
        best_i = i;
        found = 1;
      }
    }
    if (!found || *n_out >= cap) break;
    out[*n_out].samples = best_i;
    (*n_out)++;
    const uint32_t lo = best_i >= lobe - 1u ? best_i - (lobe - 1u) : 0u;
    for (uint32_t i = lo; i <= best_i + (lobe - 1u) && i < n; i++) im[i] = 0.0;
  }
  /* Arrival order: insertion sort of the few accepted. */
  for (uint32_t i = 1; i < *n_out; i++) {
    const uint32_t s = out[i].samples;
    uint32_t j = i;
    while (j > 0u && out[j - 1u].samples > s) {
      out[j].samples = out[j - 1u].samples;
      j--;
    }
    out[j].samples = s;
  }
  for (uint32_t r = 0; r < *n_out; r++) {
    const uint32_t idx = out[r].samples;
    const uint32_t i = idx - peak_idx;
    const double arrival_ms = (double)i * sample_ms;
    const double ratio = env[idx] / peak;
    out[r].samples = i;
    out[r].arrival_ms = arrival_ms;
    out[r].distance_m = arrival_ms * 0.343;
    out[r].level_db = ratio <= 0.0 ? -INFINITY : 20.0 * log10(ratio);
  }
  return OMX_ANALYSIS_OK;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "analysis/room-rt60"

/* The Schroeder curve's first downward crossing of `target`, linearly interpolated; −1 if none. */
static double crossing_index(const double *db, uint32_t n, double target) {
  for (uint32_t i = 1; i < n; i++) {
    if (db[i] <= target) {
      const double a = db[i - 1u], b = db[i];
      const double t = a == b ? 0.0 : (a - target) / (a - b);
      return (double)(i - 1u) + t;
    }
  }
  return -1.0;
}

enum omx_analysis_refusal omx_room_rt60_band(const double *samples, uint32_t n, double sample_rate, double centre_hz,
                                             double upper_db, double lower_db, double *scratch, double *t60_ms) {
  if (n == 0u) return OMX_ANALYSIS_NO_SIGNAL;
  /* Enough samples for the filter to settle and a decay to be seen: twenty periods of the centre. */
  const double min_samples = ceil((sample_rate / centre_hz) * 20.0);
  if ((double)n < min_samples) return OMX_ANALYSIS_TOO_SHORT;

  /* Three cascaded cookbook bandpass sections at octave Q, in direct form I. */
  const double bandwidth = centre_hz * (M_SQRT2 - 1.0 / M_SQRT2);
  const double q = centre_hz / bandwidth;
  double c[5];
  omx_eq_design(OMX_EQ_BANDPASS, centre_hz, q, 0.0, sample_rate, c);
  const double *src = samples;
  for (int s = 0; s < 3; s++) {
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const double x0 = src[i];
      const double y0 = c[0] * x0 + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
      scratch[i] = y0;
      x2 = x1;
      x1 = x0;
      y2 = y1;
      y1 = y0;
    }
    src = scratch;
  }

  /* The Schroeder backward integral of the squared band, in dB re its total. */
  double running = 0.0;
  for (uint32_t i = n; i-- > 0u;) {
    running += scratch[i] * scratch[i];
    scratch[i] = running;
  }
  const double total = scratch[0];
  const double denom = total > 1e-300 ? total : 1e-300;
  for (uint32_t i = 0; i < n; i++) {
    const double r = scratch[i] / denom;
    scratch[i] = 10.0 * log10(r > 1e-300 ? r : 1e-300);
  }

  const double idx_upper = crossing_index(scratch, n, upper_db);
  const double idx_lower = crossing_index(scratch, n, lower_db);
  if (idx_upper < 0.0 || idx_lower < 0.0) return OMX_ANALYSIS_INSUFFICIENT_DECAY;
  const double i_start = ceil(idx_upper), i_end = floor(idx_lower);
  if (i_end <= i_start) return OMX_ANALYSIS_INSUFFICIENT_DECAY;

  /* The span's dB/s slope by ordinary least squares over every sample in it. */
  double cnt = 0.0, sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
  for (uint32_t i = (uint32_t)i_start; i <= (uint32_t)i_end; i++) {
    const double t = (double)i / sample_rate;
    const double y = scratch[i];
    cnt += 1.0;
    sum_x += t;
    sum_y += y;
    sum_xy += t * y;
    sum_xx += t * t;
  }
  const double mean_x = sum_x / cnt, mean_y = sum_y / cnt;
  const double num = sum_xy - cnt * mean_x * mean_y;
  const double den = sum_xx - cnt * mean_x * mean_x;
  const double slope = den == 0.0 ? NAN : num / den;
  if (!(slope < 0.0)) return OMX_ANALYSIS_INSUFFICIENT_DECAY;
  *t60_ms = (60.0 / -slope) * 1000.0;
  return OMX_ANALYSIS_OK;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "analysis/room-modes"

double omx_room_median(const double *values, uint32_t n, double *scratch) {
  if (n == 0u) return NAN;
  memcpy(scratch, values, n * sizeof(double));
  sort_ascending(scratch, n);
  const uint32_t mid = n >> 1;
  return n % 2u == 0u ? (scratch[mid - 1u] + scratch[mid]) / 2.0 : scratch[mid];
}

/* The half-power (−3 dB re the peak) crossing on one side of `idx`, interpolated; NAN when the
 * curve runs off the band first. */
static double half_power_crossing(const double *db, const double *hz, uint32_t m, uint32_t idx, int dir) {
  const double target = db[idx] - 3.0;
  int64_t i = idx;
  for (;;) {
    const int64_t next = i + dir;
    if (next < 0 || next >= (int64_t)m) return NAN;
    if (db[next] <= target) {
      const double a = db[i], b = db[next];
      const double t = a == b ? 0.0 : (a - target) / (a - b);
      return hz[i] + t * (hz[next] - hz[i]);
    }
    i = next;
  }
}

void omx_room_mode_peaks(const OmxRoomAccum *acc, double floor_db, double min_above_median_db, double *scratch,
                         OmxRoomModePeak *peaks, uint32_t cap, OmxRoomModePeaks *out) {
  const uint32_t m = acc->k_hi - acc->k_lo + 1u;
  double *hz = scratch, *db = scratch + m, *sorted = scratch + 2u * m;
  for (uint32_t i = 0; i < m; i++) {
    const uint32_t k = acc->k_lo + i;
    hz[i] = ((double)k * acc->sample_rate) / (double)acc->fft_size;
    db[i] = lin_to_db_floored(hypot(acc->re[k], acc->im[k]), floor_db);
  }
  const double med = omx_room_median(db, m, sorted);
  const double bin_hz = acc->sample_rate / (double)acc->fft_size;
  uint32_t count = 0;
  for (uint32_t i = 1; i + 1u < m; i++) {
    if (!(db[i] > db[i - 1u] && db[i] > db[i + 1u])) continue;
    const double above = db[i] - med;
    if (above < min_above_median_db) continue;
    const double lo = half_power_crossing(db, hz, m, i, -1);
    const double hi = half_power_crossing(db, hz, m, i, 1);
    const double width = !isnan(lo) && !isnan(hi) ? hi - lo : 2.0 * bin_hz;
    const double narrowest = 2.0 * bin_hz;
    if (count >= cap) break;
    peaks[count].hz = hz[i];
    peaks[count].q = hz[i] / (width > narrowest ? width : narrowest);
    peaks[count].above_median_db = above;
    count++;
  }
  out->code = OMX_ANALYSIS_OK;
  out->sample_rate = acc->sample_rate;
  out->fft_size = acc->fft_size;
  out->bin_hz = bin_hz;
  out->peaks = peaks;
  out->n = count;
}

enum omx_analysis_refusal omx_room_modes(const OmxRoomModePeaks *positions, uint32_t n_positions,
                                         uint32_t min_positions, OmxRoomMode *out, uint32_t cap, uint32_t *n_out) {
  *n_out = 0;
  if (n_positions < min_positions) return OMX_ANALYSIS_TOO_FEW_POSITIONS;
  for (uint32_t p = 0; p < n_positions; p++)
    if (positions[p].code != OMX_ANALYSIS_OK) return (enum omx_analysis_refusal)positions[p].code;
  const OmxRoomModePeaks *first = &positions[0];
  for (uint32_t p = 0; p < n_positions; p++)
    if (positions[p].sample_rate != first->sample_rate || positions[p].fft_size != first->fft_size)
      return OMX_ANALYSIS_SHAPE_MISMATCH;
  const double tolerance = first->bin_hz;

  /* Every candidate of the first position that every other position matches within one bin (its
   * first match in ascending order) is a mode; the first position's peaks are distinct bins, so
   * none is claimed twice. */
  uint32_t count = 0;
  for (uint32_t c = 0; c < first->n; c++) {
    const OmxRoomModePeak *cand = &first->peaks[c];
    double sum_hz = cand->hz, sum_q = cand->q, sum_above = cand->above_median_db;
    double matched = 1.0;
    int everywhere = 1;
    for (uint32_t p = 1; p < n_positions && everywhere; p++) {
      const OmxRoomModePeak *match = NULL;
      for (uint32_t j = 0; j < positions[p].n; j++) {
        if (fabs(positions[p].peaks[j].hz - cand->hz) <= tolerance) {
          match = &positions[p].peaks[j];
          break;
        }
      }
      if (!match) {
        everywhere = 0;
        break;
      }
      sum_hz += match->hz;
      sum_q += match->q;
      sum_above += match->above_median_db;
      matched += 1.0;
    }
    if (!everywhere || count >= cap) continue;
    out[count].hz = sum_hz / matched;
    out[count].positions = (uint32_t)matched;
    out[count].q = sum_q / matched;
    out[count].above_median_db = sum_above / matched;
    count++;
  }
  /* Ascending frequency, stable. */
  for (uint32_t i = 1; i < count; i++) {
    const OmxRoomMode x = out[i];
    uint32_t j = i;
    while (j > 0u && out[j - 1u].hz > x.hz) {
      out[j] = out[j - 1u];
      j--;
    }
    out[j] = x;
  }
  *n_out = count;
  return OMX_ANALYSIS_OK;
}

int omx_room_mode_set_stable(const OmxRoomMode *before, uint32_t n_before, const OmxRoomMode *after, uint32_t n_after,
                             double tolerance_hz, unsigned char *used) {
  if (n_before != n_after) return 0;
  memset(used, 0, n_after);
  for (uint32_t b = 0; b < n_before; b++) {
    int found = 0;
    for (uint32_t a = 0; a < n_after; a++) {
      if (used[a]) continue;
      if (fabs(after[a].hz - before[b].hz) <= tolerance_hz) {
        used[a] = 1;
        found = 1;
        break;
      }
    }
    if (!found) return 0;
  }
  return 1;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "analysis/room-mode-eq"

void omx_room_mode_eq_proposals(const OmxRoomMode *modes, uint32_t n, uint32_t min_positions,
                                OmxRoomModeEqProposal *out, uint32_t cap, uint32_t *n_out) {
  /* The deepest cut: the notch depth law and the EQ's own gain travel, whichever is tighter. */
  const double max_cut = (double)OMX_FBS_MAX_NOTCH_DEPTH_DB < (double)OMX_EQ_GAIN_RANGE_MAX
                             ? (double)OMX_FBS_MAX_NOTCH_DEPTH_DB
                             : (double)OMX_EQ_GAIN_RANGE_MAX;
  const double q_min = OMX_EQ_Q_RANGE_MIN_DOUBLE, q_max = (double)OMX_EQ_Q_RANGE_MAX;
  uint32_t count = 0;
  for (uint32_t i = 0; i < n && count < cap; i++) {
    const OmxRoomMode *m = &modes[i];
    if (!(m->positions >= min_positions)) continue;
    if (!(m->above_median_db > 0.0)) continue;
    const double requested = -m->above_median_db;
    const double gain = requested > -max_cut ? requested : -max_cut;
    const double q_floored = m->q > q_min ? m->q : q_min;
    const double q = q_floored < q_max ? q_floored : q_max;
    out[count].hz = m->hz;
    out[count].gain_db = gain;
    out[count].q = q;
    out[count].positions = m->positions;
    out[count].bounded = gain != requested || q != m->q;
    out[count].requested_db = requested;
    count++;
  }
  /* Deepest first, stable: ties keep the modes' ascending frequency. */
  for (uint32_t i = 1; i < count; i++) {
    const OmxRoomModeEqProposal x = out[i];
    uint32_t j = i;
    while (j > 0u && out[j - 1u].gain_db > x.gain_db) {
      out[j] = out[j - 1u];
      j--;
    }
    out[j] = x;
  }
  *n_out = count;
}

int omx_room_mode_residual_db(const double *hz, const double *magnitude_db, uint32_t n, double target_hz,
                              double *scratch, double *residual_db) {
  if (n == 0u) return 0;
  const double med = omx_room_median(magnitude_db, n, scratch);
  uint32_t nearest = 0;
  double best = fabs(hz[0] - target_hz);
  for (uint32_t i = 0; i < n; i++) {
    const double diff = fabs(hz[i] - target_hz);
    if (diff < best) {
      best = diff;
      nearest = i;
    }
  }
  const double r = magnitude_db[nearest] - med;
  *residual_db = 0.0 > r ? 0.0 : r;
  return 1;
}

#undef OMX_CONTRACT_STAGE
