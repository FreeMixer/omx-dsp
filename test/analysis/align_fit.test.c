// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * align_fit.test.c — the cross-spectrum accumulator and the alignment fit
 * (<omxdsp/analysis/omx_align_fit.h>) against closed forms, at the nine RME rates on the RTA's own
 * frame size for each rate (analysis_rates.h), with core's declarations from the parity table.
 *
 * Every run pairs a reference of seeded noise with a target that is the reference through a known
 * transfer, so the answer follows from the transfer:
 *
 *   1. a pure delay of d samples (fractional) reads d, polarity normal, coherence 1, residual 0;
 *      a target identical to its reference reads a delay of exactly 0 and a residual of exactly 0,
 *      both for noise bins and for Hann-windowed noise frames narrowed to float as the RTA
 *      delivers them (FreeMixer/omx-dsp#117: the anchor read 5.8e-14 of a sample there);
 *   2. the same delay with the target inverted reads d and an inversion;
 *   3. a delay past half the window reads d − n on the near branch and d when the arrival is
 *      declared physical (the same peak, two names);
 *   4. a reference read b samples into its past with a ramp of d − b between the windows reads d;
 *   5. the coarse delay alone lands within half a sample of d and names the lag on both branches;
 *   6. a target of independent noise reads a coherence near 1/frames, far under the trust floor;
 *   7. two arrivals inside one correlation lobe (the second at 0.95, 150 µs after the first) have no
 *      single delay: the envelope's interpolated peak and the magnitude-weighted line through
 *      `arg H` disagree by more than a sample at every rate (1.4 at 32 kHz to 7.9 at 192 kHz), so at
 *      coherence 1 the fit refuses (residual over half a sample) rather than name either;
 *   8. the refusals: too few frames, a frame at another rate, a frame at another offset, a band that
 *      selects nothing, a silent pair;
 *   9. the correction is `applied − fit` inside the travel, flags a clamp, and flips polarity.
 *
 * The kernels run between room_count_begin() and room_count_end(): they allocate nothing.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_align_fit.h>
#include <omxdsp/omx_fft.h>

#include "analysis_rates.h"
#include "fixtures/room_ts.h"
#include "room_oracle.h"

/* One run's buffers, sized for the largest frame. */
typedef struct {
  uint32_t n, bins;
  double *xr, *xi, *pxx, *pyy;
  double *tr, *ti, *rr, *ri;
  double *sr, *si;
} Bufs;

static void bufs_alloc(Bufs *b, uint32_t n) {
  b->n = n;
  b->bins = n / 2u;
  const size_t bytes = b->bins * sizeof(double);
  b->xr = room_alloc(bytes); b->xi = room_alloc(bytes); b->pxx = room_alloc(bytes); b->pyy = room_alloc(bytes);
  b->tr = room_alloc(bytes); b->ti = room_alloc(bytes); b->rr = room_alloc(bytes); b->ri = room_alloc(bytes);
  b->sr = room_alloc(n * sizeof(double)); b->si = room_alloc(n * sizeof(double));
}

static void bufs_free(Bufs *b) {
  free(b->xr); free(b->xi); free(b->pxx); free(b->pyy);
  free(b->tr); free(b->ti); free(b->rr); free(b->ri);
  free(b->sr); free(b->si);
}

enum shape { PURE, PAIR, INDEPENDENT, SILENT };

/* Fold `frames` frames: reference noise, target = sign·e^{−iωd}·R (or the other shapes). */
static void run(Bufs *b, OmxXspec *a, double rate, uint32_t frames, double d, double sign, enum shape sh,
                double back, uint32_t seed) {
  RoomRng rng = {seed};
  const double n = (double)b->n;
  omx_xspec_init(a, b->xr, b->xi, b->pxx, b->pyy, b->n, rate, back);
  for (uint32_t f = 0; f < frames; f++) {
    for (uint32_t k = 0; k < b->bins; k++) {
      const double rr = room_noise(&rng), ri = room_noise(&rng);
      const double ph = -2.0 * M_PI * (double)k * d / n;
      double c = cos(ph), s = sin(ph);
      if (sh == PAIR) {
        const double ph2 = -2.0 * M_PI * (double)k * (d + 150e-6 * rate) / n;
        c += 0.95 * cos(ph2);
        s += 0.95 * sin(ph2);
      }
      b->rr[k] = sh == SILENT ? 0.0 : rr;
      b->ri[k] = sh == SILENT ? 0.0 : ri;
      if (sh == INDEPENDENT) {
        b->tr[k] = room_noise(&rng);
        b->ti[k] = room_noise(&rng);
      } else {
        b->tr[k] = sign * (c * b->rr[k] - s * b->ri[k]);
        b->ti[k] = sign * (s * b->rr[k] + c * b->ri[k]);
      }
    }
    omx_xspec_add(a, b->tr, b->ti, b->rr, b->ri, b->bins, b->n, rate, back);
  }
}

/* Fold `frames` identical pairs of real noise frames, each Hann-windowed (symmetric, denominator
 * n − 1), transformed, scaled by 4/n and narrowed to float: the spectra the RTA hands the fit. */
static void run_identical_frames(Bufs *b, OmxXspec *a, double rate, uint32_t frames, uint32_t seed) {
  RoomRng rng = {seed};
  const double n = (double)b->n;
  omx_xspec_init(a, b->xr, b->xi, b->pxx, b->pyy, b->n, rate, 0.0);
  for (uint32_t f = 0; f < frames; f++) {
    for (uint32_t i = 0; i < b->n; i++) {
      b->sr[i] = room_noise(&rng) * (0.5 * (1.0 - cos((2.0 * M_PI * (double)i) / (n - 1.0))));
      b->si[i] = 0.0;
    }
    omx_fft_radix2_d(b->sr, b->si, b->n, 0);
    for (uint32_t k = 0; k < b->bins; k++) {
      b->rr[k] = b->tr[k] = (double)(float)(b->sr[k] * (4.0 / n));
      b->ri[k] = b->ti[k] = (double)(float)(b->si[k] * (4.0 / n));
    }
    omx_xspec_add(a, b->tr, b->ti, b->rr, b->ri, b->bins, b->n, rate, 0.0);
  }
}

static enum omx_analysis_refusal fit(Bufs *b, const OmxXspec *a, enum omx_align_arrival arrival, double lo, double hi,
                                     OmxAlignFit *out) {
  memset(out, 0, sizeof *out);
  room_count_begin();
  const enum omx_analysis_refusal code = omx_align_fit(a, TS_MIN_FRAMES, lo, hi, arrival, TS_ALIGN_TRUST_COHERENCE, b->sr,
                                                       b->si, out);
  room_count_end();
  return code;
}

static double delay_samples(const OmxAlignFit *f, double rate) { return f->delay_ms * rate / 1000.0; }

static void arm_rate(double rate) {
  const uint32_t n = omx_analysis_fft_for_rate(rate);
  const double lo = TS_DEFAULT_BAND[0], hi = TS_DEFAULT_BAND[1];
  Bufs b;
  bufs_alloc(&b, n);
  OmxXspec a;
  OmxAlignFit f;

  g_arm = "pure delay";
  const double d = 37.4;
  run(&b, &a, rate, 6, d, 1.0, PURE, 0.0, 1u);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_OK, "a pure delay fits", 0, 0);
  check(fabs(delay_samples(&f, rate) - d) < 1e-6, "the delay reads d", delay_samples(&f, rate), d);
  check(f.invert == 0u, "polarity normal", f.invert, 0);
  check(fabs(f.coherence - 1.0) < 1e-12, "coherence 1", f.coherence, 1);
  check(f.residual_deg < 1e-6, "residual 0", f.residual_deg, 0);
  check(f.frames == 6u, "six frames", f.frames, 6);

  g_arm = "identical";
  {
    uint32_t klo, khi;
    omx_xspec_band(n, rate, lo, hi, &klo, &khi);
    for (int frames_kind = 0; frames_kind < 2; frames_kind++) {
      if (frames_kind == 0) run(&b, &a, rate, 8, 0.0, 1.0, PURE, 0.0, 11u);
      else run_identical_frames(&b, &a, rate, 8, 12u);
      int imag_zero = 1;
      for (uint32_t k = 0; k < b.bins; k++) imag_zero &= a.xi[k] == 0.0;
      check(imag_zero, "an identical pair's cross-spectrum is exactly real", imag_zero, 1);
      OmxCoarseDelay c;
      check(omx_align_coarse_delay(a.xr, a.xi, n, (int32_t)klo, (int32_t)khi, OMX_ALIGN_ARRIVAL_EITHER, b.sr, b.si, &c) == 1,
            "an identical pair has a peak", 0, 0);
      check(c.lag == 0.0 && c.samples == 0.0, "an identical pair's anchor is exactly lag 0", c.samples, 0.0);
      check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_OK, "an identical pair fits", 0, 0);
      check(f.delay_ms == 0.0, "an identical pair reads a delay of exactly 0", delay_samples(&f, rate), 0.0);
      check(f.residual_deg == 0.0, "an identical pair leaves a residual of exactly 0", f.residual_deg, 0.0);
      check(f.invert == 0u, "an identical pair is not inverted", f.invert, 0);
    }
  }

  g_arm = "inverted";
  run(&b, &a, rate, 5, -d, -1.0, PURE, 0.0, 2u);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_OK, "an inverted delay fits", 0, 0);
  check(fabs(delay_samples(&f, rate) + d) < 1e-6, "the delay reads −d", delay_samples(&f, rate), -d);
  check(f.invert == 1u, "polarity inverted", f.invert, 1);
  check(f.residual_deg < 1e-6, "residual 0 once inverted", f.residual_deg, 0);

  g_arm = "far";
  const double far = 0.7 * (double)n + 0.3;
  run(&b, &a, rate, 4, far, 1.0, PURE, 0.0, 3u);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_OK, "a far delay fits", 0, 0);
  check(fabs(delay_samples(&f, rate) - (far - (double)n)) < 1e-6, "the near branch names d − n", delay_samples(&f, rate),
        far - n);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_PHYSICAL, lo, hi, &f) == OMX_ANALYSIS_OK, "a physical far delay fits", 0, 0);
  check(fabs(delay_samples(&f, rate) - far) < 1e-6, "the physical branch names d", delay_samples(&f, rate), far);

  g_arm = "offset";
  const double back = 300.0, total = 312.75;
  run(&b, &a, rate, 4, total - back, 1.0, PURE, back, 4u);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_PHYSICAL, lo, hi, &f) == OMX_ANALYSIS_OK, "an offset reading fits", 0, 0);
  check(fabs(delay_samples(&f, rate) - total) < 1e-6, "the offset is inside the answer", delay_samples(&f, rate), total);

  g_arm = "coarse";
  {
    uint32_t klo, khi;
    check(omx_xspec_band(n, rate, lo, hi, &klo, &khi) == 1, "the band selects bins", 0, 0);
    const double bin_hz = rate / n;
    check(klo == (uint32_t)fmax(1.0, ceil(lo / bin_hz)) && khi == (uint32_t)floor(hi / bin_hz), "the band's bins", klo, khi);
    run(&b, &a, rate, 4, far, 1.0, PURE, 0.0, 5u);
    OmxCoarseDelay c;
    room_count_begin();
    const int got = omx_align_coarse_delay(a.xr, a.xi, n, (int32_t)klo, (int32_t)khi, OMX_ALIGN_ARRIVAL_EITHER, b.sr, b.si, &c);
    room_count_end();
    check(got == 1, "a peak is found", got, 1);
    check(fabs(c.lag - far) < 0.5, "the lag within half a sample of d", c.lag, far);
    check(c.samples == c.lag - (double)n, "the near branch is lag − n", c.samples, c.lag - n);
    omx_align_coarse_delay(a.xr, a.xi, n, (int32_t)klo, (int32_t)khi, OMX_ALIGN_ARRIVAL_PHYSICAL, b.sr, b.si, &c);
    check(c.samples == c.lag, "the physical branch is the lag", c.samples, c.lag);
    check(omx_align_coarse_delay(a.xr, a.xi, n, 40, 30, OMX_ALIGN_ARRIVAL_EITHER, b.sr, b.si, &c) == 0,
          "an empty band has no peak", 0, 0);
    check(omx_align_coarse_delay(a.xr, a.xi, n - 1u, 2, 30, OMX_ALIGN_ARRIVAL_EITHER, b.sr, b.si, &c) == 0,
          "a length not a power of two has no peak", 0, 0);
  }

  g_arm = "independent";
  run(&b, &a, rate, 64, 0.0, 1.0, INDEPENDENT, 0.0, 6u);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_OK, "independent noise still answers", 0, 0);
  check(f.coherence < 4.0 / 64.0, "coherence near 1/frames", f.coherence, 1.0 / 64.0);

  g_arm = "pair";
  run(&b, &a, rate, 4, 0.002 * rate, 1.0, PAIR, 0.0, 7u);
  {
    const enum omx_analysis_refusal code = fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f);
    check(code == OMX_ANALYSIS_RESIDUAL_EXCEEDS_HALF_SAMPLE, "a pair with no single delay at coherence 1 refuses", code,
          OMX_ANALYSIS_RESIDUAL_EXCEEDS_HALF_SAMPLE);
  }

  g_arm = "refusals";
  run(&b, &a, rate, 3, d, 1.0, PURE, 0.0, 8u);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_TOO_FEW_FRAMES, "three frames refuse", 0, 0);
  check(f.frames == 3u, "and say how many", f.frames, 3);
  run(&b, &a, rate, 4, d, 1.0, PURE, 0.0, 9u);
  check(omx_xspec_add(&a, b.tr, b.ti, b.rr, b.ri, b.bins, n, rate * 2.0, 0.0) == 0, "a frame at another rate", 0, 0);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_SHAPE_MISMATCH, "refuses shape-mismatch", 0, 0);
  run(&b, &a, rate, 4, d, 1.0, PURE, 0.0, 10u);
  check(omx_xspec_add(&a, b.tr, b.ti, b.rr, b.ri, b.bins - 1u, n, rate, 0.0) == 0, "a frame one bin short", 0, 0);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_SHAPE_MISMATCH, "refuses shape-mismatch", 0, 0);
  run(&b, &a, rate, 4, d, 1.0, PURE, 0.0, 11u);
  check(omx_xspec_add(&a, b.tr, b.ti, b.rr, b.ri, b.bins, n, rate, 64.0) == 0, "a frame at another offset", 0, 0);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_SHAPE_MISMATCH, "refuses shape-mismatch", 0, 0);
  run(&b, &a, rate, 4, d, 1.0, PURE, 0.0, 12u);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, 4000.0, 60.0, &f) == OMX_ANALYSIS_EMPTY_BAND, "an inverted band refuses", 0, 0);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, rate, 2.0 * rate, &f) == OMX_ANALYSIS_EMPTY_BAND,
        "a band above Nyquist refuses", 0, 0);
  run(&b, &a, rate, 4, d, 1.0, SILENT, 0.0, 13u);
  check(fit(&b, &a, OMX_ALIGN_ARRIVAL_EITHER, lo, hi, &f) == OMX_ANALYSIS_NO_SHARED_ENERGY, "a silent pair refuses", 0, 0);

  bufs_free(&b);
}

