// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The meter view law's scenarios, run by tools/meter-view-golden.c to write vectors/meter-view.json and by the
 * suite's mix_meter_view arm to check the kernel against that file. One place, so the file and the check run the
 * same scenario.
 *
 *   held   at each oracle rate and quantum: a 10 ms burst at -6 dBFS 100 ms in, silence around it, 1.2 s. The
 *          slot keeps the block peaks (omx_meter_hold_peak) until the host reads it at METER_VIEW hostReadHz; each
 *          read gives the fill and steps the marker by the time since the last read.
 *   gr     at each oracle rate and quantum: a -12 dB reduction from 100 to 150 ms, a -4 dB one from 600 to 620 ms,
 *          none elsewhere, 1.5 s, a 500 ms release. The reader keeps the deepest block (omx_meter_view_decimate)
 *          and steps the reading by the time since its last read (omx_meter_view_gr).
 *   fall   at each draw rate: the marker at full scale, then silence (or a steady -30 dBFS) for 4 s.
 */
#ifndef OMXDSP_TEST_METER_VIEW_SCENARIOS_H
#define OMXDSP_TEST_METER_VIEW_SCENARIOS_H

#include <math.h>
#include <stdint.h>

#include <omxdsp/mix/omx_meter.h>
#include <omxdsp/mix/omx_meter_view.h>
#include <omxdsp/omx_contract.h>

static const float MV_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;
#define MV_RATES_COUNT OMX_ORACLE_FULL_RATES_COUNT
static const uint32_t MV_QUANTA[] = {32u, 64u, 128u, 256u, 512u, 1024u};
#define MV_QUANTA_COUNT 6u
static const double MV_DRAW_HZ[] = {5.0, 20.0, 30.0, 60.0};
#define MV_DRAW_COUNT 4u
#define MV_FALL_SECONDS 4.0
/** The most reads one scenario makes: 1.5 s at 50 Hz, with room for a quantum longer than a read. */
#define MV_MAX_READS 96u
#define MV_MAX_TICKS 241u

/** One scenario's reads: the input each read sees and what the view answers. */
struct mv_reads {
  uint32_t n;
  double in[MV_MAX_READS];  /* held: the slot's peak, linear; gr: the deepest block, dB */
  double dt[MV_MAX_READS];  /* seconds since the last read */
  double out[MV_MAX_READS]; /* held: the fill; gr: the reading, dB */
  double marker[MV_MAX_READS];
};

/** True when `frames` since the last read reach one host read period at `sr`. */
static inline int mv_read_due(uint32_t frames, float sr) {
  return (double)frames * OMX_METER_VIEW_HOST_READ_HZ >= (double)sr;
}

static inline void mv_held(float sr, uint32_t q, struct mv_reads *r) {
  const uint32_t total = (uint32_t)lround(1.2 * sr), b0 = (uint32_t)lround(0.1 * sr), b1 = (uint32_t)lround(0.11 * sr);
  float buf[1024];
  struct omx_meter_view v = {0};
  float slot = 0.0f;
  uint32_t since = 0;
  r->n = 0;
  for (uint32_t s = 0; s + q <= total && r->n < MV_MAX_READS; s += q) {
    for (uint32_t i = 0; i < q; i++) {
      const uint32_t k = s + i;
      buf[i] = (k >= b0 && k < b1) ? (float)(0.5 * sin(2.0 * 3.14159265358979323846 * 1000.0 * (double)(k - b0) / (double)sr)) : 0.0f;
    }
    slot = omx_meter_hold_peak(slot, omx_meter_block_peak(buf, NULL, q));
    since += q;
    if (!mv_read_due(since, sr)) continue;
    const double dt = (double)since / (double)sr;
    const double fill = omx_meter_view_fill((double)slot, OMX_METER_VIEW_FLOOR_DB);
    r->in[r->n] = (double)slot;
    r->dt[r->n] = dt;
    r->out[r->n] = fill;
    r->marker[r->n] = omx_meter_view_step(&v, fill, dt, OMX_METER_VIEW_FALL_DB_PER_SECOND, OMX_METER_VIEW_FLOOR_DB);
    r->n++;
    slot = 0.0f;
    since = 0;
  }
}

