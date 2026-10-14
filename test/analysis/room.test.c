// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * room.test.c — room analysis (<omxdsp/analysis/omx_room.h>) against closed forms, at the nine RME
 * rates on the RTA's own frame size for each rate (analysis_rates.h), with core's declarations
 * from the parity table. Every run is a synthetic room whose answer is known:
 *
 *   1. TRANSFER: a target that is a tilted noise reference through `g·e^{−iωd}` reads 20·log10 g
 *      at every band bin (1e-9 dB), whatever the reference's tilt (the H1 estimate divides it out),
 *      a phase of −360·f·d/rate unwrapped across the band (1e-6°), and coherence 1; the band's bins
 *      are max(1, ⌈low/binHz⌉) and min(n/2 − 2, ⌊high/binHz⌋); one impulse `g·δ(t − d)` reads the
 *      same curve through the impulse-response path, padded to the next power of two (none past
 *      2^31, which refuses);
 *   2. IMPULSE: a direct arrival at d0 and copies at +10 ms (×0.5), +20 ms (×0.02, under the −30 dB
 *      floor) and +100 ms (×0.5, past the 80 ms horizon) name the direct arrival at d0 exactly and
 *      exactly one reflection, at 10 ms (to the sample), 3.43 m, −6.02 dB within 0.2 dB; a negative
 *      or NaN horizon lists none;
 *   3. RT60: a sinusoid at a band's centre under an exponential decay, `r^i·sin(2π·fc·i/rate)` with
 *      `r = 10^(−3/(T60·rate))`, is a complex exponential, so the bandpass passes it with exactly its
 *      decay and its Schroeder curve falls 60/T60 dB a second: every octave band below Nyquist reads
 *      its dialled T60 (300 ms to 1.2 s) within 1 % (the filter's few-ms start and the sin² ripple
 *      are all that is left); an empty capture refuses no-signal, one under twenty periods of
 *      the band's centre too-short, and one whose energy sits in its last sample insufficient-decay
 *      (its Schroeder curve never leaves 0 dB);
 *   4. MODES: three positions share resonances `A/(1 + x²)` at 45, 150 and 260 Hz, x = (f − f0)/b,
 *      A = 6, over a unit floor (|H| falls to its peak's half power at
 *      x_h = √(A/((1 + A)/√2 − 1) − 1), so Q = f0/(2·b·x_h)); each position adds a reflection ×0.1 of its own (3, 4.1 or 5.3 ms) and the
 *      second alone a fourth, narrower one at 205 Hz: exactly the three shared resonances are
 *      confirmed, each within one bin of f0 with Q within 25 % (the half-power width is read off a
 *      ~5.9 Hz grid), and the peak the second position reads alone is not; one position refuses
 *      too-few-positions; a third position leaves the set of the first two stable;
 *   5. MODE EQ: a mode `h` dB above the median proposes a cut of −h dB at its Q, bounded at
 *      −min(18, 15) dB and inside Q 0.3..8 with the bound flagged, none for a mode under two
 *      positions or at the median, deepest first with ties in input order; the residual is the
 *      nearest bin's height above the curve's median, floored at 0; the median is the middle value.
 *
 * The kernels run between room_count_begin() and room_count_end(): they allocate nothing.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_room.h>

#include "analysis_rates.h"
#include "fixtures/room_ts.h"
#include "room_oracle.h"

typedef struct {
  uint32_t n, bins;
  double *xr, *xi, *pxx, *pyy;
  double *tr, *ti, *rr, *ri;
  double *sr, *si;
  OmxRoomAccum acc;
  double *hz, *db, *ph, *co, *scratch;
} Bufs;

static void bufs_alloc(Bufs *b, uint32_t n) {
  b->n = n;
  b->bins = n / 2u;
  const size_t bytes = b->bins * sizeof(double);
  b->xr = room_alloc(bytes); b->xi = room_alloc(bytes); b->pxx = room_alloc(bytes); b->pyy = room_alloc(bytes);
  b->tr = room_alloc(bytes); b->ti = room_alloc(bytes); b->rr = room_alloc(bytes); b->ri = room_alloc(bytes);
  b->sr = room_alloc(n * sizeof(double)); b->si = room_alloc(n * sizeof(double));
  b->acc.re = room_alloc(bytes); b->acc.im = room_alloc(bytes); b->acc.coherence = room_alloc(bytes);
  b->hz = room_alloc(bytes); b->db = room_alloc(bytes); b->ph = room_alloc(bytes); b->co = room_alloc(bytes);
  b->scratch = room_alloc(3u * bytes);
}

