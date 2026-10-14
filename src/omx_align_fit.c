// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_align_fit.c — the cross-spectrum accumulator and Smart Alignment's fit
 * (<omxdsp/analysis/omx_align_fit.h>), moved from OpenMixer core's TypeScript (smart-align.ts).
 *
 * Every reading is in double and in the order core's TypeScript computed it, so the parity table
 * (test/analysis/room_ts.test.c) holds this file to core's answers within the stated tolerances.
 */
#include <omxdsp/analysis/omx_align_fit.h>

#include <math.h>
#include <string.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_fft.h>

#define OMX_CONTRACT_STAGE "analysis/xspec"

void omx_xspec_init(OmxXspec *a, double *xr, double *xi, double *pxx, double *pyy, uint32_t fft_size,
                    double sample_rate, double back_samples) {
  OMX_PRE(fft_size >= 2u, "fft-size-ge-2");
  OMX_PRE(sample_rate > 0.0, "rate-positive");
  OMX_PRE(xr && xi && pxx && pyy, "buffers-present");
  a->xr = xr;
  a->xi = xi;
  a->pxx = pxx;
  a->pyy = pyy;
  a->bins = fft_size >> 1;
  a->fft_size = fft_size;
  a->sample_rate = sample_rate;
  a->back_samples = back_samples;
  a->frames = 0;
  a->mismatch = 0;
  a->back_mismatch = 0;
  memset(xr, 0, a->bins * sizeof(double));
  memset(xi, 0, a->bins * sizeof(double));
  memset(pxx, 0, a->bins * sizeof(double));
  memset(pyy, 0, a->bins * sizeof(double));
}

int omx_xspec_add(OmxXspec *a, const double *t_re, const double *t_im, const double *r_re, const double *r_im,
                  uint32_t nbins, uint32_t fft_size, double sample_rate, double back_samples) {
  a->frames++;
  if (a->mismatch) return 0;
  if (fft_size != a->fft_size || sample_rate != a->sample_rate || nbins < a->bins) {
    a->mismatch = 1;
    return 0;
  }
  const int back_ok = back_samples == a->back_samples;
  if (!back_ok) a->back_mismatch = 1;
  for (uint32_t k = 0; k < a->bins; k++) {
    const double tr = t_re[k], ti = t_im[k], rr = r_re[k], ri = r_im[k];
    /* H = target * conj(reference): its angle is the phase of target RELATIVE to reference. */
    a->xr[k] += tr * rr + ti * ri;
    a->xi[k] += ti * rr - tr * ri;
    a->pxx[k] += tr * tr + ti * ti;
    a->pyy[k] += rr * rr + ri * ri;
  }
  return back_ok;
}