/** The release pole of a 500 ms release at `sr`, per frame. */
static inline float mv_pole(float sr) {
  return expf(-1.0f / (0.5f * sr));
}

static inline void mv_gr(float sr, uint32_t q, struct mv_reads *r) {
  const uint32_t total = (uint32_t)lround(1.5 * sr);
  const float pole = mv_pole(sr);
  struct omx_meter_gr m = {0.0f, 0.0f};
  double kept = 0.0;
  uint32_t since = 0;
  r->n = 0;
  for (uint32_t s = 0; s + q <= total && r->n < MV_MAX_READS; s += q) {
    const double t = (double)s / (double)sr;
    const double block = (t >= 0.1 && t < 0.15) ? -12.0 : (t >= 0.6 && t < 0.62) ? -4.0 : 0.0;
    kept = omx_meter_view_decimate(OMX_METER_KINDS_GAIN_REDUCTION, kept, block);
    since += q;
    if (!mv_read_due(since, sr)) continue;
    const double dt = (double)since / (double)sr;
    r->in[r->n] = kept;
    r->dt[r->n] = dt;
    r->out[r->n] = (double)omx_meter_view_gr(&m, (float)kept, dt, sr, pole);
    r->marker[r->n] = 0.0;
    r->n++;
    kept = 0.0;
    since = 0;
  }
}

/** The marker at each draw tick over MV_FALL_SECONDS after full scale: `live` the fill under it (0, silence). */
static inline uint32_t mv_fall(double hz, double live, double *marker) {
  struct omx_meter_view v = {0};
  omx_meter_view_step(&v, 1.0, 0.0, OMX_METER_VIEW_FALL_DB_PER_SECOND, OMX_METER_VIEW_FLOOR_DB);
  const uint32_t ticks = (uint32_t)lround(MV_FALL_SECONDS * hz);
  marker[0] = 1.0;
  for (uint32_t k = 1; k <= ticks; k++)
    marker[k] = omx_meter_view_step(&v, live, 1.0 / hz, OMX_METER_VIEW_FALL_DB_PER_SECOND, OMX_METER_VIEW_FLOOR_DB);
  return ticks + 1u;
}

/** The ladder: 8 segments, a ramp up and down, then a level that sits on the 4/5 edge and wobbles 0.2 of a step. */
#define MV_LADDER_STEPS 8u
#define MV_LADDER_COUNT 34u
static inline void mv_ladder_fills(double *f) {
  for (uint32_t i = 0; i <= 10u; i++) f[i] = (double)i / 10.0;
  for (uint32_t i = 0; i < 10u; i++) f[11u + i] = 0.9 - (double)i / 10.0;
  for (uint32_t i = 0; i < 13u; i++) f[21u + i] = (4.5 + ((i & 1u) ? 0.2 : -0.2)) / 8.0;
}

/** The clip word: a clip, quiet ticks, a clear, quiet, a clip, a clip and a clear on the same tick. */
#define MV_CLIP_COUNT 10u
static const int MV_CLIP_IN[MV_CLIP_COUNT][2] = { /* clip, clear-before */
    {0, 0}, {1, 0}, {0, 0}, {0, 0}, {0, 1}, {0, 0}, {1, 0}, {0, 0}, {1, 1}, {0, 0}};
static inline int mv_clip_tick(struct omx_meter_view *v, int clip, int clear) {
  if (clear) omx_meter_view_clip_clear(v);
  return omx_meter_view_clip(v, clip != 0);
}

/** The fill tables. */
#define MV_FILL_DB_COUNT 10u
static const double MV_FILL_DB[MV_FILL_DB_COUNT] = {-120.0, -60.0001, -60.0, -59.9, -30.0, -12.0, -6.0, -0.5, 0.0, 3.0};
#define MV_FILL_LIN_COUNT 9u
static const double MV_FILL_LIN[MV_FILL_LIN_COUNT] = {0.0, 1e-6, 0.001, 0.0010001, 0.25, 0.5, 0.999, 1.0, 2.0};

#endif /* OMXDSP_TEST_METER_VIEW_SCENARIOS_H */