static void bufs_free(Bufs *b) {
  free(b->xr); free(b->xi); free(b->pxx); free(b->pyy);
  free(b->tr); free(b->ti); free(b->rr); free(b->ri);
  free(b->sr); free(b->si);
  free(b->acc.re); free(b->acc.im); free(b->acc.coherence);
  free(b->hz); free(b->db); free(b->ph); free(b->co); free(b->scratch);
}

/* The synthetic room's transfer at bin k. */
typedef struct {
  double g, d;            /* the direct path: gain, delay in samples */
  uint32_t ncopies;
  double cg[3], cd[3];    /* delayed copies: gain, delay in samples */
  uint32_t nmodes;
  double f0[4], bw[4], amp[4]; /* resonances A/(1 + ((f − f0)/b)²): centre, b in Hz, A */
} Room;

static void room_at(const Room *r, double k, double n, double rate, double *hr, double *hi) {
  double re = r->g * cos(-2.0 * M_PI * k * r->d / n), im = r->g * sin(-2.0 * M_PI * k * r->d / n);
  for (uint32_t c = 0; c < r->ncopies; c++) {
    re += r->cg[c] * cos(-2.0 * M_PI * k * r->cd[c] / n);
    im += r->cg[c] * sin(-2.0 * M_PI * k * r->cd[c] / n);
  }
  const double f = k * rate / n;
  for (uint32_t m = 0; m < r->nmodes; m++) {
    const double x = (f - r->f0[m]) / r->bw[m];
    re += r->amp[m] / (1.0 + x * x);
  }
  *hr = re;
  *hi = im;
}

/* Fold `frames` noise-free frames of a tilted noise reference through the room. */
static void run(Bufs *b, OmxXspec *a, double rate, uint32_t frames, const Room *room, uint32_t seed) {
  RoomRng rng = {seed};
  omx_xspec_init(a, b->xr, b->xi, b->pxx, b->pyy, b->n, rate, 0.0);
  for (uint32_t f = 0; f < frames; f++) {
    for (uint32_t k = 0; k < b->bins; k++) {
      const double tilt = 1.0 / (1.0 + (double)k / 64.0);
      const double rr = room_noise(&rng) * tilt, ri = room_noise(&rng) * tilt;
      double hr, hi;
      room_at(room, (double)k, (double)b->n, rate, &hr, &hi);
      b->rr[k] = rr;
      b->ri[k] = ri;
      b->tr[k] = hr * rr - hi * ri;
      b->ti[k] = hr * ri + hi * rr;
    }
    omx_xspec_add(a, b->tr, b->ti, b->rr, b->ri, b->bins, b->n, rate, 0.0);
  }
}

/* The phase a delay of d samples gives bin k, degrees, folded into (−180, 180]. */
static double wrapped_deg(double k, double d, double n) {
  double p = fmod(-360.0 * k * d / n, 360.0);
  if (p <= -180.0) p += 360.0;
  if (p > 180.0) p -= 360.0;
  return p;
}

static void check_flat_curve(Bufs *b, double gain, double d, const char *what) {
  const OmxRoomAccum *acc = &b->acc;
  const uint32_t m = acc->k_hi - acc->k_lo + 1u;
  room_count_begin();
  omx_room_curve(acc, TS_ANALYSIS_FLOOR_DB, b->hz, b->db, b->ph, b->co);
  room_count_end();
  const double n = (double)acc->fft_size;
  double worst_db = 0.0, worst_ph = 0.0, worst_co = 0.0, worst_hz = 0.0;
  for (uint32_t i = 0; i < m; i++) {
    const double k = (double)(acc->k_lo + i);
    worst_hz = room_worse(worst_hz, fabs(b->hz[i] - k * acc->sample_rate / n));
    worst_db = room_worse(worst_db, fabs(b->db[i] - 20.0 * log10(gain)));
    worst_ph = room_worse(worst_ph, fabs((b->ph[i] - b->ph[0]) - (-360.0 * (k - (double)acc->k_lo) * d / n)));
    worst_co = room_worse(worst_co, fabs(b->co[i] - 1.0));
  }
  g_arm = what;
  check(worst_hz < 1e-9, "bin k sits at k·rate/n", worst_hz, 0);
  check(worst_db < 1e-9, "the magnitude is 20·log10 g at every bin", worst_db, 0);
  check(worst_ph < 1e-6, "the phase falls 360·d/n degrees a bin, unwrapped", worst_ph, 0);
  check(fabs(b->ph[0] - wrapped_deg((double)acc->k_lo, d, n)) < 1e-6, "the first bin's phase, wrapped", b->ph[0],
        wrapped_deg((double)acc->k_lo, d, n));
  check(worst_co < 1e-12, "coherence 1", worst_co, 0);
}