static void arm_correction(void) {
  g_arm = "correction";
  g_rate = 0.0;
  OmxAlignCorrection c;
  room_count_begin();
  omx_align_correction(2.0, 0u, 5.0, 0u, 0.0, 50.0, &c);
  room_count_end();
  check(c.delay_ms == 3.0 && c.inverted == 0u && c.clamped == 0u, "5 ms held, 2 ms late: 3 ms", c.delay_ms, 3.0);
  omx_align_correction(7.0, 1u, 5.0, 0u, 0.0, 50.0, &c);
  check(c.delay_ms == 0.0 && c.clamped == 1u, "a late target past the floor clamps at 0", c.delay_ms, 0.0);
  check(c.inverted == 1u, "an inversion flips a normal channel", c.inverted, 1);
  omx_align_correction(-60.0, 1u, 0.0, 1u, 0.0, 50.0, &c);
  check(c.delay_ms == 50.0 && c.clamped == 1u, "an early target past the ceiling clamps at the travel", c.delay_ms, 50.0);
  check(c.inverted == 0u, "an inversion flips an inverted channel back", c.inverted, 0);
}

int main(void) {
  omx_analysis_require_rates();
  for (int r = 0; r < OMX_ANALYSIS_RATE_COUNT; r++) {
    g_rate = OMX_ANALYSIS_RATES[r];
    arm_rate(g_rate);
  }
  arm_correction();
  return room_report("align_fit");
}
