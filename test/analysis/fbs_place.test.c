/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * fbs_place.test.c — the feedback corrector's placement in C (omx_fbs_place.h) held to the shared vector file,
 * test/analysis/fixtures/fbs_place.vectors, which the TypeScript twin's test (ts/test/fbs-place.test.mjs) reads
 * too. Every line of every vector is judged, inside
 * the tolerances the file states; test/analysis/fbs-place-sabotage.sh breaks each rule once in a copy of
 * src/omx_fbs_place.c and requires this program to fail.
 *
 *   build/analysis_fbs_place [<vectors>]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/analysis/omx_fbs_place.h>
#include <omxdsp/omx_curve.h>

#define VECTORS "test/analysis/fixtures/fbs_place.vectors"
#define MAXW 64

static int checks, failures, vectors;
static double tol_gain = -1.0, tol_cents = -1.0;

static double num(const char *w) { return strcmp(w, "nan") == 0 ? NAN : strtod(w, NULL); }
static int near(double a, double b, double tol) { return isnan(b) ? isnan(a) : fabs(a - b) <= tol; }

static int origin_of(const char *w) {
  for (unsigned i = 0; i < OMX_CURVE_ORIGINS_COUNT; i++)
    if (strcmp(OMX_CURVE_ORIGIN_WORDS[i], w) == 0) return (int)i;
  return -1;
}
static int shape_of(const char *w) {
  for (unsigned i = 0; i < OMX_CURVE_SHAPES_COUNT; i++)
    if (strcmp(OMX_CURVE_SHAPE_WORDS[i], w) == 0) return (int)i;
  return -1;
}
static int action_of(const char *w) {
  for (int i = 0; i < OMX_FBS_ACTIONS_COUNT; i++)
    if (strcmp(OMX_FBS_ACTION_WORDS[i], w) == 0) return i;
  return -1;
}

static void check(int ok, const char *vector, const char *line, const char *got) {
  checks++;
  if (ok) return;
  failures++;
  printf("FAIL %s: %s: read %s\n", vector, line, got);
}