static void arm_transfer(Bufs *b, double rate) {
  OmxXspec a;
  const double lo = TS_DEFAULT_BAND[0], hi = TS_DEFAULT_BAND[1];
  const Room room = {.g = 0.5, .d = 21.3};
  run(b, &a, rate, 4, &room, 1u);
  room_count_begin();
  const enum omx_analysis_refusal code = omx_room_transfer(&a, TS_MIN_FRAMES, lo, hi, &b->acc);
  room_count_end();
  g_arm = "transfer";
  check(code == OMX_ANALYSIS_OK, "a delayed gain reads a curve", code, 0);
  const double bin_hz = rate / b->n;
  check(b->acc.k_lo == (uint32_t)fmax(1.0, ceil(lo / bin_hz)), "the band's first bin", b->acc.k_lo, ceil(lo / bin_hz));
  check(b->acc.k_hi == (uint32_t)fmin(b->bins - 2.0, floor(hi / bin_hz)), "the band's last bin", b->acc.k_hi,
        floor(hi / bin_hz));
  check(b->acc.frames == 4u, "four frames", b->acc.frames, 4);
  check(b->acc.re[b->acc.k_lo - 1u] == 0.0 && b->acc.re[b->acc.k_hi + 1u] == 0.0, "zero outside the band", 0, 0);
  check_flat_curve(b, 0.5, 21.3, "transfer curve");

  g_arm = "transfer refusals";
  run(b, &a, rate, 3, &room, 2u);
  check(omx_room_transfer(&a, TS_MIN_FRAMES, lo, hi, &b->acc) == OMX_ANALYSIS_TOO_FEW_FRAMES, "three frames refuse", 0, 0);
  run(b, &a, rate, 4, &room, 3u);
  omx_xspec_add(&a, b->tr, b->ti, b->rr, b->ri, b->bins, b->n / 2u, rate, 0.0);
  check(omx_room_transfer(&a, TS_MIN_FRAMES, lo, hi, &b->acc) == OMX_ANALYSIS_SHAPE_MISMATCH, "another length refuses", 0, 0);
  run(b, &a, rate, 4, &room, 4u);
  omx_xspec_add(&a, b->tr, b->ti, b->rr, b->ri, b->bins, b->n, rate, 128.0);
  check(omx_room_transfer(&a, TS_MIN_FRAMES, lo, hi, &b->acc) == OMX_ANALYSIS_OK, "another offset does not (no offset is read)",
        0, 0);
  check(omx_room_transfer(&a, TS_MIN_FRAMES, hi, lo, &b->acc) == OMX_ANALYSIS_EMPTY_BAND, "an inverted band refuses", 0, 0);
  const Room silent = {.g = 0.0};
  run(b, &a, rate, 4, &silent, 5u);
  check(omx_room_transfer(&a, TS_MIN_FRAMES, lo, hi, &b->acc) == OMX_ANALYSIS_NO_SHARED_ENERGY, "a silent target refuses",
        0, 0);

  /* One impulse g·δ(t − d), read through the impulse-response path. */
  g_arm = "transfer from an IR";
  const uint32_t len = b->n / 2u + 3u, d = 37u;
  double *ir = b->scratch;
  memset(ir, 0, len * sizeof(double));
  ir[d] = 0.25;
  check(omx_room_ir_fft_size(len) == b->n, "padded to the next power of two", omx_room_ir_fft_size(len), b->n);
  check(omx_room_ir_fft_size(0x80000000u) == 0x80000000u && omx_room_ir_fft_size(0x80000001u) == 0u,
        "2^31 is the largest padding; past it none", 0, 0);
  check(omx_room_transfer_from_ir(ir, 0x80000001u, rate, lo, hi, b->sr, b->si, &b->acc) == OMX_ANALYSIS_SHAPE_MISMATCH,
        "a response past 2^31 refuses before touching a buffer", 0, 0);
  room_count_begin();
  const enum omx_analysis_refusal ic = omx_room_transfer_from_ir(ir, len, rate, lo, hi, b->sr, b->si, &b->acc);
  room_count_end();
  check(ic == OMX_ANALYSIS_OK && b->acc.frames == 1u && b->acc.fft_size == b->n, "an impulse reads a curve of one frame", ic, 0);
  check_flat_curve(b, 0.25, (double)d, "IR curve");
  memset(ir, 0, len * sizeof(double));
  check(omx_room_transfer_from_ir(ir, len, rate, lo, hi, b->sr, b->si, &b->acc) == OMX_ANALYSIS_NO_SHARED_ENERGY,
        "a silent response refuses", 0, 0);
  check(omx_room_transfer_from_ir(ir, len, rate, rate, 2.0 * rate, b->sr, b->si, &b->acc) == OMX_ANALYSIS_EMPTY_BAND,
        "a band above Nyquist refuses", 0, 0);

  g_arm = "unwrap";
  double ramp[64];
  for (int i = 0; i < 64; i++) ramp[i] = atan2(sin(-0.9 * i + 0.3), cos(-0.9 * i + 0.3));
  omx_room_unwrap(ramp, 64u);
  double worst = 0.0;
  for (int i = 0; i < 64; i++) worst = room_worse(worst, fabs(ramp[i] - (-0.9 * i + 0.3)));
  check(worst < 1e-12, "a wrapped ramp unwraps to the ramp", worst, 0);
}

