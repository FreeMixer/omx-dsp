// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * eq_bandwidth_q_table.c — omx_eq_bandwidth_q(), the one bandwidth-to-Q law, as a table
 * (FreeMixer/omx-dsp#41): every row `[sample_rate, freq_hz, bw_oct, q]` at the nine oracle rates,
 * centres from below the clamp's floor to past Nyquist, widths from a tenth of an octave to four.
 * the console's TypeScript copy (core's bandwidthOctavesQ) is held to it until it is deleted;
 * tools/ts-golden.mjs bandwidth-q runs that check from here.
 *
 *   eq_bandwidth_q_table                       prints the JSON (`make golden-write` writes the golden with it);
 *                                              %.17g reads back as the same double in C and in JavaScript
 *   eq_bandwidth_q_table --check GOLDEN [TABLE] holds the computed table (or TABLE, a file of the same form)
 *                                              to GOLDEN: the inputs exactly, each q within OMX_Q_TABLE_ULPS
 *   eq_bandwidth_q_table --nudge N TABLE       prints TABLE with one q moved N units in the last place
 *                                              (`make bandwidth-q-sabotage` proves the bound with it)
 *
 * Why q is compared within a bound and not bit for bit: the law goes through sin() and sinh(), and glibc
 * does not round its transcendental functions correctly, so two libm versions give a q that differs in its
 * last digits (Fedora 44's glibc 2.43 and Fedora 45's 2.44 differ by up to 2 ULP on this table). The bound is
 * 4 ULP: twice what was measured, and still a relative difference near 1e-15, far below anything the law
 * could get wrong and be noticed. A q of zero (a centre past Nyquist) must stay exactly zero.
 */
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OMX_Q_TABLE_ULPS 4
#define OMX_Q_TABLE_MAX_ROWS 512

static const double freqs[] = {0.5, 20.0, 100.0, 1000.0, 4000.0, 8000.0, 12000.0, 15000.0, 20000.0, 30000.0, 96000.0};
static const double widths[] = {0.1, 0.5, 1.0, 2.0, 4.0};

typedef struct { double v[4]; } row_t;