int omx_xspec_band(uint32_t fft_size, double sample_rate, double low_hz, double high_hz, uint32_t *k_lo,
                   uint32_t *k_hi) {
  const double bin_hz = sample_rate / (double)fft_size;
  const double bins = (double)(fft_size >> 1);
  double lo = ceil(low_hz / bin_hz);
  double hi = floor(high_hz / bin_hz);
  if (lo < 1.0) lo = 1.0;
  if (hi > bins - 2.0) hi = bins - 2.0;
  if (!(hi > lo)) {
    *k_lo = 0;
    *k_hi = 0;
    return 0;
  }
  *k_lo = (uint32_t)lo;
  *k_hi = (uint32_t)hi;
  return 1;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "analysis/align-fit"

int omx_align_coarse_delay(const double *cross_re, const double *cross_im, uint32_t fft_size, int32_t k_lo,
                           int32_t k_hi, enum omx_align_arrival arrival, double *scratch_re, double *scratch_im,
                           OmxCoarseDelay *out) {
  const uint32_t n = fft_size;
  if (!omx_fft_length_ok(n)) return 0;
  const int32_t lo = k_lo > 0 ? k_lo : 0;
  const int32_t hi = k_hi < (int32_t)(n >> 1) - 1 ? k_hi : (int32_t)(n >> 1) - 1;
  if (hi < lo) return 0;

  double *re = scratch_re, *im = scratch_im;
  memset(re, 0, n * sizeof(double));
  memset(im, 0, n * sizeof(double));
  double energy = 0.0;
  for (int32_t k = lo; k <= hi; k++) {
    const double xr = cross_re[k], xi = cross_im[k];
    re[k] = xr;
    im[k] = xi;
    energy += xr * xr + xi * xi;
  }
  if (!(energy > 0.0)) return 0;
  omx_fft_radix2_d(re, im, n, 1);

  double best = -1.0;
  uint32_t best_lag = 0;
  for (uint32_t l = 0; l < n; l++) {
    const double m = re[l] * re[l] + im[l] * im[l];
    if (m > best) {
      best = m;
      best_lag = l;
    }
  }
  const uint32_t il = (best_lag + n - 1u) % n, ih = (best_lag + 1u) % n;
  const double y_lo = hypot(re[il], im[il]);
  const double y0 = hypot(re[best_lag], im[best_lag]);
  const double y_hi = hypot(re[ih], im[ih]);
  const double curve = y_lo - 2.0 * y0 + y_hi;
  /* A peak has NEGATIVE curvature; anything else is a flat or degenerate top and the sample index
   * is already the best statement available. */
  double frac = curve < 0.0 ? (0.5 * (y_lo - y_hi)) / curve : 0.0;
  if (!isfinite(frac) || fabs(frac) > 0.5) frac = 0.0;

  const double lag = (double)best_lag + frac;
  out->lag = lag;
  out->samples = arrival == OMX_ALIGN_ARRIVAL_PHYSICAL ? lag : (lag < (double)n / 2.0 ? lag : lag - (double)n);
  out->peak = y0;
  return 1;
}

enum omx_analysis_refusal omx_align_fit(const OmxXspec *a, uint32_t min_frames, double low_hz, double high_hz,
                                        enum omx_align_arrival arrival, double trust_coherence,
                                        double *scratch_re, double *scratch_im, OmxAlignFit *out) {
  out->frames = a->frames;
  if (a->frames < min_frames) return OMX_ANALYSIS_TOO_FEW_FRAMES;
  if (a->mismatch || a->back_mismatch) return OMX_ANALYSIS_SHAPE_MISMATCH;

  const double *xr = a->xr, *xi = a->xi;
  const double n = (double)a->fft_size;
  uint32_t k_lo, k_hi;
  if (!omx_xspec_band(a->fft_size, a->sample_rate, low_hz, high_hz, &k_lo, &k_hi)) return OMX_ANALYSIS_EMPTY_BAND;

  /* Coherence, energy-weighted across the band: one number for "is any of this shared". */
  double coh_num = 0.0, coh_den = 0.0;
  for (uint32_t k = k_lo; k <= k_hi; k++) {
    const double cross2 = xr[k] * xr[k] + xi[k] * xi[k];
    const double den = a->pxx[k] * a->pyy[k];
    if (den <= 0.0) continue;
    const double w = sqrt(den);
    coh_num += (cross2 / den) * w;
    coh_den += w;
  }
  if (coh_den <= 0.0) return OMX_ANALYSIS_NO_SHARED_ENERGY;
  double coherence = coh_num / coh_den;
  coherence = coherence < 0.0 ? 0.0 : coherence;
  coherence = coherence > 1.0 ? 1.0 : coherence;

  /* THE ANCHOR, over the whole window. With an offset in force the naming reverts to the near
   * branch: the arrival is `measured + back`, and a negative measured residual is expected. */
  const double back = a->back_samples;
  OmxCoarseDelay coarse;
  if (!omx_align_coarse_delay(xr, xi, a->fft_size, (int32_t)k_lo, (int32_t)k_hi,
                              back > 0.0 ? OMX_ALIGN_ARRIVAL_EITHER : arrival, scratch_re, scratch_im, &coarse))
    return OMX_ANALYSIS_NO_SHARED_ENERGY;
  const double anchor = coarse.samples;

  /* THE REFINEMENT: de-ramp by the anchor, rotate the band onto its own mean phasor, and fit a
   * magnitude-weighted line through what is left about the weighted mean bin. */
  double mr = 0.0, mi = 0.0, w_sum = 0.0, kw_sum = 0.0;
  for (uint32_t k = k_lo; k <= k_hi; k++) {
    const double ang = (2.0 * M_PI * (double)k * anchor) / n;
    const double c = cos(ang), s = sin(ang);
    mr += xr[k] * c - xi[k] * s;
    mi += xr[k] * s + xi[k] * c;
    const double w = hypot(xr[k], xi[k]);
    w_sum += w;
    kw_sum += w * (double)k;
  }
  const double m_mag = hypot(mr, mi);
  if (!(w_sum > 0.0) || !(m_mag > 0.0)) return OMX_ANALYSIS_NO_SHARED_ENERGY;
  const double mc = mr / m_mag, ms = -mi / m_mag;
  const double k_bar = kw_sum / w_sum;
  double sxy = 0.0, sxx = 0.0;
  for (uint32_t k = k_lo; k <= k_hi; k++) {
    const double ang = (2.0 * M_PI * (double)k * anchor) / n;
    const double c = cos(ang), s = sin(ang);
    const double dr = xr[k] * c - xi[k] * s;
    const double di = xr[k] * s + xi[k] * c;
    const double w = hypot(dr, di);
    if (w <= 0.0) continue;
    const double gr = dr * mc - di * ms;
    const double gi = dr * ms + di * mc;
    const double dk = (double)k - k_bar;
    sxy += w * dk * atan2(gi, gr);
    sxx += w * dk * dk;
  }
  if (!(sxx > 0.0)) return OMX_ANALYSIS_NO_SHARED_ENERGY;
  /* phi(k) = −2πkD/n, so D = −step·n/(2π), in samples. */
  const double step_rad = sxy / sxx;
  const double residual_samples = (-step_rad * n) / (2.0 * M_PI);
  /* A residual over half a sample at a trusted coherence says the anchor found the wrong lobe. */
  if (fabs(residual_samples) > 0.5 && coherence >= trust_coherence) return OMX_ANALYSIS_RESIDUAL_EXCEEDS_HALF_SAMPLE;
  const double measured = anchor + residual_samples;
  const double delay_samples = measured + back;

  /* Polarity: undo the delay, then ask which way the remaining phase points. */
  double pr = 0.0;
  for (uint32_t k = k_lo; k <= k_hi; k++) {
    const double ang = (2.0 * M_PI * (double)k * measured) / n;
    pr += xr[k] * cos(ang) - xi[k] * sin(ang);
  }
  const uint32_t invert = pr < 0.0 ? 1u : 0u;

  /* Residual: how far the de-delayed, de-polarised phase still sits from zero, weighted RMS. */
  const double flip = invert ? -1.0 : 1.0;
  double sum_w = 0.0, sum_sq = 0.0;
  for (uint32_t k = k_lo; k <= k_hi; k++) {
    const double ang = (2.0 * M_PI * (double)k * measured) / n;
    const double c = cos(ang), s = sin(ang);
    const double rr = (xr[k] * c - xi[k] * s) * flip;
    const double ri = (xr[k] * s + xi[k] * c) * flip;
    const double w = hypot(xr[k], xi[k]);
    if (w <= 0.0) continue;
    const double resid = atan2(ri, rr);
    sum_sq += resid * resid * w;
    sum_w += w;
  }

  out->delay_ms = (delay_samples / a->sample_rate) * 1000.0;
  out->invert = invert;
  out->coherence = coherence;
  out->residual_deg = sum_w > 0.0 ? (sqrt(sum_sq / sum_w) * 180.0) / M_PI : 0.0;
  return OMX_ANALYSIS_OK;
}

void omx_align_correction(double fit_delay_ms, uint32_t fit_invert, double applied_delay_ms, uint32_t applied_inverted,
                          double travel_min, double travel_max, OmxAlignCorrection *out) {
  const double wanted = applied_delay_ms - fit_delay_ms;
  const double floored = travel_min > wanted ? travel_min : wanted;
  out->delay_ms = travel_max < floored ? travel_max : floored;
  out->inverted = fit_invert ? !applied_inverted : (applied_inverted ? 1u : 0u);
  out->clamped = wanted < travel_min || wanted > travel_max;
}

#undef OMX_CONTRACT_STAGE