static void arm_impulse(Bufs *b, double rate) {
  OmxXspec a;
  const double ms = rate / 1000.0;
  const double d0 = round(4.0 * ms), d1 = round(10.0 * ms);
  const Room room = {.g = 1.0, .d = d0, .ncopies = 3, .cg = {0.5, 0.02, 0.5},
                     .cd = {d0 + d1, d0 + round(20.0 * ms), d0 + round(100.0 * ms)}};
  run(b, &a, rate, 4, &room, 6u);
  g_arm = "impulse";
  check(omx_room_transfer(&a, TS_MIN_FRAMES, TS_DEFAULT_BAND[0], TS_DEFAULT_BAND[1], &b->acc) == OMX_ANALYSIS_OK,
        "the room reads a curve", 0, 0);
  const uint32_t cap = omx_room_impulse_capacity(&b->acc, TS_EARLY_REFLECTION_WINDOW_MS);
  OmxRoomReflection *r = room_alloc(cap * sizeof *r);
  uint32_t direct = 0, nr = 0;
  room_count_begin();
  const enum omx_analysis_refusal code = omx_room_impulse(&b->acc, TS_EARLY_REFLECTION_WINDOW_MS,
                                                          TS_EARLY_REFLECTION_FLOOR_DB, b->sr, b->si, &direct, r, cap, &nr);
  room_count_end();
  check(code == OMX_ANALYSIS_OK, "an impulse", code, 0);
  check(direct == (uint32_t)d0, "the direct arrival at d0", direct, d0);
  check(nr == 1u, "one reflection: one under the floor, one past the horizon", nr, 1);
  if (nr >= 1u) {
    check(r[0].samples == (uint32_t)d1, "at +10 ms, to the sample", r[0].samples, d1);
    check(fabs(r[0].arrival_ms - d1 / ms) < 1e-9, "its arrival, ms", r[0].arrival_ms, d1 / ms);
    check(fabs(r[0].distance_m - r[0].arrival_ms * 0.343) < 1e-12, "its extra path, arrival × 0.343", r[0].distance_m,
          r[0].arrival_ms * 0.343);
    check(fabs(r[0].level_db - 20.0 * log10(0.5)) < 0.2, "its level, −6.02 dB", r[0].level_db, 20.0 * log10(0.5));
  }
  /* A horizon that names no sample (negative or NaN) lists no reflection and still finds the direct
   * arrival. */
  const double no_horizon[2] = {-1.0, NAN};
  for (int h = 0; h < 2; h++) {
    uint32_t d = 0, nn = 7;
    check(omx_room_impulse(&b->acc, no_horizon[h], TS_EARLY_REFLECTION_FLOOR_DB, b->sr, b->si, &d, r, cap, &nn) ==
              OMX_ANALYSIS_OK && d == (uint32_t)d0 && nn == 0u,
          "no horizon, no reflection", nn, 0);
  }
  free(r);
  g_arm = "impulse refusal";
  memset(b->acc.re, 0, b->bins * sizeof(double));
  memset(b->acc.im, 0, b->bins * sizeof(double));
  r = room_alloc(cap * sizeof *r);
  check(omx_room_impulse(&b->acc, TS_EARLY_REFLECTION_WINDOW_MS, TS_EARLY_REFLECTION_FLOOR_DB, b->sr, b->si, &direct, r,
                         cap, &nr) == OMX_ANALYSIS_NO_SHARED_ENERGY,
        "a silent estimate refuses", 0, 0);
  free(r);
}