static unsigned compute(row_t *rows) {
  static const float rates[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;
  unsigned n = 0;
  for (unsigned r = 0; r < OMX_ORACLE_FULL_RATES_COUNT; r++)
    for (unsigned f = 0; f < sizeof freqs / sizeof freqs[0]; f++)
      for (unsigned w = 0; w < sizeof widths / sizeof widths[0]; w++) {
        const double sr = rates[r];
        rows[n++] = (row_t){{sr, freqs[f], widths[w], omx_eq_bandwidth_q(freqs[f], widths[w], sr)}};
      }
  return n;
}

static void print(const row_t *rows, unsigned n) {
  const char *sep = "";
  printf("{\n  \"law\": \"omx_eq_bandwidth_q\",\n  \"source\": \"include/omxdsp/omx_eq_design.h\",\n");
  printf("  \"columns\": [\"sampleRate\", \"freqHz\", \"bwOct\", \"q\"],\n  \"rows\": [");
  for (unsigned i = 0; i < n; i++) {
    printf("%s\n    [%.17g, %.17g, %.17g, %.17g]", sep, rows[i].v[0], rows[i].v[1], rows[i].v[2], rows[i].v[3]);
    sep = ",";
  }
  printf("\n  ]\n}\n");
}

/* the rows of a table this program printed; -1 when the file cannot be read or holds no row */
static int load(const char *path, row_t *rows) {
  FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
  char line[512];
  int n = 0;
  if (!f) { fprintf(stderr, "eq_bandwidth_q_table: cannot read %s\n", path); return -1; }
  while (fgets(line, sizeof line, f)) {
    char *p = line, *end;
    while (*p == ' ') p++;
    if (*p != '[') continue;
    p++;
    if (n == OMX_Q_TABLE_MAX_ROWS) { fprintf(stderr, "eq_bandwidth_q_table: %s has too many rows\n", path); n = -1; break; }
    for (int c = 0; c < 4; c++) {
      rows[n].v[c] = strtod(p, &end);
      if (end == p) { fprintf(stderr, "eq_bandwidth_q_table: %s: unreadable row %d\n", path, n + 1); n = -1; break; }
      p = end;
      while (*p == ',' || *p == ' ') p++;
    }
    if (n < 0) break;
    n++;
  }
  if (f != stdin) fclose(f);
  if (n == 0) fprintf(stderr, "eq_bandwidth_q_table: %s holds no row\n", path);
  return n > 0 ? n : -1;
}

/* how many doubles lie between a and b (0 when equal); UINT64_MAX across a sign or for a NaN */
static uint64_t ulps(double a, double b) {
  int64_t ia, ib;
  if (a == b) return 0;
  if (isnan(a) || isnan(b) || signbit(a) != signbit(b)) return UINT64_MAX;
  memcpy(&ia, &a, sizeof ia);
  memcpy(&ib, &b, sizeof ib);
  return ia > ib ? (uint64_t)(ia - ib) : (uint64_t)(ib - ia);
}

static int check(const char *golden, const char *table) {
  static row_t want[OMX_Q_TABLE_MAX_ROWS], got[OMX_Q_TABLE_MAX_ROWS];
  const int nw = load(golden, want);
  const int ng = table ? load(table, got) : (int)compute(got);
  uint64_t worst = 0;
  int bad = 0;
  if (nw < 0 || ng < 0) return 2;
  if (nw != ng) { printf("bandwidth-q-table: %d rows, the golden %d\n", ng, nw); return 1; }
  for (int i = 0; i < nw; i++) {
    const double *w = want[i].v, *g = got[i].v;
    if (w[0] != g[0] || w[1] != g[1] || w[2] != g[2]) {
      printf("row %d: inputs [%.17g, %.17g, %.17g], the golden [%.17g, %.17g, %.17g]\n", i + 1, g[0], g[1], g[2], w[0], w[1], w[2]);
      bad++;
      continue;
    }
    const uint64_t d = ulps(w[3], g[3]);
    if (d > worst) worst = d;
    if (d > OMX_Q_TABLE_ULPS || (w[3] == 0.0) != (g[3] == 0.0)) {
      printf("row %d [%.17g, %.17g, %.17g]: q %.17g, the golden %.17g (%llu ULP apart, bound %d)\n", i + 1, g[0], g[1], g[2], g[3], w[3],
             (unsigned long long)d, OMX_Q_TABLE_ULPS);
      bad++;
    }
  }
  if (bad) { printf("bandwidth-q-table: %d of %d rows differ from %s\n", bad, nw, golden); return 1; }
  printf("bandwidth-q-table: %d rows within %d ULP of %s (largest difference %llu ULP)\n", nw, OMX_Q_TABLE_ULPS, golden,
         (unsigned long long)worst);
  return 0;
}

static int nudge(long n, const char *table) {
  static row_t rows[OMX_Q_TABLE_MAX_ROWS];
  const int nr = load(table, rows);
  if (nr < 7) return 2;
  /* row 7 (32000 Hz, 20 Hz, half an octave) has a q well away from zero and from the clamps */
  for (long k = 0; k < (n < 0 ? -n : n); k++) rows[6].v[3] = nextafter(rows[6].v[3], n < 0 ? 0.0 : INFINITY);
  print(rows, (unsigned)nr);
  return 0;
}

int main(int argc, char **argv) {
  static row_t rows[OMX_Q_TABLE_MAX_ROWS];
  if (argc >= 3 && argc <= 4 && strcmp(argv[1], "--check") == 0) return check(argv[2], argc == 4 ? argv[3] : NULL);
  if (argc == 4 && strcmp(argv[1], "--nudge") == 0) return nudge(strtol(argv[2], NULL, 10), argv[3]);
  if (argc != 1) {
    fprintf(stderr, "usage: eq_bandwidth_q_table [--check GOLDEN [TABLE] | --nudge N TABLE]\n");
    return 2;
  }
  print(rows, compute(rows));
  return 0;
}