/* The rings of a tick line from word `from`: c <hz> <depth> and s <hz>. */
static OmxFbsCandidate conf[MAXW];
static double snap[MAXW];
static uint32_t n_conf, n_snap;
static int rings(char **w, int n, int from) {
  n_conf = n_snap = 0;
  for (int i = from; i < n;) {
    if (strcmp(w[i], "c") == 0 && i + 2 < n) {
      memset(&conf[n_conf], 0, sizeof(conf[n_conf]));
      conf[n_conf].freq_hz = num(w[i + 1]);
      conf[n_conf].depth_db = num(w[i + 2]);
      n_conf++;
      i += 3;
    } else if (strcmp(w[i], "s") == 0 && i + 1 < n) {
      snap[n_snap++] = num(w[i + 1]);
      i += 2;
    } else
      return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : VECTORS;
  FILE *f = fopen(path, "r");
  if (!f) {
    printf("FAIL fbs_place: no vector file at %s\n", path);
    return 1;
  }
  static OmxFbsPlace p;
  char name[128] = "";
  char raw[2048], line[2048];
  uint32_t seen = 0;
  int open = 0;
  while (fgets(raw, sizeof(raw), f)) {
    char *hash = strchr(raw, '#');
    if (hash) *hash = '\0';
    raw[strcspn(raw, "\r\n")] = '\0';
    strcpy(line, raw);
    char *w[MAXW];
    int n = 0;
    for (char *t = strtok(raw, " \t"); t && n < MAXW; t = strtok(NULL, " \t")) w[n++] = t;
    if (n == 0) continue;
    char got[512] = "";
    if (strcmp(w[0], "tolerance") == 0 && n == 3) {
      if (strcmp(w[1], "gain") == 0) tol_gain = num(w[2]);
      if (strcmp(w[1], "cents") == 0) tol_cents = num(w[2]);
    } else if (strcmp(w[0], "vector") == 0 && n == 2) {
      snprintf(name, sizeof(name), "%s", w[1]);
      omx_fbs_place_init(&p, OMX_FBS_POOL_LIVE);
      seen = 0;
      open = 1;
      vectors++;
    } else if (strcmp(w[0], "end") == 0) {
      open = 0;
    } else if (!open) {
      check(0, "fbs_place", line, "a line outside a vector");
    } else if (strcmp(w[0], "pool") == 0 && n == 2) {
      omx_fbs_place_set_pool(&p, strcmp(w[1], "fixed") == 0 ? OMX_FBS_POOL_FIXED : OMX_FBS_POOL_LIVE);
    } else if (strcmp(w[0], "set") == 0 && n == 6) {
      omx_fbs_place_set(&p, strcmp(w[1], "never") == 0 ? OMX_FBS_HOLD_NEVER : num(w[1]), num(w[2]), num(w[3]),
                        num(w[4]), num(w[5]));
    } else if (strcmp(w[0], "config") == 0 && n == 6) {
      const int hold_ok = strcmp(w[1], "never") == 0 ? p.hold_s == OMX_FBS_HOLD_NEVER : p.hold_s == num(w[1]);
      snprintf(got, sizeof(got), "%g %u %g %g %g", p.hold_s, p.max_bands, p.notch_q, p.merge_cents, p.cooldown_s);
      check(hold_ok && p.max_bands == (uint32_t)num(w[2]) && p.notch_q == num(w[3]) && p.merge_cents == num(w[4]) &&
                p.cooldown_s == num(w[5]),
            name, line, got);
    } else if (strcmp(w[0], "tick") == 0 && n >= 2 && rings(w, n, 2)) {
      omx_fbs_place_tick(&p, num(w[1]), snap, n_snap, conf, n_conf);
    } else if (strcmp(w[0], "ticks") == 0 && n >= 4 && rings(w, n, 4)) {
      for (double t = num(w[1]); t <= num(w[2]); t += num(w[3])) omx_fbs_place_tick(&p, t, snap, n_snap, conf, n_conf);
    } else if (strcmp(w[0], "advance") == 0 && n == 2) {
      omx_fbs_place_advance(&p, num(w[1]));
    } else if (strcmp(w[0], "slot") == 0 && n == 8) {
      const OmxBandReport r = omx_fbs_place_report(&p, (uint32_t)num(w[1]));
      snprintf(got, sizeof(got), "%.17g %.17g %.17g %g %s %s", r.freq_hz, r.depth_db, r.q, r.on,
               OMX_CURVE_ORIGIN_WORDS[(int)r.origin], OMX_CURVE_SHAPE_WORDS[(int)r.shape]);
      /* the report words are floats: a value is the float of the vector's value, a gain within the tolerance */
      check(r.freq_hz == (float)num(w[2]) && near(r.depth_db, (float)num(w[3]), tol_gain + 1e-6 * fabs(num(w[3]))) &&
                r.q == (float)num(w[4]) && r.on == (float)num(w[5]) && (int)r.origin == origin_of(w[6]) &&
                (int)r.shape == shape_of(w[7]),
            name, line, got);
      /* and the placement's own double, within the stated tolerance */
      const uint32_t k = (uint32_t)num(w[1]);
      snprintf(got, sizeof(got), "slot %u holds %.17g %.17g %.17g", k, p.slot[k].freq_hz, p.slot[k].gain_db, p.slot[k].q);
      check(k < OMX_FBS_PLACE_SLOTS && p.slot[k].freq_hz == num(w[2]) && near(p.slot[k].gain_db, num(w[3]), tol_gain) &&
                p.slot[k].q == num(w[4]),
            name, line, got);
    } else if (strcmp(w[0], "free") == 0 && n == 2) {
      const OmxBandReport r = omx_fbs_place_report(&p, (uint32_t)num(w[1]));
      snprintf(got, sizeof(got), "%g %g %g %g %g %g", r.freq_hz, r.depth_db, r.q, r.on, r.origin, r.shape);
      check(r.on == 0.0f && r.freq_hz == 0.0f && r.depth_db == 0.0f && r.q == 0.0f &&
                (int)r.origin == OMX_CURVE_ORIGINS_USER && (int)r.shape == OMX_CURVE_SHAPES_OFF,
            name, line, got);
    } else if (strcmp(w[0], "count") == 0 && n == 2) {
      snprintf(got, sizeof(got), "%u", omx_fbs_place_count(&p));
      check(omx_fbs_place_count(&p) == (uint32_t)num(w[1]), name, line, got);
    } else if (strcmp(w[0], "log") == 0 && (n - 1) % 3 == 0) {
      int ok = (uint32_t)((n - 1) / 3) == p.n_log - seen && p.log_dropped == 0;
      size_t at = 0;
      for (uint32_t e = seen; e < p.n_log && at < sizeof(got) - 64; e++)
        at += (size_t)snprintf(got + at, sizeof(got) - at, "%s %.17g %d ", OMX_FBS_ACTION_WORDS[p.log[e].action],
                               p.log[e].freq_hz, p.log[e].slot);
      for (int i = 1; ok && i < n; i += 3) {
        const OmxFbsPlaceEvent *e = &p.log[seen + (uint32_t)((i - 1) / 3)];
        ok = e->action == action_of(w[i]) && e->freq_hz == num(w[i + 1]) && e->slot == (int)num(w[i + 2]);
      }
      check(ok, name, line, got);
      seen = p.n_log;
    } else if (strcmp(w[0], "cap") == 0 && n == 4) {
      const double held = omx_fbs_place_gain_cap(origin_of(w[1]), num(w[2]));
      snprintf(got, sizeof(got), "%.17g", held);
      check(origin_of(w[1]) >= 0 && near(held, num(w[3]), 0.0), name, line, got);
    } else if (strcmp(w[0], "cents") == 0 && n == 4) {
      const double c = omx_fbs_place_cents(num(w[1]), num(w[2]));
      snprintf(got, sizeof(got), "%.17g", c);
      check(near(c, num(w[3]), tol_cents), name, line, got);
    } else {
      check(0, name, line, "a line this test does not know");
    }
  }
  fclose(f);
  check(tol_gain > 0.0 && tol_cents > 0.0, "fbs_place", "the file states its tolerances", "none");
  check(vectors >= 15, "fbs_place", "the file carries every vector", "fewer");
  printf("fbs_place: %d checks over %d vectors, %d failures\n", checks, vectors, failures);
  return failures ? 1 : 0;
}