static void arm_rt60(double rate) {
  const uint32_t nb = sizeof TS_OCTAVE_BAND_CENTRES_HZ / sizeof TS_OCTAVE_BAND_CENTRES_HZ[0];
  const uint32_t n = (uint32_t)(1.5 * rate);
  double *x = room_alloc(n * sizeof(double)), *scratch = room_alloc(n * sizeof(double));
  g_arm = "rt60";
  for (uint32_t k = 0; k < nb; k++) {
    const double hz = TS_OCTAVE_BAND_CENTRES_HZ[k];
    if (hz > 0.4 * rate) continue;
    const double t60 = 0.3 + 0.9 * (double)k / (double)(nb - 1u);
    const double r = pow(10.0, -3.0 / (t60 * rate));
    double env = 1.0;
    for (uint32_t i = 0; i < n; i++) {
      x[i] = env * sin(2.0 * M_PI * hz * (double)i / rate);
      env *= r;
    }
    double t = 0.0;
    room_count_begin();
    const enum omx_analysis_refusal code = omx_room_rt60_band(x, n, rate, hz, TS_T20[0], TS_T20[1], scratch, &t);
    room_count_end();
    check(code == OMX_ANALYSIS_OK, "a decaying band reads a T60", code, 0);
    check(fabs(t - t60 * 1000.0) / (t60 * 1000.0) < 0.01, "T60 within 1 %", t, t60 * 1000.0);
  }
  RoomRng rng = {77u};
  g_arm = "rt60 refusals";
  double t = 0.0;
  check(omx_room_rt60_band(x, 0u, rate, 125.0, TS_T20[0], TS_T20[1], scratch, &t) == OMX_ANALYSIS_NO_SIGNAL,
        "an empty capture refuses no-signal", 0, 0);
  const uint32_t twenty = (uint32_t)ceil(rate / 125.0 * 20.0);
  check(omx_room_rt60_band(x, twenty - 1u, rate, 125.0, TS_T20[0], TS_T20[1], scratch, &t) == OMX_ANALYSIS_TOO_SHORT,
        "under twenty periods refuses too-short", 0, 0);
  for (uint32_t i = 0; i < n; i++) x[i] = 1e-6 * room_noise(&rng);
  x[n - 1u] = 1e3;
  check(omx_room_rt60_band(x, n, rate, 1000.0, TS_T20[0], TS_T20[1], scratch, &t) == OMX_ANALYSIS_INSUFFICIENT_DECAY,
        "energy only at the end refuses insufficient-decay", 0, 0);
  free(x);
  free(scratch);
}

static void positions_peaks(Bufs *b, double rate, const Room *rooms, uint32_t np, OmxRoomModePeaks *pos,
                            OmxRoomModePeak (*peaks)[64]) {
  for (uint32_t p = 0; p < np; p++) {
    OmxXspec a;
    run(b, &a, rate, 4, &rooms[p], 20u + p);
    check(omx_room_transfer(&a, TS_MIN_FRAMES, TS_ROOM_MODE_BAND[0], TS_ROOM_MODE_BAND[1], &b->acc) == OMX_ANALYSIS_OK,
          "a position reads a curve", 0, 0);
    room_count_begin();
    omx_room_mode_peaks(&b->acc, TS_ANALYSIS_FLOOR_DB, TS_ROOM_MODE_MIN_ABOVE_MEDIAN_DB, b->scratch, peaks[p], 64u, &pos[p]);
    room_count_end();
  }
}

