// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * eq_default_bands.c — THE default EQ rule's C spelling (omx_eq_defaults.h) against its JavaScript
 * spelling (tools/eq-defaults-golden.mjs, the rule omx-contract carried until 3.0.0), the oracle
 * omx-contract's test/eq-defaults.test.mjs was, moved beside the functions:
 *   - for every count 1..62 the golden lists (test/golden/eq_default_bands.txt), the centres equal
 *     the golden's at float, bit for bit, and the types equal its types; a count the golden answers
 *     `none` returns 0;
 *   - every count EQ_BAND_COUNTS declares (each strip type's default and max) has an answer;
 *   - the answers the operator ruled (2026-10-08), stated here on their own: four bands at 100, 400,
 *     2000 and 8000 Hz, a low shelf, two bells, a high shelf; eight bands on R10; sixteen on R10;
 *     thirty-two needing R20; no answer for 0 bands or for more than R20 holds.
 *
 *   eq_default_bands <golden>     (make eq-default-bands)
 * Exit 0 every check holds, 1 one failed (each printed), 2 the golden cannot be read.
 */
#include <omxdsp/omx_eq_defaults.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 64u

static unsigned g_checks, g_failed;

static void ok(int cond, unsigned n, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%u bands] %s\n", n, what);
  }
}

static int type_of(const char *s) {
  if (!strcmp(s, "lowShelf")) return OMX_EQ_BAND_TYPES_LOW_SHELF;
  if (!strcmp(s, "highShelf")) return OMX_EQ_BAND_TYPES_HIGH_SHELF;
  if (!strcmp(s, "bell")) return OMX_EQ_BAND_TYPES_BELL;
  return -1;
}

