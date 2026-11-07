// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/mix/omx_meter_view.h>

#include "fixtures/meter_view_scenarios.h"

/* ---- the meter view law (mix/omx_meter_view.h, spec 7.9), against vectors/meter-view.json, the one file every copy
 * of the law reads. Each case of the file is run again here from the same scenario (fixtures/meter_view_scenarios.h)
 * and must answer the same numbers: the held burst at the nine oracle rates and quanta 32 to 1024 (its peak, fill and
 * marker at each 50 Hz read), the gain-reduction reading over the same grid, the fall at draw rates 5 to 60 Hz, the
 * ladder, the clip word, the fills and the decimation. Beside the file, the law's own closed forms, so a file written
 * from a wrong kernel is red too: the declared numbers are the spec's, the marker falls 18 dB/s of elapsed time and the
 * same at every draw rate, on silence too; the ladder holds one count on an edge; the clip word stays lit until
 * cleared; the reduction holds 300 ms. ---------------------------------------------------------------------------- */

#ifndef OMX_METER_VIEW_VECTORS
#define OMX_METER_VIEW_VECTORS "vectors/meter-view.json"
#endif

/** The number after `"key": ` on the line, or NAN. */
static double mvj_num(const char *line, const char *key) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\": ", key);
  const char *p = strstr(line, pat);
  return p ? strtod(p + strlen(pat), NULL) : NAN;
}

/** The array after `"key": [` on the line into `out` (room `max`); its length, or -1. */
static int mvj_arr(const char *line, const char *key, double *out, int max) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\": [", key);
  const char *p = strstr(line, pat);
  if (!p) return -1;
  p += strlen(pat);
  int n = 0;
  while (*p && *p != ']') {
    char *end;
    const double x = strtod(p, &end);
    if (end == p || n >= max) return -1;
    out[n++] = x;
    p = end;
    while (*p == ',' || *p == ' ') p++;
  }
  return *p == ']' ? n : -1;
}

/* The gain-reduction reading is float arithmetic through powf, omx_db_to_lin and omx_lin_to_db: one libm's
 * powf and log10f differ from another's (glibc 2.36 on bookworm, 2.39 on ubuntu 24.04, 2.41 on fc44) by a float
 * step of the dB value, and the recovery carries it from read to read: measured 2.4e-7 to 2.9e-6 dB against a file
 * written on one of them. The twin declares the same bound (ts/test/tolerances.mjs meterGr); a hold one read off
 * is a whole dB, so every sabotage of the step stays red under it. The held values, the fall, the ladder and the
 * clip word are double arithmetic in one order and stay at 1e-12 or 0. */
#define MV_GR_TOL 1e-4

/** Every entry of `got` within `tol` of `want`: the count that is not. */
static uint32_t mv_off(const double *got, const double *want, uint32_t n, double tol, double *worst) {
  uint32_t bad = 0;
  for (uint32_t i = 0; i < n; i++) {
    const double d = fabs(got[i] - want[i]);
    if (!(d <= tol)) bad++;
    if (d > *worst || d != d) *worst = d;
  }
  return bad;
}

static char *mv_load(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *s = len > 0 ? malloc((size_t)len + 1) : NULL;
  if (s && fread(s, 1, (size_t)len, f) != (size_t)len) {
    free(s);
    s = NULL;
  }
  if (s) s[len] = '\0';
  fclose(f);
  return s;
}