static void arm_modes(Bufs *b, double rate) {
  g_arm = "modes";
  const double f0[3] = {45.0, 150.0, 260.0}, bw[3] = {9.0, 12.0, 12.0}, amp = 6.0;
  const double x_h = sqrt(amp / ((1.0 + amp) / M_SQRT2 - 1.0) - 1.0);
  const double comb[3] = {3.0e-3, 4.1e-3, 5.3e-3};
  Room rooms[3];
  for (uint32_t p = 0; p < 3; p++) {
    rooms[p] = (Room){.g = 1.0, .d = 0.0, .ncopies = 1, .cg = {0.1}, .cd = {comb[p] * rate}, .nmodes = p == 1u ? 4u : 3u,
                      .f0 = {f0[0], f0[1], f0[2], 205.0}, .bw = {bw[0], bw[1], bw[2], 5.0}, .amp = {amp, amp, amp, amp}};
  }
  OmxRoomModePeaks pos[3];
  OmxRoomModePeak peaks[3][64];
  positions_peaks(b, rate, rooms, 3, pos, peaks);
  const double bin_hz = rate / b->n;
  int alone = 0;
  for (uint32_t i = 0; i < pos[1].n; i++) alone |= fabs(pos[1].peaks[i].hz - 205.0) <= bin_hz;
  check(alone, "the second position reads its own 205 Hz peak", alone, 1);

  OmxRoomMode modes[16];
  uint32_t nm = 0;
  room_count_begin();
  const enum omx_analysis_refusal code = omx_room_modes(pos, 3u, TS_ROOM_MODE_MIN_POSITIONS, modes, 16u, &nm);
  room_count_end();
  check(code == OMX_ANALYSIS_OK, "three positions confirm", code, 0);
  check(nm == 3u, "exactly the three resonances", nm, 3);
  for (uint32_t m = 0; m < nm && m < 3u; m++) {
    const double q = f0[m] / (2.0 * bw[m] * x_h);
    check(fabs(modes[m].hz - f0[m]) <= bin_hz, "a mode within one bin of its resonance", modes[m].hz, f0[m]);
    check(fabs(modes[m].q - q) / q < 0.25, "its Q within 25 % of f0/(2·b·x_h)", modes[m].q, q);
    check(modes[m].positions == 3u, "confirmed at three positions", modes[m].positions, 3);
    check(modes[m].above_median_db >= TS_ROOM_MODE_MIN_ABOVE_MEDIAN_DB, "above the median by the margin",
          modes[m].above_median_db, TS_ROOM_MODE_MIN_ABOVE_MEDIAN_DB);
  }
  uint32_t n1 = 0;
  check(omx_room_modes(pos, 1u, TS_ROOM_MODE_MIN_POSITIONS, modes, 16u, &n1) == OMX_ANALYSIS_TOO_FEW_POSITIONS,
        "one position refuses", 0, 0);
  OmxRoomMode two[16];
  uint32_t n2 = 0;
  omx_room_modes(pos, 2u, TS_ROOM_MODE_MIN_POSITIONS, two, 16u, &n2);
  omx_room_modes(pos, 3u, TS_ROOM_MODE_MIN_POSITIONS, modes, 16u, &nm);
  unsigned char used[16];
  room_count_begin();
  const int stable = omx_room_mode_set_stable(two, n2, modes, nm, bin_hz, used);
  room_count_end();
  check(stable == 1, "a third position leaves the set stable", stable, 1);
  check(omx_room_mode_set_stable(two, n2, modes, nm - 1u, bin_hz, used) == 0, "one mode fewer is not stable", 0, 0);
  OmxRoomModePeaks bad[2] = {pos[0], pos[1]};
  bad[1].code = OMX_ANALYSIS_TOO_FEW_FRAMES;
  check(omx_room_modes(bad, 2u, TS_ROOM_MODE_MIN_POSITIONS, modes, 16u, &n1) == OMX_ANALYSIS_TOO_FEW_FRAMES,
        "a refusing position's refusal", 0, 0);
  bad[1] = pos[1];
  bad[1].fft_size *= 2u;
  check(omx_room_modes(bad, 2u, TS_ROOM_MODE_MIN_POSITIONS, modes, 16u, &n1) == OMX_ANALYSIS_SHAPE_MISMATCH,
        "positions of two lengths refuse", 0, 0);
}