static int on_series(float f, const double *series, unsigned n) {
  for (unsigned k = 0; k < n; k++)
    if ((float)series[k] == f) return 1;
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: eq_default_bands <golden>\n"); return 2; }
  FILE *g = fopen(argv[1], "r");
  if (!g) { fprintf(stderr, "eq_default_bands: cannot read %s\n", argv[1]); return 2; }
  static const double r10[] = OMX_ISO_THIRD_OCTAVE_CENTRES_HZ_INIT;
  int answered[MAXN] = {0};
  unsigned lines = 0;
  char line[4096];
  while (fgets(line, sizeof line, g)) {
    char *tok = strtok(line, " \n");
    const unsigned n = (unsigned)strtoul(tok, NULL, 10);
    if (n != ++lines || n >= MAXN) { fprintf(stderr, "eq_default_bands: line %u counts %u bands\n", lines, n); return 2; }
    float out[MAXN];
    const unsigned r = omx_eq_default_centres(n, out);
    tok = strtok(NULL, " \n");
    if (!strcmp(tok, "none")) {
      ok(r == 0, n, "no answer where the golden has none");
      continue;
    }
    ok(r == n, n, "an answer where the golden has one");
    answered[n] = r == n;
    for (unsigned i = 0; i < n; i++, tok = strtok(NULL, " \n")) {
      char *colon = tok ? strchr(tok, ':') : NULL;
      if (!colon) { fprintf(stderr, "eq_default_bands: %u bands: band %u missing\n", n, i); return 2; }
      *colon = '\0';
      char what[96];
      snprintf(what, sizeof what, "band %u: type %s", i, tok);
      ok(omx_eq_default_type(i, n) == type_of(tok), n, what);
      const float want = (float)strtod(colon + 1, NULL);
      snprintf(what, sizeof what, "band %u: centre %.9g, the golden's %.9g", i, (double)out[i], (double)want);
      ok(r == n && out[i] == want, n, what);
    }
    ok(tok == NULL, n, "the golden lists no more bands than the count");
  }
  fclose(g);
  ok(lines == 62, lines, "the golden holds 1..62 bands");

  static const unsigned declared[] = {
      OMX_EQ_BAND_COUNTS_CHANNEL_DEFAULT, OMX_EQ_BAND_COUNTS_CHANNEL_MAX, OMX_EQ_BAND_COUNTS_STRIP_DEFAULT,
      OMX_EQ_BAND_COUNTS_STRIP_MAX,       OMX_EQ_BAND_COUNTS_EQ8_DEFAULT, OMX_EQ_BAND_COUNTS_EQ8_MAX,
      OMX_EQ_BAND_COUNTS_EQ16_DEFAULT,    OMX_EQ_BAND_COUNTS_EQ16_MAX,    OMX_EQ_BAND_COUNTS_EQ32_DEFAULT,
      OMX_EQ_BAND_COUNTS_EQ32_MAX};
  for (unsigned d = 0; d < sizeof declared / sizeof declared[0]; d++)
    ok(declared[d] < MAXN && answered[declared[d]], declared[d], "a declared EQ_BAND_COUNTS count has its default bands");

  float c[MAXN];
  ok(omx_eq_default_centres(4, c) == 4 && c[0] == 100.0f && c[1] == 400.0f && c[2] == 2000.0f && c[3] == 8000.0f, 4,
     "four bands start at 100, 400, 2000 and 8000 Hz");
  ok(omx_eq_default_type(0, 4) == OMX_EQ_BAND_TYPES_LOW_SHELF && omx_eq_default_type(1, 4) == OMX_EQ_BAND_TYPES_BELL &&
         omx_eq_default_type(2, 4) == OMX_EQ_BAND_TYPES_BELL && omx_eq_default_type(3, 4) == OMX_EQ_BAND_TYPES_HIGH_SHELF,
     4, "four bands: a low shelf, two bells, a high shelf");
  ok((float)OMX_EQ_BAND_DEFAULTS_GAIN_DB == 0.0f && (float)OMX_EQ_BAND_DEFAULTS_Q == 1.0f, 4, "every band flat, Q 1");
  static const float eight[] = {31.5f, 80, 160, 400, 1000, 2500, 5000, 12500};
  ok(omx_eq_default_centres(8, c) == 8 && !memcmp(c, eight, sizeof eight), 8, "eight bands: 31.5, 80, 160, 400, 1k, 2.5k, 5k, 12.5k");
  int all_r10 = omx_eq_default_centres(16, c) == 16;
  for (unsigned i = 0; all_r10 && i < 16; i++) all_r10 = on_series(c[i], r10, OMX_ISO_THIRD_OCTAVE_CENTRES_HZ_COUNT);
  ok(all_r10, 16, "sixteen bands sit on R10");
  int some_r20 = 0;
  if (omx_eq_default_centres(32, c) == 32)
    for (unsigned i = 0; i < 32; i++) some_r20 |= !on_series(c[i], r10, OMX_ISO_THIRD_OCTAVE_CENTRES_HZ_COUNT);
  ok(some_r20, 32, "thirty-two bands need the R20 refinement");
  for (unsigned n = 1; n <= 40; n++) {
    if (omx_eq_default_centres(n, c) != n) continue;
    int fine = 1;
    for (unsigned i = 0; i < n; i++) fine &= c[i] >= (float)OMX_EQ_FREQ_RANGE_MIN && c[i] <= (float)OMX_EQ_FREQ_RANGE_MAX && (i == 0 || c[i] > c[i - 1]);
    ok(fine, n, "ascending, distinct, inside EQ_FREQ_RANGE");
  }
  ok(omx_eq_default_centres(0, c) == 0, 0, "no answer for 0 bands");
  ok(omx_eq_default_centres(OMX_EQ_CENTRE_SERIES_R20_HZ_COUNT + 1, c) == 0, OMX_EQ_CENTRE_SERIES_R20_HZ_COUNT + 1,
     "no answer for more bands than R20 holds");
  ok(omx_eq_default_type(0, 1) == OMX_EQ_BAND_TYPES_BELL, 1, "a single band is a bell");
  ok(omx_eq_default_type(0, 2) == OMX_EQ_BAND_TYPES_LOW_SHELF && omx_eq_default_type(1, 2) == OMX_EQ_BAND_TYPES_HIGH_SHELF, 2,
     "two bands: a low shelf and a high shelf");

  printf("eq-default-bands: %u checks, %u failed (1..%u bands against the golden)\n", g_checks, g_failed, lines);
  return g_failed ? 1 : 0;
}