static void mv_file_case(const char *line, uint32_t *seen) {
  static double w[6][MV_MAX_TICKS];
  static struct mv_reads r;
  double worst = 0.0;
  if (strstr(line, "\"set\": \"held\"") || strstr(line, "\"set\": \"gr\"")) {
    const int held = strstr(line, "\"set\": \"held\"") != NULL;
    const float sr = (float)mvj_num(line, "rate");
    const uint32_t q = (uint32_t)mvj_num(line, "quantum");
    if (held) mv_held(sr, q, &r);
    else mv_gr(sr, q, &r);
    const int n0 = mvj_arr(line, held ? "peak" : "deepest", w[0], MV_MAX_TICKS);
    const int n1 = mvj_arr(line, "dt", w[1], MV_MAX_TICKS);
    const int n2 = mvj_arr(line, held ? "fill" : "reading", w[2], MV_MAX_TICKS);
    const int n3 = held ? mvj_arr(line, "marker", w[3], MV_MAX_TICKS) : (int)r.n;
    if (!(n0 == (int)r.n && n1 == n0 && n2 == n0 && n3 == n0 && r.n > 0)) {
      ok(0, held ? "a held case reads as many times as the file" : "a gr case reads as many times as the file", n0, r.n);
      return;
    }
    uint32_t bad = mv_off(r.in, w[0], r.n, 0.0, &worst) + mv_off(r.dt, w[1], r.n, 0.0, &worst);
    bad += mv_off(r.out, w[2], r.n, held ? 1e-12 : MV_GR_TOL, &worst);
    if (held) bad += mv_off(r.marker, w[3], r.n, 1e-12, &worst);
    if (held)
      ok(bad == 0, "held: the peak, fill and marker of every read are the file's", worst, 1e-12);
    else
      ok(bad == 0, "gr: the reading of every read is the file's within MV_GR_TOL", worst, MV_GR_TOL);
    if (!held && fabs(mvj_num(line, "pole") - (double)mv_pole(sr)) != 0.0)
      ok(0, "gr: the pole is the file's", mvj_num(line, "pole"), (double)mv_pole(sr));
    seen[held ? 0 : 1]++;
  } else if (strstr(line, "\"set\": \"fall\"")) {
    const double hz = mvj_num(line, "drawHz"), live = mvj_num(line, "live");
    const uint32_t n = mv_fall(hz, live, w[4]);
    const int m = mvj_arr(line, "marker", w[0], MV_MAX_TICKS);
    ok(m == (int)n && mv_off(w[4], w[0], n, 1e-12, &worst) == 0, "fall: the marker at every draw tick is the file's",
       worst, 1e-12);
    seen[2]++;
  } else if (strstr(line, "\"set\": \"ladder\"")) {
    mv_ladder_fills(w[4]);
    int32_t prev = -1;
    for (uint32_t i = 0; i < MV_LADDER_COUNT; i++)
      w[5][i] = prev = omx_meter_view_ladder(prev, w[4][i], MV_LADDER_STEPS, OMX_METER_VIEW_STEP_HYSTERESIS_DOUBLE);
    const int a = mvj_arr(line, "fill", w[0], MV_MAX_TICKS), b = mvj_arr(line, "count", w[1], MV_MAX_TICKS);
    ok(a == (int)MV_LADDER_COUNT && b == a && mv_off(w[4], w[0], MV_LADDER_COUNT, 0.0, &worst) == 0 &&
           mv_off(w[5], w[1], MV_LADDER_COUNT, 0.0, &worst) == 0,
       "ladder: every count is the file's", worst, 0.0);
    seen[3]++;
  } else if (strstr(line, "\"set\": \"clip\"")) {
    struct omx_meter_view v = {0};
    for (uint32_t i = 0; i < MV_CLIP_COUNT; i++) w[4][i] = mv_clip_tick(&v, MV_CLIP_IN[i][0], MV_CLIP_IN[i][1]);
    const int a = mvj_arr(line, "lit", w[0], MV_MAX_TICKS);
    ok(a == (int)MV_CLIP_COUNT && mv_off(w[4], w[0], MV_CLIP_COUNT, 0.0, &worst) == 0, "clip: the word at every tick is the file's",
       worst, 0.0);
    seen[4]++;
  } else if (strstr(line, "\"set\": \"fillDb\"") || strstr(line, "\"set\": \"fillLinear\"")) {
    const int db = strstr(line, "\"set\": \"fillDb\"") != NULL;
    const int a = mvj_arr(line, db ? "db" : "level", w[0], MV_MAX_TICKS), b = mvj_arr(line, "fill", w[1], MV_MAX_TICKS);
    uint32_t bad = (a < 1 || b != a) ? 1u : 0u;
    for (int i = 0; !bad && i < a; i++) {
      const double f = db ? omx_meter_view_fill_db(w[0][i], OMX_METER_VIEW_FLOOR_DB)
                          : omx_meter_view_fill(w[0][i], OMX_METER_VIEW_FLOOR_DB);
      if (!(fabs(f - w[1][i]) <= 1e-12)) bad++;
    }
    ok(bad == 0, db ? "fillDb: every fill is the file's" : "fillLinear: every fill is the file's", bad, 0);
    seen[5]++;
  } else if (strstr(line, "\"set\": \"decimate\"")) {
    const int a = mvj_arr(line, "kind", w[0], 16), b = mvj_arr(line, "kept", w[1], 16);
    const int c = mvj_arr(line, "value", w[2], 16), d = mvj_arr(line, "out", w[3], 16);
    uint32_t bad = (a < 1 || b != a || c != a || d != a) ? 1u : 0u;
    for (int i = 0; !bad && i < a; i++)
      if (omx_meter_view_decimate((enum omx_meter_kinds)(int)w[0][i], w[1][i], w[2][i]) != w[3][i]) bad++;
    ok(bad == 0, "decimate: every window keeps what the file keeps", bad, 0);
    seen[6]++;
  }
}