static void arm_mode_eq(void) {
  g_rate = 0.0;
  g_arm = "mode eq";
  const OmxRoomMode in[7] = {{50.0, 3u, 4.0, 10.0}, {70.0, 3u, 0.1, 40.0}, {90.0, 2u, 20.0, 10.0}, {110.0, 1u, 4.0, 12.0},
                             {130.0, 2u, 4.0, 0.0}, {150.0, 2u, 6.0, 3.0}, {170.0, 4u, 8.0, 10.0}};
  OmxRoomModeEqProposal out[7];
  uint32_t n = 0;
  room_count_begin();
  omx_room_mode_eq_proposals(in, 7u, TS_ROOM_MODE_MIN_POSITIONS, out, 7u, &n);
  room_count_end();
  const double cut = TS_ROOM_MODE_MAX_CUT_DB;
  check(cut == fmin(OMX_FBS_MAX_NOTCH_DEPTH_DB, OMX_EQ_GAIN_RANGE_MAX), "the deepest cut is min(18, 15)", cut, 15);
  check(n == 5u, "five proposals: one mode under two positions, one at the median", n, 5);
  /* Deepest first: 70 Hz (bounded −15), then the −10s in input order, then −3. */
  const double hz[5] = {70.0, 50.0, 90.0, 170.0, 150.0}, gain[5] = {-cut, -10.0, -10.0, -10.0, -3.0};
  const double q[5] = {OMX_EQ_Q_RANGE_MIN_DOUBLE, 4.0, OMX_EQ_Q_RANGE_MAX, 8.0, 6.0};
  const uint32_t bounded[5] = {1u, 0u, 1u, 0u, 0u};
  const double requested[5] = {-40.0, -10.0, -10.0, -10.0, -3.0};
  for (uint32_t i = 0; i < n && i < 5u; i++) {
    check(out[i].hz == hz[i], "the order and the frequency", out[i].hz, hz[i]);
    check(out[i].gain_db == gain[i], "the cut", out[i].gain_db, gain[i]);
    check(out[i].q == q[i], "the Q inside the travel", out[i].q, q[i]);
    check(out[i].bounded == bounded[i], "bounded when a ceiling bit", out[i].bounded, bounded[i]);
    check(out[i].requested_db == requested[i], "the cut the room asked for", out[i].requested_db, requested[i]);
  }

  g_arm = "residual";
  const double chz[5] = {50.0, 60.0, 70.0, 80.0, 90.0}, cdb[5] = {-3.0, 9.0, 1.0, -1.0, 0.0};
  double scratch[5], res = -1.0;
  room_count_begin();
  check(omx_room_mode_residual_db(chz, cdb, 5u, 61.0, scratch, &res) == 1, "a curve has a residual", 0, 0);
  room_count_end();
  check(res == 9.0, "the nearest bin's height above the median (0 dB)", res, 9.0);
  omx_room_mode_residual_db(chz, cdb, 5u, 49.0, scratch, &res);
  check(res == 0.0, "a bin under the median reads 0", res, 0.0);
  check(omx_room_mode_residual_db(chz, cdb, 0u, 61.0, scratch, &res) == 0, "an empty curve has none", 0, 0);

  g_arm = "median";
  const double odd[5] = {4.0, -1.0, 7.0, 2.0, 2.5}, even[4] = {4.0, -1.0, 7.0, 2.0};
  check(omx_room_median(odd, 5u, scratch) == 2.5, "the middle of five", omx_room_median(odd, 5u, scratch), 2.5);
  check(omx_room_median(even, 4u, scratch) == 3.0, "the mean of the middle two", omx_room_median(even, 4u, scratch), 3.0);
}

int main(void) {
  omx_analysis_require_rates();
  for (int r = 0; r < OMX_ANALYSIS_RATE_COUNT; r++) {
    g_rate = OMX_ANALYSIS_RATES[r];
    Bufs b;
    bufs_alloc(&b, omx_analysis_fft_for_rate(g_rate));
    arm_transfer(&b, g_rate);
    arm_impulse(&b, g_rate);
    arm_modes(&b, g_rate);
    bufs_free(&b);
    arm_rt60(g_rate);
  }
  arm_mode_eq();
  return room_report("room");
}
