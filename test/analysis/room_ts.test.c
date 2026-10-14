// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * room_ts.test.c — the room analysis, the Farina sweep and the alignment fit against core's
 * TypeScript: the parity table fixtures/room_ts.h, which tools/room-ts-golden.mjs wrote by
 * running core's room-*.ts and smart-align.ts on fixed
 * inputs. This program rebuilds the same inputs bit for bit (an integer PRNG and exact sums,
 * products and quotients of the table's constants) and runs the C kernels on them.
 *
 * The bounds:
 *   - every integer result is exact: refusal codes, frame counts, band bins, the direct arrival,
 *     how many reflections, peaks, modes and proposals, polarity, the bounded flag, lengths;
 *   - every real result is within 1e-9 of core's, relative to the value, or, for a value under its
 *     vector's own scale (a sweep sample near a zero crossing, a phase near 0°), to that scale.
 * Both sides compute one formula in double in one order; they differ only where V8's cos, sin,
 * exp, log10, atan2, hypot and pow round differently from the libm's, by an ulp or so, which the
 * transforms and the least-squares slope carry to some 1e-13 relative. The bound leaves the libm of
 * another platform room and still catches a change of law (test/analysis/room-sabotage.sh).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/analysis/omx_align_fit.h>
#include <omxdsp/analysis/omx_room.h>
#include <omxdsp/analysis/omx_sweep.h>

#include "fixtures/room_ts.h"

#define ROOM_TS_TOL 1e-9

static int g_fail = 0;
static int g_checks = 0;
static double g_worst = 0.0;
static const char *g_arm = "";

static void check(int cond, const char *what, double got, double want) {
  g_checks++;
  if (!cond) {
    g_fail++;
    if (g_fail <= 40) fprintf(stderr, "FAIL [%s] %s: C %.17g, TS %.17g\n", g_arm, what, got, want);
  }
}

static void exact(double got, double want, const char *what) { check(got == want, what, got, want); }

/* |c − ts| ≤ 1e-9 · max(|ts|, scale). */
static void near(double got, double want, double scale, const char *what) {
  if (isinf(want) || isnan(want)) {
    check(got == want || (isnan(got) && isnan(want)), what, got, want);
    return;
  }
  const double ref = fabs(want) > scale ? fabs(want) : scale;
  const double gap = fabs(got - want) / ref;
  if (gap > g_worst) g_worst = gap;
  check(gap <= ROOM_TS_TOL, what, got, want);
}

static void *xmalloc(size_t n) {
  void *p = calloc(1, n ? n : 1);
  if (!p) {
    fprintf(stderr, "room_ts: out of memory\n");
    exit(2);
  }
  return p;
}

/* ---- the inputs, as tools/room-ts-golden.mjs builds them --------------------------------------- */

typedef struct {
  uint32_t a;
} Rng;

static double draw(Rng *r) {
  r->a += 0x6d2b79f5u;
  uint32_t t = (r->a ^ (r->a >> 15)) * (1u | r->a);
  t = (t + (t ^ (t >> 7)) * (61u | t)) ^ t;
  return (double)(t ^ (t >> 14)) / 4294967296.0;
}

/* Four draws less two, summed left to right as the generator sums them. */
static double noise(Rng *r) {
  double s = draw(r);
  s += draw(r);
  s += draw(r);
  s += draw(r);
  return s - 2.0;
}

static void transfer_of(const struct ts_frame_set *fs, double *hr, double *hi) {
  const uint32_t bins = fs->fft >> 1;
  double z1r = 1, z1i = 0, z2r = 1, z2i = 0;
  for (uint32_t k = 0; k < bins; k++) {
    double r = fs->g * z1r + fs->a * z2r;
    double i = fs->g * z1i + fs->a * z2i;
    for (uint32_t m = 0; m < fs->nmodes; m++) {
      const double x = ((double)k - fs->modes[m].k0) / fs->modes[m].w;
      const double den = 1 + x * x;
      r += fs->modes[m].amp / den;
      i += (-fs->modes[m].amp * x) / den;
    }
    hr[k] = r;
    hi[k] = i;
    const double n1r = z1r * fs->r1[0] - z1i * fs->r1[1], n1i = z1r * fs->r1[1] + z1i * fs->r1[0];
    const double n2r = z2r * fs->r2[0] - z2i * fs->r2[1], n2i = z2r * fs->r2[1] + z2i * fs->r2[0];
    z1r = n1r; z1i = n1i; z2r = n2r; z2i = n2i;
  }
}

/* A frame set's run, folded into an accumulator whose arrays this allocates (a test may). */
static void xspec_of(uint32_t f, OmxXspec *a) {
  const struct ts_frame_set *fs = &TS_FRAME_SETS[f];
  const uint32_t bins = fs->fft >> 1;
  double *hr = xmalloc(bins * sizeof(double)), *hi = xmalloc(bins * sizeof(double));
  double *tr = xmalloc(bins * sizeof(double)), *ti = xmalloc(bins * sizeof(double));
  double *rr = xmalloc(bins * sizeof(double)), *ri = xmalloc(bins * sizeof(double));
  transfer_of(fs, hr, hi);
  omx_xspec_init(a, xmalloc(bins * sizeof(double)), xmalloc(bins * sizeof(double)), xmalloc(bins * sizeof(double)),
                 xmalloc(bins * sizeof(double)), fs->fft, fs->sr, fs->back);
  Rng rng = {fs->seed};
  for (uint32_t fr = 0; fr < fs->frames; fr++) {
    for (uint32_t k = 0; k < bins; k++) {
      const double sh = 1 / (1 + fs->tilt * (double)k);
      const double xr = noise(&rng) * sh;
      const double xi = noise(&rng) * sh;
      const double n1 = noise(&rng);
      const double n2 = noise(&rng);
      rr[k] = xr;
      ri[k] = xi;
      tr[k] = hr[k] * xr - hi[k] * xi + fs->nz * n1;
      ti[k] = hr[k] * xi + hi[k] * xr + fs->nz * n2;
    }
    const int last = fr == fs->frames - 1u && fs->mismatch;
    omx_xspec_add(a, tr, ti, rr, ri, bins, fs->fft, last ? fs->sr * 2 : fs->sr, fs->back);
  }
  free(hr); free(hi); free(tr); free(ti); free(rr); free(ri);
}

static void xspec_free(OmxXspec *a) {
  free(a->xr); free(a->xi); free(a->pxx); free(a->pyy);
}

static void accum_alloc(OmxRoomAccum *acc, uint32_t bins) {
  acc->re = xmalloc(bins * sizeof(double));
  acc->im = xmalloc(bins * sizeof(double));
  acc->coherence = xmalloc(bins * sizeof(double));
}

static void accum_free(OmxRoomAccum *acc) {
  free(acc->re); free(acc->im); free(acc->coherence);
}

/* The curve against TS_CURVE rows [off, off + n). */
static void check_curve(const OmxRoomAccum *acc, uint32_t off, uint32_t n) {
  const uint32_t m = acc->k_hi - acc->k_lo + 1u;
  exact(m, n, "the curve's bin count");
  if (m != n) return;
  double *hz = xmalloc(m * sizeof(double)), *db = xmalloc(m * sizeof(double));
  double *ph = xmalloc(m * sizeof(double)), *co = xmalloc(m * sizeof(double));
  omx_room_curve(acc, TS_ANALYSIS_FLOOR_DB, hz, db, ph, co);
  for (uint32_t i = 0; i < m; i++) {
    const double *t = &TS_CURVE[4u * (off + i)];
    near(hz[i], t[0], 1.0, "a bin's frequency");
    near(db[i], t[1], 1.0, "a bin's magnitude, dB");
    near(ph[i], t[2], 1.0, "a bin's unwrapped phase, degrees");
    near(co[i], t[3], 1.0, "a bin's coherence");
  }
  free(hz); free(db); free(ph); free(co);
}

static void check_reflections(const OmxRoomReflection *r, uint32_t n, uint32_t off, uint32_t want_n) {
  exact(n, want_n, "how many reflections");
  for (uint32_t i = 0; i < n && i < want_n; i++) {
    const double *t = &TS_REFL[3u * (off + i)];
    near(r[i].arrival_ms, t[0], 1.0, "a reflection's arrival, ms");
    near(r[i].distance_m, t[1], 1.0, "a reflection's extra path, m");
    near(r[i].level_db, t[2], 1.0, "a reflection's level, dB");
  }
}

/* ---- the arms ---------------------------------------------------------------------------------- */

static void arm_fit(void) {
  g_arm = "fit";
  for (size_t i = 0; i < sizeof TS_FITS / sizeof TS_FITS[0]; i++) {
    const struct ts_fit *t = &TS_FITS[i];
    OmxXspec a;
    xspec_of(t->fs, &a);
    double *sr = xmalloc(a.fft_size * sizeof(double)), *si = xmalloc(a.fft_size * sizeof(double));
    OmxAlignFit fit;
    memset(&fit, 0, sizeof fit);
    const enum omx_analysis_refusal code =
        omx_align_fit(&a, TS_MIN_FRAMES, TS_DEFAULT_BAND[0], TS_DEFAULT_BAND[1],
                      t->physical ? OMX_ALIGN_ARRIVAL_PHYSICAL : OMX_ALIGN_ARRIVAL_EITHER, TS_ALIGN_TRUST_COHERENCE, sr, si,
                      &fit);
    exact(code, t->code, "the fit's refusal code");
    exact(fit.frames, t->frames, "the fit's frame count");
    if (code == OMX_ANALYSIS_OK && t->code == 0) {
      near(fit.delay_ms, t->delay_ms, 1.0, "the fit's delay, ms");
      exact(fit.invert, t->invert, "the fit's polarity");
      near(fit.coherence, t->coherence, 1.0, "the fit's coherence");
      near(fit.residual_deg, t->residual_deg, 1.0, "the fit's residual, degrees");
    }
    free(sr); free(si);
    xspec_free(&a);
  }
}

static void arm_coarse(void) {
  g_arm = "coarse";
  for (size_t i = 0; i < sizeof TS_COARSE / sizeof TS_COARSE[0]; i++) {
    const struct ts_coarse *t = &TS_COARSE[i];
    const struct ts_frame_set *fs = &TS_FRAME_SETS[t->fs];
    const uint32_t bins = fs->fft >> 1;
    double *hr = xmalloc(bins * sizeof(double)), *hi = xmalloc(bins * sizeof(double));
    double *sr = xmalloc(fs->fft * sizeof(double)), *si = xmalloc(fs->fft * sizeof(double));
    transfer_of(fs, hr, hi);
    OmxCoarseDelay c = {0, 0, 0};
    const int found = omx_align_coarse_delay(hr, hi, fs->fft, t->k_lo, t->k_hi,
                                             t->physical ? OMX_ALIGN_ARRIVAL_PHYSICAL : OMX_ALIGN_ARRIVAL_EITHER, sr, si, &c);
    exact(found, t->found, "a peak found");
    if (found && t->found) {
      near(c.samples, t->samples, 1.0, "the coarse delay, samples");
      near(c.lag, t->lag, 1.0, "the circular lag");
      near(c.peak, t->peak, 1.0, "the envelope's peak");
    }
    free(hr); free(hi); free(sr); free(si);
  }
}

static void arm_correction(void) {
  g_arm = "correction";
  for (size_t i = 0; i < sizeof TS_CORRECTIONS / sizeof TS_CORRECTIONS[0]; i++) {
    const struct ts_correction *t = &TS_CORRECTIONS[i];
    OmxAlignCorrection c;
    omx_align_correction(t->fit_ms, t->fit_invert, t->applied_ms, t->applied_inverted, t->lo, t->hi, &c);
    near(c.delay_ms, t->delay_ms, 1.0, "the corrected delay, ms");
    exact(c.inverted, t->inverted, "the corrected polarity");
    exact(c.clamped, t->clamped, "clamped");
  }
}

static void arm_transfer(void) {
  g_arm = "transfer";
  for (size_t i = 0; i < sizeof TS_TRANSFERS / sizeof TS_TRANSFERS[0]; i++) {
    const struct ts_transfer *t = &TS_TRANSFERS[i];
    OmxXspec a;
    xspec_of(t->fs, &a);
    OmxRoomAccum acc;
    accum_alloc(&acc, a.bins);
    const enum omx_analysis_refusal code = omx_room_transfer(&a, TS_MIN_FRAMES, t->band[0], t->band[1], &acc);
    exact(code, t->code, "the transfer's refusal code");
    exact(acc.frames, t->frames, "the transfer's frame count");
    if (code == OMX_ANALYSIS_OK && t->code == 0) {
      exact(acc.k_lo, t->k_lo, "the band's first bin");
      exact(acc.k_hi, t->k_hi, "the band's last bin");
      check_curve(&acc, t->off, t->n);
    }
    accum_free(&acc);
    xspec_free(&a);
  }
}

static void arm_impulse(void) {
  g_arm = "impulse";
  for (size_t i = 0; i < sizeof TS_IMPULSES / sizeof TS_IMPULSES[0]; i++) {
    const struct ts_impulse *t = &TS_IMPULSES[i];
    OmxXspec a;
    xspec_of(t->fs, &a);
    OmxRoomAccum acc;
    accum_alloc(&acc, a.bins);
    enum omx_analysis_refusal code = omx_room_transfer(&a, TS_MIN_FRAMES, t->band[0], t->band[1], &acc);
    uint32_t direct = 0, n = 0;
    OmxRoomReflection *r = NULL;
    if (code == OMX_ANALYSIS_OK) {
      const uint32_t cap = omx_room_impulse_capacity(&acc, TS_EARLY_REFLECTION_WINDOW_MS);
      double *sr = xmalloc(a.fft_size * sizeof(double)), *si = xmalloc(a.fft_size * sizeof(double));
      r = xmalloc(cap * sizeof *r);
      code = omx_room_impulse(&acc, TS_EARLY_REFLECTION_WINDOW_MS, TS_EARLY_REFLECTION_FLOOR_DB, sr, si, &direct, r, cap,
                              &n);
      free(sr); free(si);
    }
    exact(code, t->code, "the impulse's refusal code");
    if (code == OMX_ANALYSIS_OK && t->code == 0) {
      exact(direct, t->direct, "the direct arrival, samples");
      check_reflections(r, n, t->off, t->n);
    }
    free(r);
    accum_free(&acc);
    xspec_free(&a);
  }
}

static void arm_ir(void) {
  g_arm = "ir";
  for (size_t i = 0; i < sizeof TS_IRS / sizeof TS_IRS[0]; i++) {
    const struct ts_ir *t = &TS_IRS[i];
    double *ir = xmalloc(t->n * sizeof(double));
    for (uint32_t j = 0; j < t->ntaps; j++) ir[t->at[j]] = t->level[j];
    const uint32_t size = omx_room_ir_fft_size(t->n);
    double *sr = xmalloc(size * sizeof(double)), *si = xmalloc(size * sizeof(double));
    OmxRoomAccum acc;
    accum_alloc(&acc, size / 2u);
    const enum omx_analysis_refusal code = omx_room_transfer_from_ir(ir, t->n, t->sr, t->band[0], t->band[1], sr, si, &acc);
    exact(code, t->code, "the IR transfer's refusal code");
    exact(acc.frames, t->frames, "the IR transfer's frame count");
    if (code == OMX_ANALYSIS_OK && t->code == 0) {
      exact(acc.fft_size, t->fft_size, "the IR's transform length");
      exact(acc.k_lo, t->k_lo, "the band's first bin");
      exact(acc.k_hi, t->k_hi, "the band's last bin");
      check_curve(&acc, t->off, t->n_bins);
      const uint32_t cap = omx_room_impulse_capacity(&acc, TS_EARLY_REFLECTION_WINDOW_MS);
      OmxRoomReflection *r = xmalloc(cap * sizeof *r);
      uint32_t direct = 0, n = 0;
      const enum omx_analysis_refusal ic =
          omx_room_impulse(&acc, TS_EARLY_REFLECTION_WINDOW_MS, TS_EARLY_REFLECTION_FLOOR_DB, sr, si, &direct, r, cap, &n);
      exact(ic, t->impulse_code, "the IR impulse's refusal code");
      if (ic == OMX_ANALYSIS_OK) {
        exact(direct, t->direct, "the IR's direct arrival, samples");
        check_reflections(r, n, t->roff, t->rn);
      }
      free(r);
    }
    accum_free(&acc);
    free(ir); free(sr); free(si);
  }
}

/* One position's peaks in the mode band; `peaks` is the caller's to free. */
static void peaks_of(uint32_t f, OmxRoomModePeaks *out, OmxRoomModePeak **peaks) {
  OmxXspec a;
  xspec_of(f, &a);
  OmxRoomAccum acc;
  accum_alloc(&acc, a.bins);
  memset(out, 0, sizeof *out);
  *peaks = NULL;
  const enum omx_analysis_refusal code = omx_room_transfer(&a, TS_MIN_FRAMES, TS_ROOM_MODE_BAND[0], TS_ROOM_MODE_BAND[1], &acc);
  if (code != OMX_ANALYSIS_OK) {
    out->code = code;
  } else {
    const uint32_t m = acc.k_hi - acc.k_lo + 1u;
    double *scratch = xmalloc(3u * m * sizeof(double));
    *peaks = xmalloc((m / 2u + 1u) * sizeof **peaks);
    omx_room_mode_peaks(&acc, TS_ANALYSIS_FLOOR_DB, TS_ROOM_MODE_MIN_ABOVE_MEDIAN_DB, scratch, *peaks, m / 2u + 1u, out);
    free(scratch);
  }
  accum_free(&acc);
  xspec_free(&a);
}

static void arm_modes(void) {
  g_arm = "modes";
  enum { NP = sizeof TS_PEAK_ROWS / sizeof TS_PEAK_ROWS[0] };
  OmxRoomModePeaks pos[NP];
  OmxRoomModePeak *store[NP];
  for (uint32_t p = 0; p < NP; p++) {
    const struct ts_peaks *t = &TS_PEAK_ROWS[p];
    peaks_of(t->fs, &pos[p], &store[p]);
    exact(pos[p].code, t->code, "a position's refusal code");
    if (pos[p].code != 0 || t->code != 0) continue;
    near(pos[p].bin_hz, t->bin_hz, 1.0, "a position's bin width");
    exact(pos[p].n, t->n, "how many peaks a position reads");
    for (uint32_t i = 0; i < pos[p].n && i < t->n; i++) {
      const double *w = &TS_PEAKS[3u * (t->off + i)];
      near(pos[p].peaks[i].hz, w[0], 1.0, "a peak's frequency");
      near(pos[p].peaks[i].q, w[1], 1.0, "a peak's Q");
      near(pos[p].peaks[i].above_median_db, w[2], 1.0, "a peak's height above the median");
    }
  }
  for (size_t r = 0; r < sizeof TS_MODE_ROWS / sizeof TS_MODE_ROWS[0]; r++) {
    const struct ts_modes *t = &TS_MODE_ROWS[r];
    OmxRoomModePeaks set[3];
    for (uint32_t j = 0; j < t->np; j++) set[j] = pos[t->pos[j]];
    OmxRoomMode out[16];
    uint32_t n = 0;
    const enum omx_analysis_refusal code = omx_room_modes(set, t->np, TS_ROOM_MODE_MIN_POSITIONS, out, 16u, &n);
    exact(code, t->code, "the modes' refusal code");
    exact(n, t->n, "how many modes");
    for (uint32_t i = 0; i < n && i < t->n; i++) {
      const double *w = &TS_MODES[4u * (t->off + i)];
      near(out[i].hz, w[0], 1.0, "a mode's frequency");
      exact(out[i].positions, w[1], "a mode's positions");
      near(out[i].q, w[2], 1.0, "a mode's Q");
      near(out[i].above_median_db, w[3], 1.0, "a mode's height above the median");
    }
  }
  for (uint32_t p = 0; p < NP; p++) free(store[p]);
}

static void arm_mode_eq(void) {
  g_arm = "mode-eq";
  /* The bound the C derives from omx-contract is the one core derives from its declarations. */
  near(OMX_FBS_MAX_NOTCH_DEPTH_DB < OMX_EQ_GAIN_RANGE_MAX ? OMX_FBS_MAX_NOTCH_DEPTH_DB : OMX_EQ_GAIN_RANGE_MAX,
       TS_ROOM_MODE_MAX_CUT_DB, 1.0, "the deepest cut");
  for (size_t r = 0; r < sizeof TS_PROPOSALS / sizeof TS_PROPOSALS[0]; r++) {
    const struct ts_proposals *t = &TS_PROPOSALS[r];
    OmxRoomMode in[16];
    for (uint32_t i = 0; i < t->in_n; i++) {
      const double *w = &TS_PROPOSAL_IN[4u * (t->in_off + i)];
      in[i] = (OmxRoomMode){w[0], (uint32_t)w[1], w[2], w[3]};
    }
    OmxRoomModeEqProposal out[16];
    uint32_t n = 0;
    omx_room_mode_eq_proposals(in, t->in_n, TS_ROOM_MODE_MIN_POSITIONS, out, 16u, &n);
    exact(n, t->n, "how many proposals");
    for (uint32_t i = 0; i < n && i < t->n; i++) {
      const double *w = &TS_PROPOSAL_OUT[6u * (t->off + i)];
      near(out[i].hz, w[0], 1.0, "a proposal's frequency");
      near(out[i].gain_db, w[1], 1.0, "a proposal's cut, dB");
      near(out[i].q, w[2], 1.0, "a proposal's Q");
      exact(out[i].positions, w[3], "a proposal's positions");
      exact(out[i].bounded, w[4], "a proposal's bounded flag");
      near(out[i].requested_db, w[5], 1.0, "a proposal's requested cut, dB");
    }
  }
  /* The residual over the C curve of the same position. */
  OmxXspec a;
  xspec_of(TS_RESIDUAL_FS, &a);
  OmxRoomAccum acc;
  accum_alloc(&acc, a.bins);
  exact(omx_room_transfer(&a, TS_MIN_FRAMES, TS_ROOM_MODE_BAND[0], TS_ROOM_MODE_BAND[1], &acc), 0, "the residual curve");
  const uint32_t m = acc.k_hi - acc.k_lo + 1u;
  exact(m, TS_RESIDUAL_CURVE_N, "the residual curve's bins");
  double *hz = xmalloc(m * sizeof(double)), *db = xmalloc(m * sizeof(double));
  double *ph = xmalloc(m * sizeof(double)), *co = xmalloc(m * sizeof(double)), *scratch = xmalloc(m * sizeof(double));
  omx_room_curve(&acc, TS_ANALYSIS_FLOOR_DB, hz, db, ph, co);
  for (size_t r = 0; r < sizeof TS_RESIDUALS / sizeof TS_RESIDUALS[0]; r++) {
    double res = -1.0;
    const int defined = omx_room_mode_residual_db(hz, db, m, TS_RESIDUALS[r].hz, scratch, &res);
    exact(defined, TS_RESIDUALS[r].defined, "a residual is defined");
    if (defined) near(res, TS_RESIDUALS[r].value, 1.0, "a residual, dB");
  }
  double res = -1.0;
  exact(omx_room_mode_residual_db(hz, db, 0u, 100.0, scratch, &res), TS_RESIDUAL_EMPTY_DEFINED, "an empty curve's residual");
  free(hz); free(db); free(ph); free(co); free(scratch);
  accum_free(&acc);
  xspec_free(&a);

  for (size_t r = 0; r < sizeof TS_MEDIANS / sizeof TS_MEDIANS[0]; r++) {
    double scratch8[8];
    near(omx_room_median(TS_MEDIANS[r].v, TS_MEDIANS[r].n, scratch8), TS_MEDIANS[r].median, 1.0, "a median");
  }
  for (size_t r = 0; r < sizeof TS_STABLE / sizeof TS_STABLE[0]; r++) {
    const struct ts_stable *t = &TS_STABLE[r];
    OmxRoomMode b[2], af[2];
    unsigned char used[2];
    for (uint32_t i = 0; i < 2; i++) {
      b[i] = (OmxRoomMode){t->before[i], 2u, 4.0, 7.0};
      af[i] = (OmxRoomMode){t->after[i], 2u, 4.0, 7.0};
    }
    exact(omx_room_mode_set_stable(b, t->nb, af, t->na, t->tol, used), t->stable, "a mode set's stability");
  }
}

static void arm_rt60(void) {
  g_arm = "rt60";
  for (size_t d = 0; d < sizeof TS_DECAYS / sizeof TS_DECAYS[0]; d++) {
    const struct ts_decay *t = &TS_DECAYS[d];
    double *x = xmalloc(t->n * sizeof(double)), *scratch = xmalloc(t->n * sizeof(double));
    Rng rng = {t->seed};
    double env = 1.0;
    for (uint32_t i = 0; i < t->n; i++) {
      const double a = noise(&rng);
      const double b = noise(&rng);
      x[i] = a * env + t->nf * b;
      env *= t->r;
    }
    for (uint32_t b = 0; b < t->nbands; b++) {
      const double *w = &TS_RT60[3u * (t->off + b)];
      double t60 = 0.0;
      const enum omx_analysis_refusal code =
          omx_room_rt60_band(x, t->n, t->sr, w[0], TS_T20[0], TS_T20[1], scratch, &t60);
      exact(code, w[1], "a band's refusal code");
      if (code == OMX_ANALYSIS_OK && w[1] == 0.0) near(t60, w[2], 1.0, "a band's T60, ms");
    }
    free(x); free(scratch);
  }
}

static void arm_sweep(void) {
  g_arm = "sweep";
  for (size_t s = 0; s < sizeof TS_SWEEPS / sizeof TS_SWEEPS[0]; s++) {
    const struct ts_sweep *t = &TS_SWEEPS[s];
    const uint32_t n = omx_sweep_length(t->sr, t->dur);
    exact(n, t->n, "the sweep's length");
    if (n != t->n) continue;
    near((double)n / t->sr, t->realised, 1.0, "the realised duration");
    double *stim = xmalloc(n * sizeof(double)), *inv = xmalloc(n * sizeof(double)), L = 0.0;
    exact(omx_sweep_generate(t->sr, t->f0, t->f1, t->dur, t->fade_in_s, t->fade_out_s, stim, inv, n, &L), 1, "generated");
    near(L, t->L, 1.0, "L, the sweep-rate constant");
    double inv_scale = 0.0;
    for (uint32_t i = 0; i < t->ns; i++) {
      const double v = fabs(TS_SWEEP[2u * (t->off + i) + 1u]);
      inv_scale = v > inv_scale ? v : inv_scale;
    }
    for (uint32_t i = 0, j = 0; j < t->ns; i += t->stride, j++) {
      near(stim[i], TS_SWEEP[2u * (t->off + j)], 1.0, "a sweep sample");
      near(inv[i], TS_SWEEP[2u * (t->off + j) + 1u], inv_scale, "an inverse-filter sample");
    }
    double *resp = xmalloc(t->resp_len * sizeof(double));
    for (uint32_t k = 0; k < t->ntaps; k++)
      for (uint32_t i = 0; i < n; i++) resp[i + t->at[k]] += stim[i] * t->level[k];
    double *ir = xmalloc(t->length * sizeof(double));
    double *scratch = xmalloc(4u * (size_t)omx_sweep_convolve_size(t->resp_len, n) * sizeof(double));
    omx_sweep_recover_ir(resp, t->resp_len, inv, n, t->pre, ir, t->length, scratch);
    double ir_scale = 0.0;
    for (uint32_t i = 0; i < t->length; i++) {
      const double v = fabs(TS_SWEEP_IR[t->ir_off + i]);
      ir_scale = v > ir_scale ? v : ir_scale;
    }
    for (uint32_t i = 0; i < t->length; i++) near(ir[i], TS_SWEEP_IR[t->ir_off + i], ir_scale, "a recovered IR sample");
    for (uint32_t k = 0; k < 5; k++)
      near(omx_sweep_harmonic_delay(L, t->sr, TS_HARMONIC_ORDERS[k]), TS_SWEEP_HARMONIC[t->harm_off + k], 1.0,
           "a harmonic's delay, samples");
    exact(n - 1u, t->lag, "the linear-response lag");
    free(stim); free(inv); free(resp); free(ir); free(scratch);
  }
  for (size_t c = 0; c < sizeof TS_CONVOLVES / sizeof TS_CONVOLVES[0]; c++) {
    const struct ts_convolve *t = &TS_CONVOLVES[c];
    double scratch[4u * 16u];
    exact(4u * omx_sweep_convolve_size(t->na, t->nb) <= 64u, 1, "the scratch fits");
    const uint32_t len = omx_sweep_convolve(t->a, t->na, t->b, t->nb, scratch);
    exact(len, t->n, "the convolution's length");
    for (uint32_t i = 0; i < len && i < t->n; i++) near(scratch[i], t->out[i], 1.0, "a convolution sample");
  }
}

int main(void) {
  arm_fit();
  arm_coarse();
  arm_correction();
  arm_transfer();
  arm_impulse();
  arm_ir();
  arm_modes();
  arm_mode_eq();
  arm_rt60();
  arm_sweep();
  printf("analysis/room_ts: %d checks, %d failed; largest gap from core's TypeScript %.3g (bound %.0e)\n", g_checks,
         g_fail, g_worst, ROOM_TS_TOL);
  return g_fail ? 1 : 0;
}