static void arm_mix_meter_view(void) {
  g_arm = "mix_meter_view";

  /* the declared numbers are the spec's (7.9): floor -60 dBFS, hysteresis a quarter step, 18 dB/s, 50 Hz, 20 Hz */
  ok(OMX_METER_VIEW_FLOOR_DB == -60, "the floor is -60 dBFS", OMX_METER_VIEW_FLOOR_DB, -60);
  ok(OMX_METER_VIEW_STEP_HYSTERESIS_DOUBLE == 0.25, "the ladder's hysteresis is 0.25 of a step",
     OMX_METER_VIEW_STEP_HYSTERESIS_DOUBLE, 0.25);
  ok(OMX_METER_VIEW_FALL_DB_PER_SECOND == 18, "the marker falls 18 dB/s", OMX_METER_VIEW_FALL_DB_PER_SECOND, 18);
  ok(OMX_METER_VIEW_HOST_READ_HZ == 50 && OMX_METER_VIEW_STREAM_HZ == 20, "the host reads at 50 Hz, the stream is 20 Hz",
     OMX_METER_VIEW_HOST_READ_HZ, 50);

  /* the unit of each kind and measurement */
  static const enum omx_meter_units UNIT_OF[OMX_METER_MEASUREMENTS_COUNT] = {
      OMX_METER_UNITS_DBFS, OMX_METER_UNITS_DBTP, OMX_METER_UNITS_LUFS,
      OMX_METER_UNITS_LUFS, OMX_METER_UNITS_LUFS, OMX_METER_UNITS_LU};
  uint32_t wrong = 0;
  for (uint32_t i = 0; i < OMX_METER_MEASUREMENTS_COUNT; i++) {
    if (omx_meter_view_unit_of(OMX_METER_KINDS_LEVEL, (enum omx_meter_measurements)i) != UNIT_OF[i]) wrong++;
    if (omx_meter_view_unit_of(OMX_METER_KINDS_GAIN_REDUCTION, (enum omx_meter_measurements)i) != OMX_METER_UNITS_DB) wrong++;
  }
  if (omx_meter_view_unit_of((enum omx_meter_kinds)OMX_METER_KINDS_COUNT, OMX_METER_MEASUREMENTS_PEAK) != OMX_METER_UNITS_COUNT) wrong++;
  ok(wrong == 0, "unit: dBFS, dBTP, LUFS, LU for a level's measurements, dB for a reduction, none past the set", wrong, 0);

  /* the fill: linear in dB from the floor to 0 dBFS */
  ok(omx_meter_view_fill_db(-60.0, -60.0) == 0.0 && omx_meter_view_fill_db(-30.0, -60.0) == 0.5 &&
         omx_meter_view_fill_db(0.0, -60.0) == 1.0 && omx_meter_view_fill_db(NAN, -60.0) == 0.0 &&
         omx_meter_view_fill_db(-INFINITY, -60.0) == 0.0,
     "fill: 0 at the floor, 0.5 at -30 dB, 1 at 0 dBFS, 0 for NaN and -inf", omx_meter_view_fill_db(-30.0, -60.0), 0.5);
  ok(fabs(omx_meter_view_fill(0.25, OMX_METER_VIEW_FLOOR_DB) - (60.0 + 20.0 * log10(0.25)) / 60.0) < 1e-15,
     "fill of a linear -12 dBFS peak is about 0.8 of the scale", omx_meter_view_fill(0.25, OMX_METER_VIEW_FLOOR_DB), 0.8);

  /* the fall by elapsed time: the same at every draw rate, the closed form 1 - 18 t / 60, on silence too */
  static double fall[MV_DRAW_COUNT][MV_MAX_TICKS];
  for (int li = 1; li >= 0; li--) { /* silence last: the check after the loop reads it */
    const double live = li ? 0.5 : 0.0;
    double worst = 0.0, gap = 0.0;
    for (uint32_t h = 0; h < MV_DRAW_COUNT; h++) {
      const uint32_t n = mv_fall(MV_DRAW_HZ[h], live, fall[h]);
      for (uint32_t k = 0; k < n; k++) {
        const double t = (double)k / MV_DRAW_HZ[h], want = fmax(live, 1.0 - 18.0 * t / 60.0);
        if (fabs(fall[h][k] - want) > worst) worst = fabs(fall[h][k] - want);
      }
    }
    for (uint32_t s = 0; s <= 20u; s++) /* every 0.2 s, a tick of every draw rate */
      for (uint32_t h = 1; h < MV_DRAW_COUNT; h++) {
        const uint32_t k0 = (uint32_t)lround(0.2 * s * MV_DRAW_HZ[0]), k = (uint32_t)lround(0.2 * s * MV_DRAW_HZ[h]);
        if (fabs(fall[h][k] - fall[0][k0]) > gap) gap = fabs(fall[h][k] - fall[0][k0]);
      }
    ok(worst < 1e-12, li ? "fall: over a steady -30 dBFS the marker falls 18 dB/s to it and stays"
                         : "fall: on silence the marker falls 18 dB/s of elapsed time to empty",
       worst, 1e-12);
    ok(gap < 1e-12, li ? "fall: the same marker at draw rates 5, 20, 30 and 60 Hz, over a steady level"
                       : "fall: the same marker at draw rates 5, 20, 30 and 60 Hz, on silence",
       gap, 1e-12);
  }
  ok(fall[3][1] < 1.0 && fall[3][201] == 0.0, "fall: silence moves the marker at the first tick and empties it by 3.4 s",
     fall[3][1], 1.0);

  /* a louder value is taken at once and held as the marker */
  struct omx_meter_view v = {0};
  double m = omx_meter_view_step(&v, 0.4, 0.05, OMX_METER_VIEW_FALL_DB_PER_SECOND, OMX_METER_VIEW_FLOOR_DB);
  m = omx_meter_view_step(&v, 0.9, 0.05, OMX_METER_VIEW_FALL_DB_PER_SECOND, OMX_METER_VIEW_FLOOR_DB);
  ok(m == 0.9, "a louder fill is the marker at once", m, 0.9);
  m = omx_meter_view_step(&v, 0.2, 0.5, OMX_METER_VIEW_FALL_DB_PER_SECOND, OMX_METER_VIEW_FLOOR_DB);
  ok(fabs(m - (0.9 - 0.15)) < 1e-12, "half a second later the marker is 9 dB lower", m, 0.75);

  /* the ladder holds one count on an edge; a bar has no release */
  static double f[MV_LADDER_COUNT];
  mv_ladder_fills(f);
  int32_t prev = -1, c0 = -1;
  uint32_t moves = 0;
  for (uint32_t i = 0; i < MV_LADDER_COUNT; i++) {
    prev = omx_meter_view_ladder(prev, f[i], MV_LADDER_STEPS, OMX_METER_VIEW_STEP_HYSTERESIS_DOUBLE);
    if (i == 21u) c0 = prev;
    if (i > 21u && prev != c0) moves++;
  }
  ok(moves == 0, "ladder: a level that wobbles 0.2 of a step on an edge holds one count", moves, 0);
  ok(omx_meter_view_bar_step(8, 0.0, 8u) == 0 && omx_meter_view_bar_step(-1, 1.0, 8u) == 8,
     "bar: full to silence in one tick, no release", omx_meter_view_bar_step(8, 0.0, 8u), 0);

  /* the clip word stays lit until cleared */
  struct omx_meter_view cv = {0};
  uint32_t lit = 0;
  omx_meter_view_clip(&cv, true);
  for (uint32_t i = 0; i < 100u; i++) lit += omx_meter_view_clip(&cv, false);
  ok(lit == 100u, "clip: lit on every quiet tick after a clip", lit, 100);
  omx_meter_view_clip_clear(&cv);
  ok(!omx_meter_view_clip(&cv, false), "clip: dark once cleared", cv.clip, 0);

  /* the reduction holds 300 ms, then recovers, at every rate and quantum */
  static struct mv_reads r;
  uint32_t unheld = 0, stuck = 0;
  for (uint32_t ri = 0; ri < MV_RATES_COUNT; ri++)
    for (uint32_t qi = 0; qi < MV_QUANTA_COUNT; qi++) {
      mv_gr(MV_RATES[ri], MV_QUANTA[qi], &r);
      double t = 0.0, last = -4.0;
      for (uint32_t k = 0; k < r.n; k++) {
        t += r.dt[k];
        if (t > 0.18 && t < 0.42 && r.out[k] != -12.0) unheld++;
        if (t > 1.0 && !(r.out[k] > -4.0 && r.out[k] >= last && r.out[k] <= 0.0)) stuck++;
        if (t > 1.0) last = r.out[k];
      }
    }
  ok(unheld == 0, "gr: the -12 dB reading holds 300 ms after the block, at every rate and quantum", unheld, 0);
  ok(stuck == 0, "gr: once the hold is spent the reading recovers toward 0 dB, never past it", stuck, 0);

  /* the file: every case run again, the same numbers */
  char *s = mv_load(OMX_METER_VIEW_VECTORS);
  ok(s != NULL, "the vector file " OMX_METER_VIEW_VECTORS " reads", 0, 1);
  if (!s) return;
  ok(mvj_num(s, "floorDb") == OMX_METER_VIEW_FLOOR_DB && mvj_num(s, "fallDbPerSecond") == OMX_METER_VIEW_FALL_DB_PER_SECOND &&
         mvj_num(s, "stepHysteresis") == OMX_METER_VIEW_STEP_HYSTERESIS_DOUBLE &&
         mvj_num(s, "hostReadHz") == OMX_METER_VIEW_HOST_READ_HZ && mvj_num(s, "grHoldMs") == OMX_METER_GR_HOLD_MS,
     "the file's law is the declared one", mvj_num(s, "fallDbPerSecond"), OMX_METER_VIEW_FALL_DB_PER_SECOND);
  uint32_t seen[7] = {0};
  for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) mv_file_case(line, seen);
  free(s);
  ok(seen[0] == MV_RATES_COUNT * MV_QUANTA_COUNT && seen[1] == MV_RATES_COUNT * MV_QUANTA_COUNT,
     "the file holds a held and a gr case for every oracle rate and quantum", seen[0] + seen[1],
     2 * MV_RATES_COUNT * MV_QUANTA_COUNT);
  ok(seen[2] == 2u * MV_DRAW_COUNT && seen[3] == 1u && seen[4] == 1u && seen[5] == 2u && seen[6] == 1u,
     "the file holds the fall at every draw rate, the ladder, the clip, the fills and the decimation", seen[2], 8);
}
